#ifndef MYKERNEL_MM_H
#define MYKERNEL_MM_H

#include <kernel/types.h>

#define PAGE_SIZE 4096u
#define VMA_MAX   32

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_FIXED   0x10

struct process;
struct file;

typedef struct vma {
    uint32_t start;
    uint32_t end;
    uint32_t offset; /* file offset of start */
    int      prot;
    int      flags;
    int      fd;     /* vfs fd, -1 if anon */
    void    *pages;  /* contiguous physical backing */
    size_t   npages;
    int      used;
} vma_t;

void mm_init(void);
void *mm_alloc_pages(size_t npages);
void  mm_free_pages(void *ptr, size_t npages);

/* File-backed mmap on identity-mapped address space. */
long mm_mmap(struct process *p, uint32_t addr, size_t len, int prot, int flags,
             int vfs_fd, off_t off);
int  mm_munmap(struct process *p, uint32_t addr, size_t len);
int  mm_msync(struct process *p, uint32_t addr, size_t len, int flags);

#endif
