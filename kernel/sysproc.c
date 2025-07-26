#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
#ifdef LAB_MMAP
uint64
sys_mmap(void){
  struct proc *p = myproc();
  struct vma *ptvma = 0;
  for (int i = 0; i < 16; i++) // 查找可用的 VMA 结构体
  {
    if (p->pvma[i].npages == 0)
    {
      ptvma = &(p->pvma[i]);
      break;
    }
  }
  if (!ptvma)
  {
    printf("no enough vmas\n");
    return -1;
  }
  p->sz = PGROUNDUP(p->sz); // 对齐地址
  // 赋值和获取参数
  ptvma->addr = p->sz;
  argsizet(1, &ptvma->len);
  argint(2, &ptvma->prot);
  argint(3, &ptvma->flags);
  argint(4, &ptvma->fd);
  arglong(5, &ptvma->offset);
  ptvma->vfile = p->ofile[ptvma->fd];
  if ((ptvma->prot & PROT_READ) && !((ptvma->vfile)->readable)) // 若要求可读但文件不可读，返回错误
    return 0xffffffffffffffff;
  if ((ptvma->flags & MAP_SHARED) && (ptvma->prot & PROT_WRITE) && !((ptvma->vfile)->writable)) // 若共享写但文件不可写，返回错误
    return 0xffffffffffffffff;

  filedup(ptvma->vfile); // 增加文件引用计数，防止文件被提前关闭
  ptvma->npages = PGROUNDUP(ptvma->len) / PGSIZE; // 计算所需的页数
  p->sz += ptvma->npages * PGSIZE; // 更新进程的内存大小(虚拟地址范围)
  return ptvma->addr;
}

int munmap_filewrite(struct file *f, uint64 addr, uint off, int n) {
  int r, ret = 0;
  int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE; // 单次最大写入字节数
  int i = 0; // 已写入字节数
  while (i < n) {
    int n1 = n - i; // 剩余要写入的字节数
    n1 = n1 > max ? max : n1; // 限制单次写入量不超过 max
    begin_op();
    ilock(f->ip);
    if ((r = writei(f->ip, 1, addr + i, off, n1)) > 0) // 从用户地址 addr+i 写入 n1 字节到文件 off 处
      off += r;                                        // r 是实际写入的字节数
    iunlock(f->ip);
    end_op();
    if (r != n1)
      break;
    i += r;
  }
  ret = (i == n ? n : -1);
  return ret;
}

int munmap(int i, struct proc *p, uint64 addr, int len) {
  int do_free = 0; // 是否需要释放物理页
  struct vma *ptvma = &(p->pvma[i]);
  struct file *vfile = ptvma->vfile;
  uint fizesize = vfile->ip->size;
  uint64 va = addr;
  int npages = PGROUNDUP(len) / PGSIZE;
  int can_write = (ptvma->flags & MAP_SHARED) && (ptvma->prot & PROT_WRITE) && (vfile->writable);
  for (int j = 0; j < npages; j++) {
    if (walkaddr(p->pagetable, va) != 0) { // 检查页是否已映射
      if (can_write){
        int off = va - addr;
        int n = (off + PGSIZE + ptvma->offset > fizesize) ? fizesize % PGSIZE : PGSIZE;
        if (munmap_filewrite(vfile, va, ptvma->offset + off, n) == -1) { // 写回
          printf("munmap_filewrite error\n");
          return -1;
        };
      }
      uvmunmap(p->pagetable, va, 1, do_free); // 解除页表映射
    }
    va += PGSIZE; // 处理下一个页
  }
  // 当munmap的位置在开头时，要对VMA做特别调整
  if (addr == ptvma->addr) {
    ptvma->addr = va; // 更新起始地址为解除区域的末尾
    ptvma->offset += npages * PGSIZE; // 更新偏移量
  }
  ptvma->npages -= npages;
  ptvma->len -= len;
  if (ptvma->npages == 0) { // 如果 VMA 已完全解除
    fileclose(ptvma->vfile); // 关闭文件，减少引用计数
    ptvma->vfile = 0; // 标记VMA为空
  }
  return 0;
}

uint64
sys_munmap(void) {
  uint64 addr;
  size_t len;
  struct proc *p = myproc();
  struct vma *ptvma;
  argaddr(0, &addr);
  argsizet(1, &len);
  int i = 0;
  for (; i < 16; i++) 
  {
    ptvma = &(p->pvma[i]);
    if (addr >= ptvma->addr && addr < ptvma->addr + ptvma->len) // 遍历找到匹配的 VMA
    {
      munmap(i, p, addr, len);
      return 0;
    }
  }
  return -1;
}
#endif 