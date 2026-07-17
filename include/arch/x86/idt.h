#ifndef MYKERNEL_IDT_H
#define MYKERNEL_IDT_H

#include <kernel/types.h>

void idt_init(void);
void idt_load(void);
void idt_set_irq_gate(uint8_t vector, uint32_t handler);

#endif
