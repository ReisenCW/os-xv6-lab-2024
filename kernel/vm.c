#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// Initialize the one kernel_pagetable
void kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int *is_superpage)
{
  *is_superpage = -1; // When the pte not found
  if (va >= MAXVA)
    panic("walk");

  pte_t *pte; // iterate from L2 to L0
  int level = 2;
  while (level >= 0)
  {
    pte = &pagetable[PX(level, va)];

    if (*pte & PTE_V)
    {
      if (PTE_LEAF(*pte))
      {
        if (level == 2)
        {
          panic("walk: superpage entry in level 2");
        }
        else if (level == 1)
        {
          *is_superpage = 1;
        }
        else
        {
          *is_superpage = 0;
        }
        break;
      }
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      // The pagetable entry has not been allocated.
      pte = 0;
      break;
    }

    --level;
  }
  return pte;
}

// Walk the pagetable, and allocate the pagetable page.
// If is_superpage is 1, a superpage shall be allocated.
// If is_superpage is 0, a normal page shall be allocated.
// Shall panic if this va has been mapped.
pte_t *
walk_alloc(pagetable_t pagetable, uint64 va, int is_superpage)
{
  if (va >= MAXVA)
    panic("walk");

  int lowest_level = 0; // The lowest level of the pagetable.
  if (is_superpage)
    lowest_level = 1;

  pte_t *pte; // iterate from L2 to L0
  int level = 2;
  while (level >= lowest_level)
  {
    pte = &pagetable[PX(level, va)];

    if (level != lowest_level)
    {
      if (*pte & PTE_V)
      {
        pagetable = (pagetable_t)PTE2PA(*pte);
      }
      else
      {
        if ((pagetable = (pde_t *)kalloc()) == 0)
          return 0;
        memset(pagetable, 0, PGSIZE);
        *pte = PA2PTE(pagetable) | PTE_V;
      }
    }
    else
    {
      if (*pte & PTE_V)
      {
        panic("walk_alloc: The pagetable entry has been allocated.");
        // return 0;
      }
      break;
    }

    --level;
  }
  return pte;
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  int is_superpage = -1;
  pte = walk(pagetable, va, &is_superpage);
  if (is_superpage != 0 && is_superpage != -1)
  {
    panic("walkaddr: Not superpage shall exist now.");
  }

  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(0, kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(int allow_superpage, pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  { // Basic preconditions check
    if ((va % PGSIZE) != 0)
      panic("mappages: va not aligned");

    if ((size % PGSIZE) != 0)
      panic("mappages: size not aligned");

    if (size == 0)
      panic("mappages: size");
  }

  int enable_superpage = 0;
  int pagesize = PGSIZE;
  { // Check the pre-condition for enabling superpage
    if (allow_superpage && size >= SUPERPGSIZE)
    {
      if (size % SUPERPGSIZE != 0)
        panic("mappages: size not aligned for superpage");
      if (va % SUPERPGSIZE != 0)
        panic("mappages: va not aligned for superpage");

      enable_superpage = 1;
      pagesize = SUPERPGSIZE;

      // printf("mappages: superpage enabled. va = 0x%lx\n", va);
    }
  }

  a = va;
  last = va + size - pagesize;
  for (;;)
  {
    if ((pte = walk_alloc(pagetable, a, enable_superpage)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += pagesize;
    pa += pagesize;
  }
  return 0;
}

// Remove npages of mappings starting from va.
// va must be page-aligned. The mappings must exist.
// Optionally free the physical memory.
// npages must be a multiple of PGSIZE.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  uint64 current_va = va;
  uint64 last_va = va + npages * PGSIZE;
  while (current_va < last_va)
  {
    // printf("uvmunmap: current_va: 0x%lx, last_va: 0x%lx\n", current_va, last_va);
    uint64 pgsize = 0; // updated below.
    int superpage = -1;

    pte = walk(pagetable, current_va, &superpage);

    // The pte must contain a valid addr.
    if (pte == 0)
    {
      // printf("uvmunmap: walked to not mapped vaddr 0x%lx, due to the interval between small pages and a superpage\n", current_va);
      current_va = SUPERPGROUNDUP(current_va);
      continue;
    }

    // The pte must be valid.
    if ((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");

    // The pte must be a leaf (i.e., not only PTE_V, shall with other flags)
    if (PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");

    uint64 pa = PTE2PA(*pte);

    if (superpage == 1)
    {
      if (do_free)
        superfree((void *)pa);

      pgsize = SUPERPGSIZE;
    }
    else if (superpage == 0)
    {
      if (do_free)
        kfree((void *)pa);

      pgsize = PGSIZE;
    }
    else
    {
      panic("uvmunmap: superpage flag not set");
    }

    // Clear the pagetable entry.
    *pte = 0;

    current_va += pgsize;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(0, pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int pgsize;
  int allow_superpg = 0;

  if (newsz < oldsz)
    return oldsz;

  uint64 size_diff = newsz - oldsz;

  uint64 roundup_oldsz;
  uint64 pagealigned_delta_sz;

  if (size_diff < SUPERPGSIZE)
  {
    allow_superpg = 0;
    pgsize = PGSIZE;
    roundup_oldsz = PGROUNDUP(oldsz);
    pagealigned_delta_sz = PGROUNDUP(size_diff);
  }
  else
  {
    allow_superpg = 1;
    pgsize = SUPERPGSIZE;
    roundup_oldsz = SUPERPGROUNDUP(oldsz);
    pagealigned_delta_sz = SUPERPGROUNDUP(size_diff);
  }

  for (a = roundup_oldsz; a < roundup_oldsz + pagealigned_delta_sz; a += pgsize)
  {
    if (allow_superpg == 1)
      mem = superalloc();
    else
      mem = kalloc();

    if (mem == 0)
    {
      uvmdealloc(pagetable, a, roundup_oldsz);
      return 0;
    }

    memset(mem, 0, pgsize);

    if (mappages(allow_superpg, pagetable, a, pgsize, (uint64)mem, PTE_R | PTE_U | xperm) != 0)
    {
      if (allow_superpg == 1)
        superfree(mem);
      else
        kfree(mem);

      uvmdealloc(pagetable, a, roundup_oldsz);
      return 0;
    }
  }

  return roundup_oldsz + pagealigned_delta_sz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa;
  uint64 vaddr = 0;
  uint flags;
  char *mem;
  int szinc = PGSIZE; // Default size increment is PGSIZE, might be changed to SUPERPGSIZE

  while (vaddr < sz)
  {
    int is_superpage = -1;
    if ((pte = walk(old, vaddr, &is_superpage)) == 0)
    {
      // You can fall into this branch,
      // When super page exist, might exist some virtual addrs in between that are not mapped.
      vaddr = SUPERPGROUNDUP(vaddr);
      continue;
    }

    if (is_superpage == 1)
      szinc = SUPERPGSIZE;
    else if (is_superpage == 0)
      szinc = PGSIZE;
    else
      panic("uvmcopy: is_superpage is not set");

    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    if (is_superpage == 1)
    {
      if ((mem = superalloc()) == 0)
        goto err;
    }
    else
    {
      if ((mem = kalloc()) == 0)
        goto err;
    }
    memmove(mem, (char *)pa, szinc);
    if (mappages(is_superpage, new, vaddr, szinc, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }

    vaddr += szinc;
  }
  return 0;

err:
  uvmunmap(new, 0, vaddr / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  int is_superpage = -1;
  pte = walk(pagetable, va, &is_superpage);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    int is_superpage = -1;
    if ((pte = walk(pagetable, va0, &is_superpage)) == 0)
    {
      // printf("copyout: pte should exist 0x%x %d\n", dstva, len);
      return -1;
    }

    // forbid copyout over read-only user text pages.
    if ((*pte & PTE_W) == 0)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0)
    {
      if (*p == '\0')
      {
        *dst = '\0';
        got_null = 1;
        break;
      }
      else
      {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null)
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

#ifdef LAB_PGTBL
void vm_recur_print(pagetable_t pagetable, int depth);
void vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", (void *)pagetable);
  vm_recur_print(pagetable, 1);
}

void vm_recur_print(pagetable_t pagetable, int depth)
{
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if (pte & PTE_V)
    {
      // 打印自己
      for (int i = 0; i < depth; i++)
      {
        printf(" ..");
      }
      uint64 pa = PTE2PA(pte);
      printf("%p: pte %p pa %p\n",
             (void *)(pagetable + i), (void *)pte, (void *)pa);
      // 打印自己的页表项指向的页表
      if ((pte & (PTE_R | PTE_W | PTE_X)) == 0)
      {
        // 指向低一级的页表,权限必须为0，有效必须为1
        uint64 child = PTE2PA(pte);
        vm_recur_print((pagetable_t)child, depth + 1);
      }
    }
  }
}
#endif

#ifdef LAB_PGTBL
pte_t *
pgpte(pagetable_t pagetable, uint64 va)
{
  int is_superpage = -1;
  return walk(pagetable, va, &is_superpage);
}
#endif
