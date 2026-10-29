# Operating System — top-level build. Run inside WSL/Linux:
#   make          build the kernel and bootable ISO
#   make run      boot it in QEMU (UEFI) with a window
#   make test     headless boot test (serial log assertions)

ARCH        ?= x86_64
LIMINE_DIR  := vendor/limine
ifeq ($(ARCH),arm64)
BUILD       := build/arm64
else
BUILD       := build
endif
ISO         := $(BUILD)/os.iso
KERNEL      := $(BUILD)/kernel.elf
INITRD      := $(BUILD)/initrd.cpio
BASE_CPIO   := $(BUILD)/base.cpio
BASE_DIR    ?= $(HOME)/osbuild/base
# Bundled applications (not part of the core image). Prism is the user's own
# IDE, shipped as an AppImage; the newest build in its dist folder is used.
MUSL_CC     ?= musl-gcc
# Development-only: Ubuntu's static busybox (GPL) gives us a shell and the
# usual tools to exercise the Linux ABI. It is never part of a release image.
BUSYBOX     ?= /bin/busybox
OVMF        ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd \
                 /opt/homebrew/share/qemu/edk2-x86_64-code.fd /usr/local/share/qemu/edk2-x86_64-code.fd))
OVMF_VARS   ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_VARS_4M.fd \
                 /opt/homebrew/share/qemu/edk2-i386-vars.fd /usr/local/share/qemu/edk2-i386-vars.fd))
AAVMF       ?= $(firstword $(wildcard /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
                 /opt/homebrew/share/qemu/edk2-aarch64-code.fd /usr/local/share/qemu/edk2-aarch64-code.fd))
AAVMF_VARS  ?= $(firstword $(wildcard /usr/share/AAVMF/AAVMF_VARS.fd \
                 /opt/homebrew/share/qemu/edk2-arm-vars.fd /usr/local/share/qemu/edk2-arm-vars.fd))
ifeq ($(ARCH),arm64)
QEMU        ?= qemu-system-aarch64
QEMU_FLAGS  ?= -M virt,gic-version=3 -accel $(if $(filter Darwin,$(shell uname -s)),hvf,kvm) -cpu host -smp 4 -m 2G
DOCKER_PLATFORM := linux/arm64
BUILDER     := os-builder-arm64
else
QEMU        ?= qemu-system-x86_64
QEMU_FLAGS  ?= -M q35 -m 2G -smp 4 -cpu max -no-reboot -netdev user,id=n0 -device e1000,netdev=n0
DOCKER_PLATFORM := linux/amd64
BUILDER     := os-builder
endif
# The persistent disk for `make run`: /dev/sda inside the OS, created empty
# (1 GiB, sparse) on first use. `make clean` keeps it; delete it by hand to
# start over as a new person.
DISK_IMG    ?= $(BUILD)/os-disk.img
DISK_FLAGS  := -drive id=disk0,file=$(DISK_IMG),format=raw,if=none -device ide-hd,drive=disk0,bus=ide.0

.PHONY: all kernel ports userland initrd iso run run-bios test test-persist clean distclean

all: iso

ifeq ($(shell uname -s),Darwin)
# The build needs a Linux toolchain for the target, so on macOS we run it in an Ubuntu container.
kernel ports userland initrd base iso:
	docker build -q --platform $(DOCKER_PLATFORM) -t $(BUILDER) - < scripts/builder.Dockerfile >/dev/null
	docker run --rm --platform $(DOCKER_PLATFORM) -v "$(CURDIR)":/os -w /os $(BUILDER) make $@$(if $(filter-out x86_64,$(ARCH)), ARCH=$(ARCH))
else
kernel:
	$(MAKE) -C kernel BUILD=../$(BUILD)/kernel OUT=../$(KERNEL)

ports:
	bash ports/build-ports.sh

userland: ports
	$(MAKE) -C userland OUT=../$(BUILD)/userland PORTS=../$(BUILD)/ports

