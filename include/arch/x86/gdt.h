#ifndef MYKERNEL_ARCH_GDT_H
#define MYKERNEL_ARCH_GDT_H

#include <kernel/types.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x1B  /* 0x18 | RPL3 */
#define GDT_USER_DATA   0x23  /* 0x20 | RPL3 */
#define GDT_TSS         0x28  /* TSS for CPU0; CPU n at 0x28 + n*8 */

void gdt_init(void);
void gdt_set_kernel_stack(uint32_t esp0);
void gdt_get_ptr(uint16_t *limit_out, uint32_t *base_out);
void gdt_load_cpu(int cpu);

#endif
