// 物理内存分配器，用于用户进程、内核栈、页表页和管道缓冲区
// 分配整个4096字节的页

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // 内核后的第一个地址，由kernel.ld定义

// 内存块结构
struct run {
  struct run *next;
};

// 每CPU的空闲链表结构
struct cpu_kmem {
  struct spinlock lock;  // 保护本CPU空闲链表的锁
  struct run *freelist;  // 空闲内存块链表
};

// 为每个CPU创建一个空闲链表（NCPU定义在param.h中）
static struct cpu_kmem kmem_cpus[NCPU];

// 初始化内存分配器
void
kinit()
{
  // 初始化每个CPU的锁和空闲链表
  for (int i = 0; i < NCPU; i++) {
    char lockname[16];
    snprintf(lockname, sizeof(lockname), "kmem%d", i); // 锁名称以"kmem"开头
    initlock(&kmem_cpus[i].lock, lockname);
    kmem_cpus[i].freelist = 0;
  }

  // 将所有空闲内存分配给当前CPU（执行kinit的CPU）
  freerange(end, (void*)PHYSTOP);
}

// 释放物理内存页
void
kfree(void *pa)
{
  struct run *r;

  // 检查地址有效性：必须页对齐，且在合法物理内存范围内
  if (((uint64)pa % PGSIZE) != 0 || 
      (char*)pa < end || 
      (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 填充垃圾数据以捕获悬垂引用
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // 获取当前CPU编号（关闭中断确保安全）
  push_off(); // 关闭中断
  int c = cpuid(); // 获取当前CPU核心号
  pop_off(); // 恢复中断

  // 将内存块加入当前CPU的空闲链表
  acquire(&kmem_cpus[c].lock);
  r->next = kmem_cpus[c].freelist;
  kmem_cpus[c].freelist = r;
  release(&kmem_cpus[c].lock);
}

// 分配一个4096字节的物理内存页
// 返回内核可使用的指针，若无法分配则返回0
void *
kalloc(void)
{
  struct run *r = 0;

  // 获取当前CPU编号（关闭中断确保安全）
  push_off();
  int c = cpuid();
  pop_off();

  // 1. 优先从当前CPU的空闲链表分配
  acquire(&kmem_cpus[c].lock);
  if (kmem_cpus[c].freelist) {
    r = kmem_cpus[c].freelist;
    kmem_cpus[c].freelist = r->next;
    release(&kmem_cpus[c].lock);
  } else {
    // 2. 当前CPU链表为空，尝试从其他CPU窃取内存
    release(&kmem_cpus[c].lock); // 先释放当前CPU的锁，避免死锁

    // 遍历其他CPU的空闲链表
    for (int i = 0; i < NCPU; i++) {
      if (i == c) continue; // 跳过当前CPU
      acquire(&kmem_cpus[i].lock);
      if (kmem_cpus[i].freelist) {
        // 从CPU i的链表中窃取一个内存块
        r = kmem_cpus[i].freelist;
        kmem_cpus[i].freelist = r->next;
        release(&kmem_cpus[i].lock);
        break; // 成功窃取后退出循环
      }
      release(&kmem_cpus[i].lock);
    }
  }

  // 填充垃圾数据以捕获未初始化内存的使用
  if (r)
    memset((char*)r, 5, PGSIZE);

  return (void*)r;
}

// 初始化指定范围的物理内存为空闲状态
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start); // 页对齐起始地址
  for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p); // 将每页加入当前CPU的空闲链表
}