#include "isa.h"
#include <kernel/arch.h>

void isa_irq_route(uint8_t isa_irq, uint8_t vector) {
    bool level, active_low;
    uint32_t gsi = irq_to_gsi(isa_irq, &level, &active_low);
    ioapic_route(gsi, vector, cpus[0]->arch.lapic_id, level, active_low);
}
