#pragma once
#include <asm/regs.h>
#include <stdbool.h>
#include <stdint.h>

/* Interrupt vectors. 0-31 are CPU exceptions. */
#define VEC_TIMER    0x20
#define VEC_RESCHED  0x21
#define VEC_TLB      0x22
#define VEC_HDA      0x2c
#define VEC_SPURIOUS 0xff

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void idt_load(void);
void idt_set_handler(uint8_t vector, irq_handler_t handler);

/* APIC: local APIC on every CPU, I/O APIC for device interrupts. */
void apic_init(void);       /* BSP: parse MADT, disable 8259, calibrate timer */
void lapic_init_cpu(void);  /* every CPU: enable local APIC + periodic timer */
void lapic_eoi(void);
uint32_t lapic_id(void);
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic, bool level, bool active_low);
uint32_t irq_to_gsi(uint8_t isa_irq, bool *level, bool *active_low);

static inline void irq_set_handler(unsigned irq, irq_handler_t handler) { idt_set_handler((uint8_t)irq, handler); }
static inline void irq_eoi(void) { lapic_eoi(); }

/* SMP */
void smp_init(void);
