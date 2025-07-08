// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

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

struct spinlock ref_lock;
int pm_ref[(PHYSTOP - KERNBASE) / PGSIZE]; 

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref_lock, "pm_ref");
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

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&ref_lock);

  pm_ref[refidx((uint64)pa)] --;

  // 如果引用计数已经为 0，说明没人再使用这页，可以释放
  if(pm_ref[refidx((uint64)pa)] <= 0){
    memset(pa, 1, PGSIZE);
    r = (struct run*)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }

  release(&ref_lock);
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
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE);

    // 初始化引用计数为 1（即当前只有一个使用者）
    pm_ref[refidx((uint64)r)] = 1;
  }
  return (void*)r;
}

// 将物理地址转换为引用计数数组的索引
uint64
refidx(uint64 pa){
  return (pa - KERNBASE) / PGSIZE;
}

// 增加某页的引用计数（例如共享页时）
void
refup(void* pa){
  acquire(&ref_lock);
  pm_ref[refidx((uint64)pa)] ++;
  release(&ref_lock);
}

// 减少某页的引用计数（不是释放，只是标记减少）
void
refdown(void* pa){
  acquire(&ref_lock);
  pm_ref[refidx((uint64)pa)] --;
  release(&ref_lock);
}

void*
copyPA(void* pa){
  acquire(&ref_lock);

  // 如果引用计数只有 1，说明没有其他用户，可以直接写
  if(pm_ref[refidx((uint64)pa)] <= 1){
    release(&ref_lock);
    return pa;
  }

  // 否则分配新页（进行复制）
  char* new = kalloc();
  if(new == 0){
    release(&ref_lock);
    panic("out of memory");
    return 0;
  }

  // 将原页内容复制到新页
  memmove((void*)new, pa, PGSIZE);

  // 原页的引用计数减一（当前页会换成新页）
  pm_ref[refidx((uint64)pa)] --;

  release(&ref_lock);

  return (void*)new;
}
