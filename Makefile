# mykernel — bare-metal i386 Multiboot + MKDX (.kmod) + usermode C++ (.mke)

BUILD   := build
SRC     := src
INC     := include
TARGET  := $(BUILD)/kernel.bin
DRIVERS := $(BUILD)/drivers
USEROUT := $(BUILD)/user
INITRD  := $(DRIVERS)/initrd.img
PACKER  := $(BUILD)/pack_initrd
PACK_MKE := $(BUILD)/pack_mke

AS      := nasm
CC      := i686-elf-gcc
CXX     := i686-elf-g++
LD      := i686-elf-ld
OBJCOPY := i686-elf-objcopy
NM      := i686-elf-nm
HOSTCC  := cc
QEMU    := qemu-system-i386

ASFLAGS := -f elf32
CFLAGS  := -std=c11 -ffreestanding -m32 -Wall -Wextra -Werror \
           -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
           -fno-common -I$(INC)
CXXFLAGS := -std=c++17 -ffreestanding -m32 -Wall -Wextra -Werror \
            -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
            -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics \
            -I$(INC) -DUSERMODE
MODCFLAGS := $(CFLAGS) -I$(SRC)/drivers/mkdx \
             -I$(SRC)/drivers/display/bga \
             -I$(SRC)/drivers/display/virtio_gpu
USERCFLAGS := $(CFLAGS) -DUSERMODE
LDFLAGS := -m elf_i386 -n -T linker.ld -nostdlib
MODLDFLAGS := -m elf_i386 -r
USERLDFLAGS := -m elf_i386 -nostdlib -T user.ld

ARCH_ASM := $(SRC)/arch/x86/boot.asm \
            $(SRC)/arch/x86/switch.asm \
            $(SRC)/arch/x86/isr.asm \
            $(SRC)/arch/x86/gdt_flush.asm \
            $(SRC)/arch/x86/usermode.asm

ARCH_C   := $(SRC)/arch/x86/idt.c \
            $(SRC)/arch/x86/gdt.c

KERNEL_C := $(SRC)/kernel/main.c \
            $(SRC)/kernel/heap.c \
            $(SRC)/kernel/mm.c \
            $(SRC)/kernel/process.c \
            $(SRC)/kernel/scheduler.c \
            $(SRC)/kernel/syscall.c \
            $(SRC)/kernel/vfs.c \
            $(SRC)/kernel/vfs_api.c \
            $(SRC)/kernel/block_api.c \
            $(SRC)/kernel/ksym.c \
            $(SRC)/kernel/module.c \
            $(SRC)/kernel/mkdx_api.c \
            $(SRC)/kernel/mke.c \
            $(SRC)/kernel/initrd_store.c \
            $(SRC)/kernel/uaccess.c

LIB_C    := $(SRC)/lib/string.c

DRV_C    := $(SRC)/drivers/driver.c \
            $(SRC)/drivers/internal.c \
            $(SRC)/drivers/vga.c \
            $(SRC)/drivers/console.c \
            $(SRC)/drivers/keyboard.c \
            $(SRC)/drivers/ps2.c \
            $(SRC)/drivers/mouse.c \
            $(SRC)/drivers/pci/pci.c \
            $(SRC)/drivers/display/display.c

ASM_SRCS := $(ARCH_ASM)
C_SRCS   := $(ARCH_C) $(KERNEL_C) $(LIB_C) $(DRV_C)

ASM_OBJS := $(patsubst $(SRC)/%.asm,$(BUILD)/%.o,$(ASM_SRCS))
C_OBJS   := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(C_SRCS))
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ---- loadable drivers (.kmod = relocatable ELF) ----
# VFS/block stack first (initrd pack order below).
BLOCK_SRCS := $(SRC)/drivers/block/block.c
VFS_SRCS   := $(SRC)/drivers/vfs/vfs_core.c \
              $(SRC)/drivers/vfs/vfs_dcache.c \
              $(SRC)/drivers/vfs/vfs_pcache.c \
              $(SRC)/drivers/vfs/vfs_xattr.c \
              $(SRC)/drivers/vfs/vfs_flock.c \
              $(SRC)/drivers/vfs/vfs_fsnotify.c
