# mykernel — bare-metal i386 Multiboot + MKDX (.kmod) graphics

BUILD   := build
SRC     := src
INC     := include
TARGET  := $(BUILD)/kernel.bin
DRIVERS := $(BUILD)/drivers
INITRD  := $(DRIVERS)/initrd.img
PACKER  := $(BUILD)/pack_initrd

AS      := nasm
CC      := i686-elf-gcc
LD      := i686-elf-ld
HOSTCC  := cc
QEMU    := qemu-system-i386

ASFLAGS := -f elf32
CFLAGS  := -std=c11 -ffreestanding -m32 -Wall -Wextra -Werror \
           -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
           -fno-common -I$(INC)
MODCFLAGS := $(CFLAGS) -I$(SRC)/drivers/mkdx \
             -I$(SRC)/drivers/display/bga \
             -I$(SRC)/drivers/display/virtio_gpu
LDFLAGS := -m elf_i386 -n -T linker.ld -nostdlib
MODLDFLAGS := -m elf_i386 -r

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
            $(SRC)/kernel/mkdx_api.c

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

USER_C   := $(SRC)/user/libgx.c \
            $(SRC)/user/os-ui.c \
            $(SRC)/user/terminal.c \
            $(SRC)/user/notepad.c

ASM_SRCS := $(ARCH_ASM)
C_SRCS   := $(ARCH_C) $(KERNEL_C) $(LIB_C) $(DRV_C) $(USER_C)

ASM_OBJS := $(patsubst $(SRC)/%.asm,$(BUILD)/%.o,$(ASM_SRCS))
C_OBJS   := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(C_SRCS))
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ---- loadable drivers (.kmod = relocatable ELF) ----
BGA_SRCS := $(SRC)/drivers/display/bga/bga.c
VIRTIO_SRCS := $(SRC)/drivers/display/virtio_gpu/virtio_pci.c \
               $(SRC)/drivers/display/virtio_gpu/virtio_gpu.c
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

.PHONY: all drivers run run-bga run-virtio clean

all: $(TARGET) $(INITRD)

drivers: $(KMODS) $(INITRD)

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

$(INITRD): $(PACKER) $(KMODS)
	$(PACKER) $@ $(KMOD_BGA) $(KMOD_VIRTIO) $(KMOD_MKDX)

$(BUILD)/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODCFLAGS) -c $< -o $@

# Both backends present: virtio-gpu wins (DISPLAY_PRIO_VIRTIO > BGA)
run: $(TARGET) $(INITRD)
	$(QEMU) -kernel $(TARGET) -initrd $(INITRD) -m 128M \
		-vga std -device virtio-gpu-pci

# BGA only
run-bga: $(TARGET) $(INITRD)
	$(QEMU) -kernel $(TARGET) -initrd $(INITRD) -m 128M -vga std

# Virtio-vga only (no Bochs BGA)
run-virtio: $(TARGET) $(INITRD)
	$(QEMU) -kernel $(TARGET) -initrd $(INITRD) -m 128M -vga virtio

clean:
	rm -rf $(BUILD)
