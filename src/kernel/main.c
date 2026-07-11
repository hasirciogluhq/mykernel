#include <drivers/vga.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <arch/x86/idt.h>

static void delay(unsigned int count)
{
    for (volatile unsigned int i = 0; i < count; i++)
        ;
}

static void task_ping(void)
{
    for (;;) {
        const char *msg = "[ping] PING\n";
        sys_write(STDOUT_FILENO, msg, strlen(msg));
        delay(30000000);
        sys_yield();
    }
}

static void task_pong(void)
{
    for (;;) {
        const char *msg = "[pong] PONG\n";
        sys_write(STDOUT_FILENO, msg, strlen(msg));
        delay(30000000);
        sys_yield();
    }
}

static void task_reader(void)
{
    char buf[64];
    int fd = (int)sys_open("/motd", O_RDONLY);
    if (fd < 0) {
        const char *err = "[reader] open failed\n";
        sys_write(STDOUT_FILENO, err, strlen(err));
        sys_exit(1);
    }

    for (;;) {
        long n = sys_read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            sys_write(STDOUT_FILENO, "[reader] ", 9);
            sys_write(STDOUT_FILENO, buf, (size_t)n);
            sys_close(fd);
            fd = (int)sys_open("/motd", O_RDONLY);
        }
        delay(60000000);
        sys_yield();
    }
}

void kernel_main(void)
{
    vga_init();
    vga_set_color(VGA_ATTR(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    vga_print("mykernel — process / scheduler / syscall\n");
    vga_set_color(VGA_ATTR(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    vga_print("syscalls: read write open close getpid yield exit\n\n");

    idt_init();
    syscall_init();
    vfs_init();
    process_init();
    scheduler_init();

    process_create("ping", task_ping);
    process_create("pong", task_pong);
    process_create("reader", task_reader);

    vga_print("starting scheduler...\n\n");
    scheduler_start();
}
