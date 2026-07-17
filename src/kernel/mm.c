#include <kernel/mm.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <kernel/errno.h>
#include <kernel/string.h>

void mm_init(void)
{
}

void *mm_alloc_pages(size_t npages)
{
    if (npages == 0)
        return NULL;
    return kmalloc_aligned(npages * PAGE_SIZE, PAGE_SIZE);
}

void mm_free_pages(void *ptr, size_t npages)
{
    (void)npages;
    kfree(ptr);
}

static vma_t *proc_vmas(process_t *p)
{
    return p ? p->vmas : NULL;
}

long mm_mmap(process_t *p, uint32_t addr, size_t len, int prot, int flags,
             int vfs_fd, off_t off)
{
    vma_t *vmas;
    size_t npages, i;
    void *pages;
    uint32_t start;
    int slot = -1;

    (void)flags;
    if (!p || len == 0)
        return -EINVAL;
    vmas = proc_vmas(p);
    if (!vmas)
        return -ENOMEM;

    npages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    pages = mm_alloc_pages(npages);
    if (!pages)
        return -ENOMEM;
    memset(pages, 0, npages * PAGE_SIZE);

    if (vfs_fd >= 0) {
        off_t pos = off;
        size_t got = 0;
        while (got < len) {
            ssize_t n = vfs_read(vfs_fd, (uint8_t *)pages + got, len - got);
            if (n < 0) {
                mm_free_pages(pages, npages);
                return n;
            }
            if (n == 0)
                break;
            got += (size_t)n;
            pos += n;
            (void)pos;
            if (vfs_lseek(vfs_fd, off + (off_t)got, SEEK_SET) < 0)
                break;
        }
    }

    if (addr)
        start = addr & ~(PAGE_SIZE - 1);
    else {
        start = 0x04000000u; /* default mmap base */
        for (i = 0; i < VMA_MAX; i++) {
            if (vmas[i].used && vmas[i].end > start)
                start = (vmas[i].end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        }
    }

    for (i = 0; i < VMA_MAX; i++) {
        if (!vmas[i].used) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        mm_free_pages(pages, npages);
        return -ENOMEM;
    }

    vmas[slot].used = 1;
    vmas[slot].start = start;
    vmas[slot].end = start + (uint32_t)(npages * PAGE_SIZE);
    vmas[slot].offset = (uint32_t)off;
    vmas[slot].prot = prot;
    vmas[slot].flags = flags;
    vmas[slot].fd = vfs_fd;
    vmas[slot].pages = pages;
    vmas[slot].npages = npages;

    /* Identity map: return physical address of backing pages. */
    return (long)(uintptr_t)pages;
}

int mm_munmap(process_t *p, uint32_t addr, size_t len)
{
    vma_t *vmas;
    size_t i;
    (void)len;
    if (!p)
        return -EINVAL;
    vmas = proc_vmas(p);
    for (i = 0; i < VMA_MAX; i++) {
        if (!vmas[i].used)
            continue;
        if ((uint32_t)(uintptr_t)vmas[i].pages == addr || vmas[i].start == addr) {
            if ((vmas[i].prot & PROT_WRITE) && vmas[i].fd >= 0) {
                vfs_lseek(vmas[i].fd, (off_t)vmas[i].offset, SEEK_SET);
                vfs_write(vmas[i].fd, vmas[i].pages, vmas[i].npages * PAGE_SIZE);
            }
            mm_free_pages(vmas[i].pages, vmas[i].npages);
            memset(&vmas[i], 0, sizeof(vmas[i]));
            return 0;
        }
    }
    return -EINVAL;
}

int mm_msync(process_t *p, uint32_t addr, size_t len, int flags)
{
    vma_t *vmas;
    size_t i;
    (void)flags;
    if (!p)
        return -EINVAL;
    vmas = proc_vmas(p);
    for (i = 0; i < VMA_MAX; i++) {
        if (!vmas[i].used)
            continue;
        if ((uint32_t)(uintptr_t)vmas[i].pages == addr ||
            (addr >= vmas[i].start && addr < vmas[i].end)) {
            size_t n = len ? len : (vmas[i].npages * PAGE_SIZE);
            if ((vmas[i].prot & PROT_WRITE) && vmas[i].fd >= 0) {
                vfs_lseek(vmas[i].fd, (off_t)vmas[i].offset, SEEK_SET);
                vfs_write(vmas[i].fd, vmas[i].pages, n);
            }
            return 0;
        }
    }
    return -EINVAL;
}
