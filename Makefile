# mykernel — bare-metal i386 Multiboot + MKDX (.kmod) + usermode (.mke)

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
LD      := i686-elf-ld
OBJCOPY := i686-elf-objcopy
NM      := i686-elf-nm
HOSTCC  := cc
QEMU    := qemu-system-i386

ASFLAGS := -f elf32
CFLAGS  := -std=c11 -ffreestanding -m32 -Wall -Wextra -Werror \
           -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
           -fno-common -I$(INC)
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
            $(SRC)/kernel/process.c \
            $(SRC)/kernel/scheduler.c \
            $(SRC)/kernel/syscall.c \
            $(SRC)/kernel/vfs.c \
            $(SRC)/kernel/ksym.c \
            $(SRC)/kernel/module.c \
            $(SRC)/kernel/mkdx_api.c \
            $(SRC)/kernel/mke.c \
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

BGA_OBJS    := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(BGA_SRCS))
VIRTIO_OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(VIRTIO_SRCS))
MKDX_OBJS   := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(MKDX_SRCS))

KMOD_BGA    := $(DRIVERS)/display_bga.kmod
KMOD_VIRTIO := $(DRIVERS)/display_virtio.kmod
KMOD_MKDX   := $(DRIVERS)/mkdx.kmod
KMODS       := $(KMOD_BGA) $(KMOD_VIRTIO) $(KMOD_MKDX)

# ---- usermode .mke apps ----
USER_LIB_SRCS := $(SRC)/user/libgx.c $(SRC)/user/string.c
USER_LIB_OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(USER_LIB_SRCS))

MKE_OSUI     := $(USEROUT)/os-ui.mke
MKE_TERMINAL := $(USEROUT)/terminal.mke
MKE_NOTEPAD  := $(USEROUT)/notepad.mke
MKES         := $(MKE_OSUI) $(MKE_TERMINAL) $(MKE_NOTEPAD)

LOAD_OSUI     := 0x02000000
LOAD_TERMINAL := 0x02800000
LOAD_NOTEPAD  := 0x03000000

.PHONY: all drivers userapps run clean

all: $(TARGET) $(INITRD)

drivers: $(KMODS)

userapps: $(MKES)

$(TARGET): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KMOD_BGA): $(BGA_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(MODLDFLAGS) -o $@ $^

$(KMOD_VIRTIO): $(VIRTIO_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(MODLDFLAGS) -o $@ $^

$(KMOD_MKDX): $(MKDX_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(MODLDFLAGS) -o $@ $^

$(PACKER): tools/pack_initrd.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

$(PACK_MKE): tools/pack_mke.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

# Pack flat image + MKE1 header. Args: elf bin out load name
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

$(USEROUT)/os-ui.elf: $(BUILD)/user/os-ui.o $(USER_LIB_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_OSUI) -o $@ \
		$(BUILD)/user/os-ui.o $(USER_LIB_OBJS)

$(USEROUT)/terminal.elf: $(BUILD)/user/terminal.o $(USER_LIB_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_TERMINAL) -o $@ \
		$(BUILD)/user/terminal.o $(USER_LIB_OBJS)

$(USEROUT)/notepad.elf: $(BUILD)/user/notepad.o $(USER_LIB_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_NOTEPAD) -o $@ \
		$(BUILD)/user/notepad.o $(USER_LIB_OBJS)

$(MKE_OSUI): $(USEROUT)/os-ui.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/os-ui.bin,$@,$(LOAD_OSUI),os-ui)

$(MKE_TERMINAL): $(USEROUT)/terminal.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/terminal.bin,$@,$(LOAD_TERMINAL),terminal)

$(MKE_NOTEPAD): $(USEROUT)/notepad.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/notepad.bin,$@,$(LOAD_NOTEPAD),notepad)

$(INITRD): $(PACKER) $(KMODS) $(MKES)
	$(PACKER) $@ $(KMOD_BGA) $(KMOD_VIRTIO) $(KMOD_MKDX) $(MKES)

$(BUILD)/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# User objects before generic C rule (must use USERCFLAGS)
$(BUILD)/user/%.o: $(SRC)/user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODCFLAGS) -c $< -o $@

run: $(TARGET) $(INITRD)
	$(QEMU) -kernel $(TARGET) -initrd $(INITRD) -m 128M -vga std

clean:
	rm -rf $(BUILD)
