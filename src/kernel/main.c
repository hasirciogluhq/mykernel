#include <multiboot.h>
#include <drivers/vga.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <gfx/server.h>
#include <arch/x86/gdt.h>
#include <arch/x86/idt.h>
#include <user/gx.h>

#define HEAP_SIZE (20u * 1024u * 1024u)
static uint8_t heap_area[HEAP_SIZE] __attribute__((aligned(16)));

void kernel_main(uint32_t magic, multiboot_info_t *mbi)
{
    vga_init();

    if (magic != MULTIBOOT_MAGIC) {
        vga_print("bad multiboot magic\n");
        for (;;)
            __asm__ volatile("hlt");
    }

    heap_init(heap_area, HEAP_SIZE);
    gdt_init();
    idt_init();
    syscall_init();
    vfs_init();
    process_init();
    scheduler_init();

    if (gx_server_init(mbi) < 0) {
        vga_print("gx_server_init failed\n");
        for (;;)
            __asm__ volatile("hlt");
    }

    if (process_create_user("os-ui", user_os_ui_main) < 0) {
        vga_print("os-ui spawn failed\n");
        for (;;)
            __asm__ volatile("hlt");
    }

    scheduler_start();
}
