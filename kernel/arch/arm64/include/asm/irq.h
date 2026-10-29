#pragma once
#include <asm/regs.h>

/* GICv3 interrupt IDs. An MSI raises an LPI, and LPIs start at 8192. */
#define VEC_HDA 8192

typedef void (*irq_handler_t)(struct regs *r);

void irq_set_handler(unsigned irq, irq_handler_t handler);
void irq_eoi(void);

/* SMP */
void smp_init(void);
