// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "kalloc_macros.h"

int pg_refcnt[MX_PGIDX];

struct spinlock refcnt_lock;

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&refcnt_lock, "ref cnt");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    PG_REFCNT(p) = 0;
    kfree(p);
  }
    
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

  acquire(&refcnt_lock);
  if(--PG_REFCNT(pa) <= 0){
    memset(pa, 1, PGSIZE);
    // Fill with junk to catch dangling refs.
    r = (struct run*)pa;
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
  release(&refcnt_lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
  }
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    acquire(&refcnt_lock);
    PG_REFCNT(r) = 1;
    release(&refcnt_lock);
  }
  return (void*)r;
}

void refcnt_inc(void* pa){
  acquire(&refcnt_lock);
  PG_REFCNT(pa)++;
  release(&refcnt_lock);
} 

void refcnt_dec(void* pa){
  acquire(&refcnt_lock);
  PG_REFCNT(pa)--;
  release(&refcnt_lock);
}

int get_free_pgtbl(){
  int ret = 0;
  acquire(&kmem.lock); // 先加锁
  struct run *free_pagelist = kmem.freelist;
  while(free_pagelist){ // 遍历这个链表
    free_pagelist = free_pagelist->next;
    ret++;
  }
  release(&kmem.lock);
  return ret;
}