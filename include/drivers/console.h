#ifndef MYKERNEL_DRIVERS_CONSOLE_H
#define MYKERNEL_DRIVERS_CONSOLE_H

#include <kernel/types.h>

void    console_init(void);
void    console_putc(char c);
void    console_write(const char *s, size_t n);
void    console_print(const char *s);
int     console_getc(void);              /* blocking (yields) */
ssize_t console_read(void *buf, size_t count); /* blocking, up to count */
void    console_push_scancode_char(char c);    /* from keyboard */

#endif