PART_GPT_SRCS := $(SRC)/drivers/part/gpt/part_gpt.c
PART_MBR_SRCS := $(SRC)/drivers/part/mbr/part_mbr.c
RAMFS_SRCS := $(SRC)/drivers/fs/ramfs/ramfs.c
DEVTMPFS_SRCS := $(SRC)/drivers/fs/devtmpfs/devtmpfs.c
RAMDISK_SRCS := $(SRC)/drivers/block/ramdisk/ramdisk.c
LOOP_SRCS := $(SRC)/drivers/block/loop/loop.c
VIRTIO_BLK_SRCS := $(SRC)/drivers/block/virtio_blk/virtio_blk.c
FAT_SRCS := $(SRC)/drivers/fs/fat/fat.c
TMPFS_SRCS := $(SRC)/drivers/fs/tmpfs/tmpfs.c
PROCFS_SRCS := $(SRC)/drivers/fs/procfs/procfs.c
SYSFS_SRCS := $(SRC)/drivers/fs/sysfs/sysfs.c
INITRDFS_SRCS := $(SRC)/drivers/fs/initrdfs/initrdfs.c
EXFAT_SRCS := $(SRC)/drivers/fs/exfat/exfat.c
EXT_SRCS := $(SRC)/drivers/fs/ext/ext.c
ISO9660_SRCS := $(SRC)/drivers/fs/iso9660/iso9660.c
UDF_SRCS := $(SRC)/drivers/fs/udf/udf.c
NTFS_SRCS := $(SRC)/drivers/fs/ntfs/ntfs.c
AHCI_SRCS := $(SRC)/drivers/block/ahci/ahci.c
NVME_SRCS := $(SRC)/drivers/block/nvme/nvme.c

BGA_SRCS := $(SRC)/drivers/display/bga/bga.c
VIRTIO_SRCS := $(SRC)/drivers/display/virtio_gpu/virtio_gpu.c \
               $(SRC)/drivers/display/virtio_gpu/virtio_pci.c
MKDX_SRCS := $(SRC)/drivers/mkdx/surface.c \
             $(SRC)/drivers/mkdx/draw.c \
             $(SRC)/drivers/mkdx/blur.c \
             $(SRC)/drivers/mkdx/font.c \
             $(SRC)/drivers/mkdx/device.c \
             $(SRC)/drivers/mkdx/accel.c \
             $(SRC)/drivers/mkdx/compositor.c \
             $(SRC)/drivers/mkdx/window.c \
             $(SRC)/drivers/mkdx/server.c \
             $(SRC)/drivers/mkdx/context.c \
             $(SRC)/drivers/mkdx/render3d.c \
             $(SRC)/drivers/mkdx/mkdx_mod.c

define kmod_objs
$(patsubst $(SRC)/%.c,$(BUILD)/%.o,$($(1)))
endef

BLOCK_OBJS := $(call kmod_objs,BLOCK_SRCS)
VFS_OBJS := $(call kmod_objs,VFS_SRCS)
PART_GPT_OBJS := $(call kmod_objs,PART_GPT_SRCS)
PART_MBR_OBJS := $(call kmod_objs,PART_MBR_SRCS)
RAMFS_OBJS := $(call kmod_objs,RAMFS_SRCS)
DEVTMPFS_OBJS := $(call kmod_objs,DEVTMPFS_SRCS)
RAMDISK_OBJS := $(call kmod_objs,RAMDISK_SRCS)
LOOP_OBJS := $(call kmod_objs,LOOP_SRCS)
VIRTIO_BLK_OBJS := $(call kmod_objs,VIRTIO_BLK_SRCS)
FAT_OBJS := $(call kmod_objs,FAT_SRCS)
TMPFS_OBJS := $(call kmod_objs,TMPFS_SRCS)
PROCFS_OBJS := $(call kmod_objs,PROCFS_SRCS)
SYSFS_OBJS := $(call kmod_objs,SYSFS_SRCS)
INITRDFS_OBJS := $(call kmod_objs,INITRDFS_SRCS)
EXFAT_OBJS := $(call kmod_objs,EXFAT_SRCS)
EXT_OBJS := $(call kmod_objs,EXT_SRCS)
ISO9660_OBJS := $(call kmod_objs,ISO9660_SRCS)
UDF_OBJS := $(call kmod_objs,UDF_SRCS)
NTFS_OBJS := $(call kmod_objs,NTFS_SRCS)
AHCI_OBJS := $(call kmod_objs,AHCI_SRCS)
NVME_OBJS := $(call kmod_objs,NVME_SRCS)
BGA_OBJS    := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(BGA_SRCS))
VIRTIO_OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(VIRTIO_SRCS))
MKDX_OBJS   := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(MKDX_SRCS))

