#include <arch/x86/idt.h>
#include <kernel/types.h>

struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern void isr_syscall(void);

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[num].base_lo = (uint16_t)(base & 0xFFFF);
    idt[num].base_hi = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

void idt_init(void)
{
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base = (uint32_t)&idt;

    for (int i = 0; i < 256; i++)
        idt_set_gate((uint8_t)i, 0, 0, 0);

    /* 0x08 = kernel code segment (Multiboot flat GDT), 0x8E = present | ring0 | 32-bit interrupt gate */
    idt_set_gate(0x80, (uint32_t)isr_syscall, 0x08, 0x8E);

    __asm__ volatile("lidt %0" : : "m"(idtp));
}
