# mykernel — bare-metal i386 Multiboot + MKDX graphics

BUILD   := build
SRC     := src
INC     := include
TARGET  := $(BUILD)/kernel.bin

AS      := nasm
CC      := i686-elf-gcc
LD      := i686-elf-ld
QEMU    := qemu-system-i386

ASFLAGS := -f elf32
CFLAGS  := -std=c11 -ffreestanding -m32 -Wall -Wextra -Werror \
           -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
           -I$(INC)
LDFLAGS := -m elf_i386 -n -T linker.ld -nostdlib

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
            $(SRC)/kernel/vfs.c

LIB_C    := $(SRC)/lib/string.c

DRV_C    := $(SRC)/drivers/vga.c \
            $(SRC)/drivers/fb.c \
            $(SRC)/drivers/console.c \
            $(SRC)/drivers/keyboard.c \
            $(SRC)/drivers/ps2.c \
            $(SRC)/drivers/mouse.c

GFX_C    := $(SRC)/gfx/surface.c \
            $(SRC)/gfx/draw.c \
            $(SRC)/gfx/blur.c \
            $(SRC)/gfx/font.c \
            $(SRC)/gfx/device.c \
            $(SRC)/gfx/accel.c \
            $(SRC)/gfx/compositor.c \
            $(SRC)/gfx/window.c \
            $(SRC)/gfx/server.c

USER_C   := $(SRC)/user/libgx.c \
            $(SRC)/user/os-ui.c

ASM_SRCS := $(ARCH_ASM)
C_SRCS   := $(ARCH_C) $(KERNEL_C) $(LIB_C) $(DRV_C) $(GFX_C) $(USER_C)

ASM_OBJS := $(patsubst $(SRC)/%.asm,$(BUILD)/%.o,$(ASM_SRCS))
C_OBJS   := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(C_SRCS))
OBJS     := $(ASM_OBJS) $(C_OBJS)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	$(QEMU) -kernel $< -m 128M -vga std

clean:
	rm -rf $(BUILD)
