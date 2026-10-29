#pragma once

/* We target QEMU's `virt` machine, whose device addresses are fixed
 * (hw/arm/virt.c, with its default high PCIe ECAM window). */
#define VIRT_UART     0x09000000ULL
#define VIRT_UART_IRQ 33 /* SPI 1 */
#define VIRT_RTC      0x09010000ULL
#define VIRT_FW_CFG   0x09020000ULL
#define VIRT_PCIE_PIO 0x3eff0000ULL
#define VIRT_PCIE_PIO_SIZE 0x10000ULL
#define VIRT_PCIE_ECAM 0x4010000000ULL
