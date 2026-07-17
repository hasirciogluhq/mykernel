#include <arch/x86/gdt.h>
#include <arch/x86/cpu.h>
#include <kernel/string.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* null + code/data×2 + per-CPU TSS */
#define GDT_ENTRIES (5 + CPU_MAX)

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtp;
static struct tss_entry tss[CPU_MAX];

extern void gdt_flush(struct gdt_ptr *ptr);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[num].base_low = (uint16_t)(base & 0xFFFF);
    gdt[num].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[num].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[num].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[num].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

static void gdt_set_tss(int cpu)
{
    uint32_t base = (uint32_t)&tss[cpu];
    int idx = 5 + cpu;

    memset(&tss[cpu], 0, sizeof(tss[cpu]));
    tss[cpu].ss0 = GDT_KERNEL_DATA;
    tss[cpu].iomap_base = (uint16_t)sizeof(tss[cpu]);
    gdt_set_gate(idx, base, sizeof(tss[cpu]) - 1, 0x89, 0x00);
}

void gdt_set_kernel_stack(uint32_t esp0)
{
    int id = cpu_id();
    if (id < 0 || id >= CPU_MAX)
        id = 0;
    tss[id].esp0 = esp0;
    tss[id].ss0 = GDT_KERNEL_DATA;
}

void gdt_get_ptr(uint16_t *limit_out, uint32_t *base_out)
{
    if (limit_out)
        *limit_out = gdtp.limit;
    if (base_out)
        *base_out = gdtp.base;
}

void gdt_load_cpu(int cpu)
{
    uint16_t sel;

    if (cpu < 0 || cpu >= CPU_MAX)
        return;
    gdt_flush(&gdtp);
    sel = (uint16_t)(GDT_TSS + (uint16_t)(cpu * 8));
    __asm__ volatile("ltr %0" : : "r"(sel));
}

void gdt_init(void)
{
    int i;

    gdtp.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtp.base = (uint32_t)&gdt;

    memset(gdt, 0, sizeof(gdt));
    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    for (i = 0; i < CPU_MAX; i++)
        gdt_set_tss(i);

    gdt_load_cpu(0);
}
