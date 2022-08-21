// #define FDEBUG
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "fcntl.h"
#include "defs.h"

// 为了用一个 struct file.....
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

#include "dbg_macros.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  int bad = 0;
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if ((r_scause() == 13 || r_scause() == 15)){
    try(mmap_fault_handler(r_stval()), bad = 1)
  }
  else if((which_dev = devintr()) != 0){
    // ok
  } else{
    bad = 1;
  }

  if (bad){
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

struct mmap_vam* 
get_vma_by_addr(uint64 addr){
// 接收一个地址，判断这个地址属于哪个 vma
  struct proc* p = myproc();
  for(int i = 0; i < VMA_SZ; i++){
    if(p->mmap_vams[i].in_use && addr >= p->mmap_vams[i].sta_addr && addr < p->mmap_vams[i].sta_addr + p->mmap_vams[i].sz){
      return p->mmap_vams + i;
    }
  }
  return 0;
}

int
mmap_fault_handler(uint64 addr){
  struct proc* p = myproc();
  struct mmap_vma* cur_vma;
  if((cur_vma = get_vma_by_addr(addr)) == 0){
    return -1;
  }

  if(!cur_vma->file->readable && r_scause() == 13 && cur_vma->flags & MAP_SHARED){
    DEBUG("mmap_fault_handler: not readable\n");
    return -1;
  } // 读错误
    
  if(!cur_vma->file->writable && r_scause() == 15 && cur_vma->flags & MAP_SHARED){
    DEBUG("mmap_fault_handler: not writable\n");
    return -1;
  }
    

  uint64 pg_sta = PGROUNDDOWN(addr);
  uint64 pa = kalloc();
  if(!pa){
    DEBUG("mmap_fault_handler: kalloc failed\n");
    return -1;
  }
  memset(pa, 0, PGSIZE);

  int perm = PTE_U | PTE_V;
  if(cur_vma->prot & PROT_READ) perm |= PTE_R;
  if(cur_vma->prot & PROT_WRITE) perm |= PTE_W;
  if(cur_vma->prot& PROT_EXEC) perm |= PTE_X;
  // 在 mmap 的时候已经排除了不可能的情况了

  uint64 off = PGROUNDDOWN(addr - cur_vma->sta_addr); // 因为不是从 addr 开始拷贝，所以也要 PGROUNDOWN
  // off 代表当前位置超出了其实位置的几倍 PGSIZE


  ilock(cur_vma->file->ip);
  int rdret;
  if((rdret = readi(cur_vma->file->ip, 0, pa, off, PGSIZE)) == 0){
    DEBUG("mmap_fault_handler: readi fail\n");
    iunlock(cur_vma->file->ip);
    return -1;
  }

  iunlock(cur_vma->file->ip); // 没有 put 是这个文件之后还需要使用
                              // 在 unmap 中应该可以 put
  mappages(p->pagetable, pg_sta, PGSIZE, pa, perm);
  return 0;
}