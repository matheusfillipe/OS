#pragma once
#include <asm/regs.h>

/* GICv3 interrupt IDs: SGIs 0-15, PPIs 16-31, SPIs from 32. An MSI raises an
 * LPI, and LPIs start at 8192. */
#define IRQ_RESCHED 0
#define IRQ_TIMER   27 /* EL1 virtual timer */
#define IRQ_HDA     8192

typedef void (*irq_handler_t)(struct regs *r);

/* SGIs and PPIs get enabled on each CPU by gic_init_cpu, so their handlers are set before it runs. */
void irq_set_handler(unsigned irq, irq_handler_t handler);
void irq_eoi(void);
void irq_dispatch(struct regs *r);

struct cpu;
void gic_init(void);     /* BSP: find the GIC in the MADT, set up the distributor */
void gic_init_cpu(void); /* every CPU: redistributor and CPU interface */
void gic_send_sgi(struct cpu *c, unsigned intid);

void smp_init(void);