initrd: userland
	rm -rf $(BUILD)/initrd
	mkdir -p $(BUILD)/initrd/sbin $(BUILD)/initrd/bin $(BUILD)/initrd/etc $(BUILD)/initrd/tmp \
	         $(BUILD)/initrd/root $(BUILD)/initrd/usr/bin $(BUILD)/initrd/usr/sbin \
	         $(BUILD)/initrd/usr/share/fonts
	cp $(BUILD)/userland/init $(BUILD)/initrd/sbin/init
	cp $(BUILD)/userland/os-ai $(BUILD)/initrd/bin/os-ai
	cp $(BUILD)/userland/os-audio $(BUILD)/initrd/bin/os-audio
	ln -sf os-audio $(BUILD)/initrd/bin/os-play
	ln -sf os-audio $(BUILD)/initrd/bin/os-rec
	[ ! -f $(BUILD)/userland/os-shell ] || cp $(BUILD)/userland/os-shell $(BUILD)/initrd/bin/os-shell
	[ ! -d $(BUILD)/ports/fonts ] || cp $(BUILD)/ports/fonts/*.ttf $(BUILD)/initrd/usr/share/fonts/
	cp $(BUSYBOX) $(BUILD)/initrd/bin/busybox
	for app in $$($(BUSYBOX) --list); do \
		[ -e $(BUILD)/initrd/bin/$$app ] || ln -s busybox $(BUILD)/initrd/bin/$$app; done
	cp -r userland/rootfs/. $(BUILD)/initrd/
	chmod +x $(BUILD)/initrd/etc/hello.sh
	chmod +x $(BUILD)/initrd/usr/libexec/os/*
	chmod +x $(BUILD)/initrd/usr/bin/run-appimage
	cd $(BUILD)/initrd && find . | LC_ALL=C sort | cpio -o -H newc --quiet > ../initrd.cpio

$(LIMINE_DIR):
	git clone --depth=1 --branch=v10.x-binary https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR)

# The base system (scripts/build-base.sh): packed when its tree changes.
$(BASE_CPIO): $(shell [ -f $(BASE_DIR)/.stamp ] && echo $(BASE_DIR)/.stamp)
	# through dd in big blocks: cpio writes 512 bytes at a time, which crawls on a Windows drive under WSL
	if [ -d $(BASE_DIR)/usr ]; then cd $(BASE_DIR) && find . -path ./.stamp -prune -o -print | LC_ALL=C sort | cpio -o -H newc --quiet | dd of=$(abspath $(BASE_CPIO)) bs=4M iflag=fullblock status=none; else : > $@; fi

base: $(BASE_CPIO)

ifeq ($(ARCH),arm64)
# UEFI only, with the kernel alone: there is no arm64 user space.
iso: kernel $(LIMINE_DIR)
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel.elf
	sed '/module_path/d' boot/limine.conf > $(BUILD)/iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTAA64.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $(ISO) 2>/dev/null
else
iso: kernel initrd base $(LIMINE_DIR)
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/kernel.elf
	cp $(INITRD) $(BUILD)/iso_root/boot/initrd.cpio
	[ ! -s $(BASE_CPIO) ] || cp $(BASE_CPIO) $(BUILD)/iso_root/boot/base.cpio
	cp boot/limine.conf $(BUILD)/iso_root/boot/limine/
	[ -s $(BASE_CPIO) ] || sed -i '/base.cpio/d' $(BUILD)/iso_root/boot/limine/limine.conf
	# The one model that comes with the system, small, for when there is no
	# internet (others are downloaded; scripts/get-builtin-model.sh fetches
	# this one to build/mind/builtin.gguf), loaded in place as a file
	if [ -s $(BUILD)/mind/builtin.gguf ]; then \
		mkdir -p $(BUILD)/iso_root/boot/files; \
		cp $(BUILD)/mind/builtin.gguf $(BUILD)/iso_root/boot/files/mind.gguf; \
		printf '    module_path: boot():/boot/files/mind.gguf\n    module_string: /usr/share/os/mind/model.gguf\n' \
			>> $(BUILD)/iso_root/boot/limine/limine.conf; \
	fi
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	   $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(BUILD)/iso_root -o $(ISO) 2>/dev/null
	$(LIMINE_DIR)/limine bios-install $(ISO) 2>/dev/null
endif
endif

# On a Windows drive under WSL (/mnt/c) NTFS keeps the file sparse only if
# told so before it grows; elsewhere fsutil.exe is absent and truncate is enough.
$(DISK_IMG):
	mkdir -p $(dir $@)
	: > $@
	-fsutil.exe sparse setflag "$$(wslpath -w $(abspath $@))" >/dev/null 2>&1
	truncate -s 1G $@

ifeq ($(ARCH),arm64)
run: iso
	cp $(AAVMF_VARS) $(BUILD)/aavmf_vars.fd
	$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,readonly=on,file=$(AAVMF) \
		-drive if=pflash,format=raw,file=$(BUILD)/aavmf_vars.fd -cdrom $(ISO) -serial stdio
else
run: iso | $(DISK_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd $(DISK_FLAGS) -cdrom $(ISO) -serial stdio
endif

run-bios: iso | $(DISK_IMG)
	$(QEMU) $(QEMU_FLAGS) $(DISK_FLAGS) -cdrom $(ISO) -serial stdio

test: iso
	OVMF=$(OVMF) OVMF_VARS=$(OVMF_VARS) QEMU=$(QEMU) ISO=$(ISO) tests/boot_test.sh

# Two boots on one disk image: what the first writes, the second must read.
test-persist: iso
	OVMF=$(OVMF) OVMF_VARS=$(OVMF_VARS) QEMU=$(QEMU) ISO=$(ISO) tests/disk_persist_test.sh

clean:
	find $(BUILD) -mindepth 1 -maxdepth 1 ! -name $(notdir $(DISK_IMG)) -exec rm -rf {} + 2>/dev/null || true

distclean: clean
	rm -rf vendor $(BUILD)
