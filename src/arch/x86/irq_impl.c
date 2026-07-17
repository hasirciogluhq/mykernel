#include <arch/x86/irq.h>
#include <arch/x86/idt.h>
#include <kernel/scheduler.h>
#include <drivers/driver.h>
#include <arch/x86/io.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_HZ    100u
#define PIT_DIV   (1193182u / PIT_HZ)

#define IRQ_TIMER 0

static volatile uint64_t g_timer_ticks;
static volatile uint64_t g_idle_ticks;

extern void irq_stub_0(void);
extern void irq_stub_1(void);
extern void irq_stub_2(void);
extern void irq_stub_3(void);
extern void irq_stub_4(void);
extern void irq_stub_5(void);
extern void irq_stub_6(void);
extern void irq_stub_7(void);
extern void irq_stub_8(void);
extern void irq_stub_9(void);
extern void irq_stub_10(void);
extern void irq_stub_11(void);
extern void irq_stub_12(void);
extern void irq_stub_13(void);
extern void irq_stub_14(void);
extern void irq_stub_15(void);

static void (*const g_irq_stubs[16])(void) = {
    irq_stub_0,  irq_stub_1,  irq_stub_2,  irq_stub_3,
    irq_stub_4,  irq_stub_5,  irq_stub_6,  irq_stub_7,
    irq_stub_8,  irq_stub_9,  irq_stub_10, irq_stub_11,
    irq_stub_12, irq_stub_13, irq_stub_14, irq_stub_15,
};

static inline void pic_outb(uint16_t port, uint8_t val)
{
    outb(port, val);
}

static void pic_remap(void)
{
    pic_outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    pic_outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    pic_outb(PIC1_DATA, 32);   /* master vector offset */
    pic_outb(PIC2_DATA, 40);   /* slave vector offset */
    pic_outb(PIC1_DATA, 4);    /* master has slave at IRQ2 */
    pic_outb(PIC2_DATA, 2);
    pic_outb(PIC1_DATA, ICW4_8086);
    pic_outb(PIC2_DATA, ICW4_8086);
    /* Mask everything; irq_init unmasks only the timer line. */
    pic_outb(PIC1_DATA, 0xFF);
    pic_outb(PIC2_DATA, 0xFF);
}

static void pit_init(void)
{
    /* Channel 0, lobyte/hibyte, rate generator mode 3. */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(PIT_DIV & 0xFF));
    outb(PIT_CH0, (uint8_t)((PIT_DIV >> 8) & 0xFF));
}

static void pic_unmask_timer(void)
{
    uint8_t mask = inb(PIC1_DATA);
    mask &= (uint8_t)~(1u << IRQ_TIMER);
    outb(PIC1_DATA, mask);
}

static void timer_eoi(void)
{
    if (IRQ_TIMER >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

uint64_t irq_timer_ticks(void)
{
    return g_timer_ticks;
}

uint64_t irq_idle_ticks(void)
{
    return g_idle_ticks;
}

void irq_dispatch(uint32_t irq)
{
    if (irq == IRQ_TIMER) {
        g_timer_ticks++;
        scheduler_wake_sleepers(g_timer_ticks);
        if (scheduler_current_is_idle() || !scheduler_has_runnable_apps())
            g_idle_ticks++;
        drivers_poll();
        scheduler_on_timer();
        timer_eoi();
        return;
    }

    if (irq < 8)
        outb(PIC1_CMD, 0x20);
    else
        outb(PIC2_CMD, 0x20);
}

void irq_ensure_timer_unmasked(void)
{
    pic_unmask_timer();
}

void irq_init(void)
{
    g_timer_ticks = 0;
    g_idle_ticks = 0;
    pic_remap();
    pit_init();
    for (int i = 0; i < 16; i++)
        idt_set_irq_gate((uint8_t)(32 + i), (uint32_t)g_irq_stubs[i]);
    irq_ensure_timer_unmasked();
    __asm__ volatile("sti");
}
