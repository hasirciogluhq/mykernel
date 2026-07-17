#ifndef MYKERNEL_ARCH_LAPIC_H
#define MYKERNEL_ARCH_LAPIC_H

#include <kernel/types.h>

#define LAPIC_TIMER_VECTOR 0x30
#define LAPIC_IPI_VECTOR   0xF0

void     lapic_init_bsp(void);
void     lapic_init_ap(void);
uint32_t lapic_id(void);
void     lapic_eoi(void);
void     lapic_send_ipi(uint8_t apic_id, uint8_t vector);
void     lapic_send_init(uint8_t apic_id);
void     lapic_send_startup(uint8_t apic_id, uint8_t vector);
void     lapic_broadcast_ipi(uint8_t vector);

#endif