KMOD_BLOCK := $(DRIVERS)/block.kmod
KMOD_VFS := $(DRIVERS)/vfs.kmod
KMOD_PART_GPT := $(DRIVERS)/part_gpt.kmod
KMOD_PART_MBR := $(DRIVERS)/part_mbr.kmod
KMOD_RAMFS := $(DRIVERS)/ramfs.kmod
KMOD_DEVTMPFS := $(DRIVERS)/devtmpfs.kmod
KMOD_RAMDISK := $(DRIVERS)/ramdisk.kmod
KMOD_LOOP := $(DRIVERS)/loop.kmod
KMOD_VIRTIO_BLK := $(DRIVERS)/virtio_blk.kmod
KMOD_FAT := $(DRIVERS)/fat.kmod
KMOD_TMPFS := $(DRIVERS)/tmpfs.kmod
KMOD_PROCFS := $(DRIVERS)/procfs.kmod
KMOD_SYSFS := $(DRIVERS)/sysfs.kmod
KMOD_INITRDFS := $(DRIVERS)/initrdfs.kmod
KMOD_EXFAT := $(DRIVERS)/exfat.kmod
KMOD_EXT := $(DRIVERS)/ext.kmod
KMOD_ISO9660 := $(DRIVERS)/iso9660.kmod
KMOD_UDF := $(DRIVERS)/udf.kmod
KMOD_NTFS := $(DRIVERS)/ntfs.kmod
KMOD_AHCI := $(DRIVERS)/ahci.kmod
KMOD_NVME := $(DRIVERS)/nvme.kmod
KMOD_BGA    := $(DRIVERS)/display_bga.kmod
KMOD_VIRTIO := $(DRIVERS)/display_virtio.kmod
KMOD_MKDX   := $(DRIVERS)/mkdx.kmod

# Load order matters: block → vfs → part → pseudo FS → disks → disk FS → gfx
KMODS := \
	$(KMOD_BLOCK) $(KMOD_VFS) \
	$(KMOD_PART_GPT) $(KMOD_PART_MBR) \
	$(KMOD_RAMFS) $(KMOD_DEVTMPFS) \
	$(KMOD_RAMDISK) $(KMOD_LOOP) $(KMOD_VIRTIO_BLK) \
	$(KMOD_FAT) \
	$(KMOD_TMPFS) $(KMOD_PROCFS) $(KMOD_SYSFS) $(KMOD_INITRDFS) \
	$(KMOD_EXFAT) $(KMOD_EXT) $(KMOD_ISO9660) $(KMOD_UDF) $(KMOD_NTFS) \
	$(KMOD_AHCI) $(KMOD_NVME) \
	$(KMOD_BGA) $(KMOD_VIRTIO) $(KMOD_MKDX)

# ---- usermode C++ SDK + apps (.mke) ----
SDK_SRCS := $(SRC)/user/sdk/syscall.cpp \
            $(SRC)/user/sdk/gfx.cpp \
            $(SRC)/user/string.c
SDK_OBJS := $(BUILD)/user/sdk/syscall.o \
            $(BUILD)/user/sdk/gfx.o \
            $(BUILD)/user/string.o

MKE_OSUI := $(USEROUT)/os-ui.mke
MKE_TERM := $(USEROUT)/terminal.mke
MKES     := $(MKE_OSUI) $(MKE_TERM)
LOAD_OSUI := 0x02000000
LOAD_TERM := 0x02200000

.PHONY: all drivers userapps run clean

all: $(TARGET) $(INITRD)

drivers: $(KMODS)

userapps: $(MKES)

$(TARGET): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

