#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request boot_rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

/* Just enough ACPI to find static tables (MADT, HPET, MCFG, FADT). The AML
 * interpreter (uACPI) arrives with power management. */

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
} __attribute__((packed));

static struct sdt_header *root;
static bool root_is_xsdt;

static bool checksum_ok(const void *p, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += ((const uint8_t *)p)[i];
    return sum == 0;
}

static struct sdt_header *map_table(uint64_t phys) {
    vmm_map_mmio(phys, sizeof(struct sdt_header), false);
    struct sdt_header *h = phys_to_virt(phys);
    vmm_map_mmio(phys, h->length, false);
    return h;
}

void acpi_init(void) {
    struct limine_rsdp_response *r = boot_rsdp_request.response;
    if (!r || !r->address)
        panic("acpi: firmware provided no RSDP; ACPI is required");
    uint64_t phys = (uint64_t)r->address;
    if (phys >= hhdm_offset)
        phys -= hhdm_offset; /* older protocol revisions hand out a virtual address */
    vmm_map_mmio(phys, sizeof(struct rsdp), false);
    struct rsdp *rsdp = phys_to_virt(phys);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) || !checksum_ok(rsdp, 20))
        panic("acpi: bad RSDP");

    if (rsdp->revision >= 2 && rsdp->xsdt) {
        root = map_table(rsdp->xsdt);
        root_is_xsdt = true;
    } else {
        root = map_table(rsdp->rsdt);
    }
    if (!checksum_ok(root, root->length))
        panic("acpi: bad %s checksum", root_is_xsdt ? "XSDT" : "RSDT");
    kprintf("acpi: revision %u, %.6s, %s\n", rsdp->revision, rsdp->oem, root_is_xsdt ? "XSDT" : "RSDT");
}

void *acpi_find_table(const char *sig, int index) {
    size_t entry_size = root_is_xsdt ? 8 : 4;
    size_t count = (root->length - sizeof *root) / entry_size;
    uint8_t *entries = (uint8_t *)(root + 1);
    for (size_t i = 0; i < count; i++) {
        uint64_t phys = 0;
        memcpy(&phys, entries + i * entry_size, entry_size);
        struct sdt_header *h = map_table(phys);
        if (!memcmp(h->signature, sig, 4) && index-- == 0)
            return checksum_ok(h, h->length) ? h : NULL;
    }
    return NULL;
}
