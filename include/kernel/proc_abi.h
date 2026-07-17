#ifndef MYKERNEL_PROC_ABI_H
#define MYKERNEL_PROC_ABI_H

#include <kernel/types.h>

/*
 * Shared kernel↔userspace process snapshot (identity-mapped; seqlock).
 * Same idea as time_page: map once, read hot path without syscall.
 */
#define PROC_PAGE_MAGIC 0x434F5250u /* 'PROC' little-endian */
/* Display/snapshot cap (full table may be larger; see process_count). */
#define PROC_PAGE_MAX   256
#define PROC_PAGE_NAME  32

typedef struct proc_page_entry {
    pid_t    pid;
    pid_t    ppid;
    uint32_t state;
    uint32_t is_user;
    uint64_t cpu_ticks;
    uint64_t uptime_ticks;
    uint32_t mem_bytes;
    char     name[PROC_PAGE_NAME];
} proc_page_entry_t;

typedef struct proc_page {
    uint32_t magic;
    volatile uint32_t seq; /* seqlock: odd while writer updates */

    uint32_t count;          /* entries[] filled */
    uint32_t process_count;  /* live processes (may exceed PROC_PAGE_MAX) */
    uint64_t uptime_ticks;
    uint64_t total_cpu_ticks;
    uint32_t total_ram_bytes;
    uint32_t used_ram_bytes;
    uint32_t free_ram_bytes;
    uint32_t generation;     /* bumps on create/exit */

    proc_page_entry_t entries[PROC_PAGE_MAX];
} proc_page_t;

#endif