define link_kmod
$(1): $(2)
	@mkdir -p $$(dir $$@)
	$$(LD) $$(MODLDFLAGS) -o $$@ $$^
endef

$(eval $(call link_kmod,$(KMOD_BLOCK),$(BLOCK_OBJS)))
$(eval $(call link_kmod,$(KMOD_VFS),$(VFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_PART_GPT),$(PART_GPT_OBJS)))
$(eval $(call link_kmod,$(KMOD_PART_MBR),$(PART_MBR_OBJS)))
$(eval $(call link_kmod,$(KMOD_RAMFS),$(RAMFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_DEVTMPFS),$(DEVTMPFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_RAMDISK),$(RAMDISK_OBJS)))
$(eval $(call link_kmod,$(KMOD_LOOP),$(LOOP_OBJS)))
$(eval $(call link_kmod,$(KMOD_VIRTIO_BLK),$(VIRTIO_BLK_OBJS)))
$(eval $(call link_kmod,$(KMOD_FAT),$(FAT_OBJS)))
$(eval $(call link_kmod,$(KMOD_TMPFS),$(TMPFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_PROCFS),$(PROCFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_SYSFS),$(SYSFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_INITRDFS),$(INITRDFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_EXFAT),$(EXFAT_OBJS)))
$(eval $(call link_kmod,$(KMOD_EXT),$(EXT_OBJS)))
$(eval $(call link_kmod,$(KMOD_ISO9660),$(ISO9660_OBJS)))
$(eval $(call link_kmod,$(KMOD_UDF),$(UDF_OBJS)))
$(eval $(call link_kmod,$(KMOD_NTFS),$(NTFS_OBJS)))
$(eval $(call link_kmod,$(KMOD_AHCI),$(AHCI_OBJS)))
$(eval $(call link_kmod,$(KMOD_NVME),$(NVME_OBJS)))
$(eval $(call link_kmod,$(KMOD_BGA),$(BGA_OBJS)))
$(eval $(call link_kmod,$(KMOD_VIRTIO),$(VIRTIO_OBJS)))
$(eval $(call link_kmod,$(KMOD_MKDX),$(MKDX_OBJS)))

$(PACKER): tools/pack_initrd.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

$(PACK_MKE): tools/pack_mke.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

define pack_mke_from_elf
	@mkdir -p $(dir $(3))
	$(OBJCOPY) -O binary $(1) $(2)
	ENTRY=$$($(NM) -n $(1) | awk '/ mke_main$$/{print $$1}'); \
	EDATA=$$($(NM) -n $(1) | awk '/ _mke_edata$$/{print $$1}'); \
	END=$$($(NM) -n $(1) | awk '/ _mke_end$$/{print $$1}'); \
	ENTRY_OFF=$$((0x$$ENTRY - $(4))); \
	IMG=$$((0x$$EDATA - $(4))); \
	BSS=$$((0x$$END - 0x$$EDATA)); \
	$(PACK_MKE) $(3) $(2) $(4) $$ENTRY_OFF $$IMG $$BSS $(5)
endef

$(USEROUT)/os-ui.elf: $(BUILD)/user/apps/os-ui.o $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_OSUI) -o $@ \
		$(BUILD)/user/apps/os-ui.o $(SDK_OBJS)

$(MKE_OSUI): $(USEROUT)/os-ui.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/os-ui.bin,$@,$(LOAD_OSUI),os-ui)

$(USEROUT)/terminal.elf: $(BUILD)/user/apps/terminal.o $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_TERM) -o $@ \
		$(BUILD)/user/apps/terminal.o $(SDK_OBJS)

$(MKE_TERM): $(USEROUT)/terminal.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/terminal.bin,$@,$(LOAD_TERM),terminal)

$(INITRD): $(PACKER) $(KMODS) $(MKES)
	$(PACKER) $@ $(KMODS) $(MKES)

$(BUILD)/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/user/%.o: $(SRC)/user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(BUILD)/user/%.o: $(SRC)/user/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODCFLAGS) -c $< -o $@

run: $(TARGET) $(INITRD)
	$(QEMU) -kernel $(TARGET) -initrd $(INITRD) -m 128M -vga std

clean:
	rm -rf $(BUILD)
