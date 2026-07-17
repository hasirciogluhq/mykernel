#ifndef MYKERNEL_MKDX_CONSOLE_H
#define MYKERNEL_MKDX_CONSOLE_H

#include "window.h"
#include <kernel/types.h>

/* Pointer table only — console buffers allocate on demand. */
#define PROC_CONSOLE_MAX      1024
#define PROC_CONSOLE_BUF_SIZE 8192
#define PROC_CONSOLE_COLS     80
#define PROC_CONSOLE_ROW_H    10

int        proc_console_init(void);
void       proc_console_shutdown(void);
int        proc_console_alloc(wm_t *wm, int pid, const char *name, int visible);
void       proc_console_free(int pid);
ssize_t    proc_console_write(int pid, const void *buf, size_t len);
int        proc_console_show(int pid, int visible);
void       proc_console_paint_dirty(wm_t *wm);

#endif
