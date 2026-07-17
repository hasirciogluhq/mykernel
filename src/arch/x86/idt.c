#include <arch/x86/idt.h>
#include <arch/x86/gdt.h>
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

#define DECL_ISR(n) extern void isr_exc_##n(void);
DECL_ISR(0) DECL_ISR(1) DECL_ISR(2) DECL_ISR(3)
DECL_ISR(4) DECL_ISR(5) DECL_ISR(6) DECL_ISR(7)
DECL_ISR(8) DECL_ISR(9) DECL_ISR(10) DECL_ISR(11)
DECL_ISR(12) DECL_ISR(13) DECL_ISR(14) DECL_ISR(15)
DECL_ISR(16) DECL_ISR(17) DECL_ISR(18) DECL_ISR(19)
DECL_ISR(20) DECL_ISR(21) DECL_ISR(22) DECL_ISR(23)
DECL_ISR(24) DECL_ISR(25) DECL_ISR(26) DECL_ISR(27)
DECL_ISR(28) DECL_ISR(29) DECL_ISR(30) DECL_ISR(31)

static void *const g_exc[32] = {
    isr_exc_0,  isr_exc_1,  isr_exc_2,  isr_exc_3,
    isr_exc_4,  isr_exc_5,  isr_exc_6,  isr_exc_7,
    isr_exc_8,  isr_exc_9,  isr_exc_10, isr_exc_11,
    isr_exc_12, isr_exc_13, isr_exc_14, isr_exc_15,
    isr_exc_16, isr_exc_17, isr_exc_18, isr_exc_19,
    isr_exc_20, isr_exc_21, isr_exc_22, isr_exc_23,
    isr_exc_24, isr_exc_25, isr_exc_26, isr_exc_27,
    isr_exc_28, isr_exc_29, isr_exc_30, isr_exc_31,
};

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[num].base_lo = (uint16_t)(base & 0xFFFF);
    idt[num].base_hi = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

void idt_set_irq_gate(uint8_t vector, uint32_t handler)
{
    idt_set_gate(vector, handler, GDT_KERNEL_CODE, 0x8E);
}

void idt_load(void)
{
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

void idt_init(void)
{
    int i;

    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base = (uint32_t)&idt;

    for (i = 0; i < 256; i++)
        idt_set_gate((uint8_t)i, 0, 0, 0);

    /* 0x8E = present | DPL0 | 32-bit interrupt gate */
    for (i = 0; i < 32; i++)
        idt_set_gate((uint8_t)i, (uint32_t)g_exc[i], GDT_KERNEL_CODE, 0x8E);

    /* 0xEE = present | DPL3 | 32-bit interrupt gate (userspace int 0x80) */
    idt_set_gate(0x80, (uint32_t)isr_syscall, GDT_KERNEL_CODE, 0xEE);

    idt_load();
}
