// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
// #define FDEBUG
#include "dbg_macros.h"


void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

#define STEAL_CNT 64


struct {
  struct spinlock lock, stlk;
  struct run *freelist;
  uint64 st_ret[STEAL_CNT];
} kmems[NCPU];

const uint name_sz = sizeof("kmem cpu 0");
char kmem_lk_n[NCPU][sizeof("kmem cpu 0")];

void
kinit() // 单个核心执行
{ 
  for(int i = 0; i < NCPU; i++){
    snprintf(kmem_lk_n[i], name_sz, "kmem cpu %d", i);
    initlock(&kmems[i].lock, kmem_lk_n[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  push_off();
  uint cpu = cpuid();
  pop_off();
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;
  acquire(&kmems[cpu].lock);
  r->next = kmems[cpu].freelist;
  kmems[cpu].freelist = r;
  release(&kmems[cpu].lock);
  
}



int steal(uint cpu){
  uint st_left = STEAL_CNT;
  int idx = 0; 

  memset(kmems[cpu].st_ret, 0, sizeof(kmems[cpu].st_ret));
  DEBUG("\n\ncur cpu %d\n", cpu);
  for(int i = 0; i < NCPU; i++){
    if(i == cpu)  continue;
    acquire(&kmems[i].lock);

    while(kmems[i].freelist && st_left){  
      kmems[cpu].st_ret[idx++] = kmems[i].freelist; // 
      kmems[i].freelist = kmems[i].freelist->next;  // 顺序不能换 
      DEBUG("stealed cpu %d\n", i);
      st_left--;
    }

    release(&kmems[i].lock);
    if(st_left == 0) {
      break;
    }
  }
  DEBUG("steal 成功，cpu=%d, idx=%d\n", cpu, idx);
  return idx;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r = 0;
  
  push_off();
  uint cpu = cpuid();   
  acquire(&kmems[cpu].lock);
  r = kmems[cpu].freelist;
  if(r){
    kmems[cpu].freelist = r->next;
    release(&kmems[cpu].lock);
    } else {
    release(&kmems[cpu].lock);
    int ret = steal(cpu); // steal 过程中不可能 kfree，因为关闭中断
    if(ret <= 0){
      pop_off(); // 到这里 pop off 主要是为了防止执行别的进程时也 kalloc
      return 0;
    }
    acquire(&kmems[cpu].lock);
    for(int i = 0; i < ret; i++){
      if (!kmems[cpu].st_ret[i]) break;

      ((struct run*)kmems[cpu].st_ret[i])->next = kmems[cpu].freelist;
      kmems[cpu].freelist = kmems[cpu].st_ret[i];
    }
    r = kmems[cpu].freelist;
    kmems[cpu].freelist = r->next;
    release(&kmems[cpu].lock);
  }
  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk  
    DEBUG("kalloc 成功\n");
  }
  pop_off();
  return r;
}
