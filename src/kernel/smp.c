#include <kernel/smp.h>
#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <arch/x86/cpu.h>
#include <arch/x86/lapic.h>
#include <arch/x86/gdt.h>
#include <arch/x86/idt.h>
#include <arch/x86/io.h>
#include <drivers/serial.h>

#define SMP_TRAMPOLINE_ADDR 0x8000u
#define SMP_INFO_ADDR       0x7000u
#define AP_STACK_SIZE       8192u

struct smp_boot_info {
    uint32_t entry;
    uint32_t stack;
    uint16_t gdt_limit;
    uint32_t gdt_base;
    uint32_t cpu_index;
} __attribute__((packed));

extern char smp_trampoline_start[];
extern uint32_t smp_trampoline_size;

extern void ipi_stub(void);

static volatile int g_smp_sched_go;

static void smp_delay(uint32_t loops)
{
    for (uint32_t i = 0; i < loops; i++)
        outb(0x80, 0);
}

static void smp_install_trampoline(void)
{
    uint32_t n = smp_trampoline_size;
    uint8_t *dst = (uint8_t *)(uintptr_t)SMP_TRAMPOLINE_ADDR;

    if (n == 0 || n > 0x1000u)
        return;
    memcpy(dst, smp_trampoline_start, n);
}

void ipi_dispatch(void)
{
    /* Wake from HLT only — scheduling happens in idle/yield paths. */
    lapic_eoi();
}

void smp_ap_main(uint32_t cpu_index)
{
    cpu_t *c = cpu_get((int)cpu_index);

    if (!c) {
        for (;;)
            __asm__ volatile("hlt");
    }

    lapic_init_ap();
    gdt_load_cpu(c->id);
    idt_load();
    c->started = 1;
    c->online = 1;

    klog("[smp] AP online cpu=");
    serial_print_uint((uint32_t)c->id);
    klog(" apic=");
    serial_print_uint((uint32_t)c->apic_id);
    klog("\n");

    process_set_current(NULL);

    /* Park until BSP finishes driver/app bring-up. */
    while (!g_smp_sched_go)
        __asm__ volatile("pause" ::: "memory");

    __asm__ volatile("sti" ::: "memory");
    schedule();
    for (;;)
        __asm__ volatile("hlt");
}

static int smp_start_ap(cpu_t *c)
{
    struct smp_boot_info *info = (struct smp_boot_info *)(uintptr_t)SMP_INFO_ADDR;
    uint16_t gdt_limit;
    uint32_t gdt_base;
    uint32_t *stack;
    int wait;

    stack = (uint32_t *)kmalloc_aligned(AP_STACK_SIZE, 16);
    if (!stack)
        return -1;
    c->boot_stack = stack;

    gdt_get_ptr(&gdt_limit, &gdt_base);
    memset(info, 0, sizeof(*info));
    info->entry = (uint32_t)smp_ap_main;
    info->stack = (uint32_t)((uint8_t *)stack + AP_STACK_SIZE);
    info->gdt_limit = gdt_limit;
    info->gdt_base = gdt_base;
    info->cpu_index = (uint32_t)c->id;

    c->started = 0;
    c->online = 0;

    lapic_send_init(c->apic_id);
    smp_delay(200000);
    lapic_send_startup(c->apic_id, (uint8_t)(SMP_TRAMPOLINE_ADDR >> 12));
    smp_delay(4000);
    lapic_send_startup(c->apic_id, (uint8_t)(SMP_TRAMPOLINE_ADDR >> 12));

    for (wait = 0; wait < 500000 && !c->started; wait++)
        __asm__ volatile("pause" ::: "memory");

    return c->started ? 0 : -1;
}

void smp_init(void)
{
    uint8_t bsp_apic = (uint8_t)lapic_id();

    g_smp_sched_go = 0;
    idt_set_irq_gate(LAPIC_IPI_VECTOR, (uint32_t)ipi_stub);
    smp_install_trampoline();

    /* Keep IRQs off while APs run the trampoline (no IDT yet). */
    __asm__ volatile("cli" ::: "memory");

    /*
     * QEMU -smp N uses contiguous APIC ids. Probe until the first AP that
     * fails to start (no more CPUs).
     */
    for (uint32_t apic = 0; apic < CPU_MAX; apic++) {
        cpu_t *c;

        if ((uint8_t)apic == bsp_apic)
            continue;
        if (cpu_count() >= CPU_MAX)
            break;

        c = cpu_alloc((uint8_t)apic);
        if (!c)
            break;

        /* AP parks on the barrier until smp_start_scheduling — idle later. */
        if (smp_start_ap(c) < 0) {
            klog("[smp] AP start timeout apic=");
            serial_print_uint(apic);
            klog(" — stopping probe\n");
            cpu_rollback_last();
            break;
        }

        if (scheduler_create_idle_for_cpu(c) < 0) {
            klog("[smp] idle create failed apic=");
            serial_print_uint(apic);
            klog("\n");
            break;
        }
    }

    __asm__ volatile("sti" ::: "memory");
    klog_uint("[smp] cpus online=", (uint32_t)cpu_count());
}

void smp_start_scheduling(void)
{
    __asm__ volatile("" ::: "memory");
    g_smp_sched_go = 1;
    smp_reschedule_others();
}

void smp_reschedule_others(void)
{
    int self = cpu_id();

    for (int i = 0; i < cpu_count(); i++) {
        cpu_t *c = cpu_get(i);
        if (!c || !c->online || c->id == self)
            continue;
        lapic_send_ipi(c->apic_id, LAPIC_IPI_VECTOR);
    }
}

int smp_cpu_count(void)
{
    return cpu_count();
}
