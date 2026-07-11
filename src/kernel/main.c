#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/shell.h>
#include <arch/x86/gdt.h>
#include <arch/x86/idt.h>

void kernel_main(void)
{
    gdt_init();
    idt_init();
    syscall_init();

    vga_init();
    keyboard_init();
    console_init();
    vfs_init();
    process_init();
    scheduler_init();

    vga_set_color(VGA_ATTR(VGA_COLOR_YELLOW, VGA_COLOR_BLACK));
    vga_print("mykernel ready\n");
    vga_set_color(VGA_ATTR(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    vga_print("console + ring-3 userspace enabled\n");

    process_create("shell", shell_main);
    scheduler_start();
}
