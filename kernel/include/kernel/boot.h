#pragma once
#include <limine.h>

/* Boot information handed over by the bootloader; valid until we reclaim
 * bootloader memory (we don't yet). */
extern volatile struct limine_framebuffer_request boot_fb_request;
extern volatile struct limine_memmap_request boot_memmap_request;
extern volatile struct limine_hhdm_request boot_hhdm_request;
extern volatile struct limine_executable_address_request boot_kaddr_request;

extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys) { return (void *)(phys + hhdm_offset); }
static inline uint64_t virt_to_phys_hhdm(const void *virt) { return (uint64_t)virt - hhdm_offset; }
static inline uint64_t kernel_virt_to_phys(const void *p) {
    struct limine_executable_address_response *ka = boot_kaddr_request.response;
    return (uint64_t)p - ka->virtual_base + ka->physical_base;
}

/* ACPI static tables, found through the RSDP the bootloader hands over. */
void acpi_init(void);
void *acpi_find_table(const char *sig, int index);
