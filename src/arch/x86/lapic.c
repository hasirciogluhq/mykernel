#include <arch/x86/lapic.h>
#include <arch/x86/idt.h>

#define LAPIC_BASE 0xFEE00000u

#define LAPIC_ID      0x0020
#define LAPIC_EOI     0x00B0
#define LAPIC_SVR     0x00F0
#define LAPIC_ESR     0x0280
#define LAPIC_ICR_LO  0x0300
#define LAPIC_ICR_HI  0x0310
#define LAPIC_LVT_TMR 0x0320
#define LAPIC_LVT_LINT0 0x0350
#define LAPIC_LVT_LINT1 0x0360
#define LAPIC_LVT_ERR 0x0370

#define SVR_ENABLE      (1u << 8)
#define LVT_MASKED      (1u << 16)
#define LVT_DM_EXTINT   (7u << 8) /* 8259 PIC via LINT0 */
#define ICR_INIT        0x00000500u
#define ICR_STARTUP     0x00000600u
#define ICR_LEVEL       0x00008000u
#define ICR_ASSERT      0x00004000u
#define ICR_PENDING     0x00001000u
#define ICR_DEST_ALLX   0x000C0000u /* all except self */

#define APIC_SPURIOUS_VECTOR 0xFF
#define APIC_ERROR_VECTOR    0xFE

extern void apic_spurious_stub(void);
extern void apic_error_stub(void);

static inline uint32_t lapic_read(uint32_t reg)
{
    return *(volatile uint32_t *)(LAPIC_BASE + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(LAPIC_BASE + reg) = val;
}

static void lapic_enable_msr(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1Bu));
    lo |= (1u << 11); /* APIC global enable */
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1Bu));
}

static void lapic_wait_icr(void)
{
    while (lapic_read(LAPIC_ICR_LO) & ICR_PENDING)
        __asm__ volatile("pause" ::: "memory");
}

void apic_error_dispatch(void)
{
    /* Read ESR to clear; then EOI. */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    lapic_eoi();
}

static void lapic_install_vectors(void)
{
    idt_set_irq_gate(APIC_SPURIOUS_VECTOR, (uint32_t)apic_spurious_stub);
    idt_set_irq_gate(APIC_ERROR_VECTOR, (uint32_t)apic_error_stub);
}

static void lapic_common_init(int is_bsp)
{
    lapic_install_vectors();
    lapic_enable_msr();

    /* Mask local vectors we do not service yet. */
    lapic_write(LAPIC_LVT_TMR, LVT_MASKED);
    /*
     * PIC (PIT/PS2) reaches the BSP through LINT0 in ExtINT mode. Masking
     * it kills the timer → yield-sleep never wakes → UI/mouse freeze.
     * APs must keep LINT0 masked so only the BSP handles 8259 IRQs.
     */
    if (is_bsp)
        lapic_write(LAPIC_LVT_LINT0, LVT_DM_EXTINT);
    else
        lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERR, LVT_MASKED | APIC_ERROR_VECTOR);

    /* Spurious vector + software enable. */
    lapic_write(LAPIC_SVR, SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    /* Unmask error LVT now that the IDT gate exists. */
    lapic_write(LAPIC_LVT_ERR, APIC_ERROR_VECTOR);
    lapic_eoi();
}

void lapic_init_bsp(void)
{
    lapic_common_init(1);
}

void lapic_init_ap(void)
{
    lapic_common_init(0);
}

uint32_t lapic_id(void)
{
    return (lapic_read(LAPIC_ID) >> 24) & 0xFFu;
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

void lapic_send_ipi(uint8_t apic_id, uint8_t vector)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, (uint32_t)vector);
    lapic_wait_icr();
}

void lapic_send_init(uint8_t apic_id)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, ICR_INIT | ICR_LEVEL | ICR_ASSERT);
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_LO, ICR_INIT | ICR_LEVEL);
    lapic_wait_icr();
}

void lapic_send_startup(uint8_t apic_id, uint8_t vector)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICR_LO, ICR_STARTUP | (uint32_t)vector);
    lapic_wait_icr();
}

void lapic_broadcast_ipi(uint8_t vector)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HI, 0);
    lapic_write(LAPIC_ICR_LO, ICR_DEST_ALLX | (uint32_t)vector);
    lapic_wait_icr();
}
