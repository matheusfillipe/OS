# Freestanding kernel; ARCH selects arch/$(ARCH). The host gcc is fine for now because we pass
# -ffreestanding and our own linker script; a dedicated cross toolchain arrives
# with the musl userland (milestone 2).

BUILD ?= build
OUT   ?= $(BUILD)/kernel.elf
CC    ?= gcc
LD    ?= ld
ARCH  ?= x86_64

include arch/$(ARCH)/arch.mk

CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
          -fno-pic -fno-pie -fno-omit-frame-pointer \
          $(ARCH_CFLAGS) -Iinclude -Iarch/$(ARCH)/include -Ithird_party -MMD -MP
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 $(ARCH_LDFLAGS) -T arch/$(ARCH)/linker.ld

SRCS := $(shell find . \( -name '*.c' -o -name '*.S' \) -not -path './arch/*' -not -path './platform/*' \
                -not -path './third_party/*') \
        $(shell find arch/$(ARCH) $(ARCH_DIRS) \( -name '*.c' -o -name '*.S' \) | sed 's|^|./|')
OBJS := $(patsubst ./%,$(BUILD)/%.o,$(SRCS))

$(OUT): $(OBJS) arch/$(ARCH)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)
