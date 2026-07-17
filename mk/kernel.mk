# Kernel sources, link, and generic object rules

ARCH_ASM := $(SRC)/arch/x86/boot.asm \
            $(SRC)/arch/x86/switch.asm \
            $(SRC)/arch/x86/isr.asm \
            $(SRC)/arch/x86/exc.asm \
            $(SRC)/arch/x86/gdt_flush.asm \
            $(SRC)/arch/x86/usermode.asm

ARCH_C   := $(SRC)/arch/x86/idt.c \
            $(SRC)/arch/x86/gdt.c \
            $(SRC)/arch/x86/exception.c

KERNEL_C := $(SRC)/kernel/main.c \
            $(SRC)/kernel/heap.c \
            $(SRC)/kernel/mm.c \
            $(SRC)/kernel/process.c \
            $(SRC)/kernel/env.c \
            $(SRC)/kernel/argv.c \
            $(SRC)/kernel/scheduler.c \
            $(SRC)/kernel/time.c \
            $(SRC)/kernel/syscall.c \
            $(SRC)/kernel/vfs.c \
            $(SRC)/kernel/vfs_api.c \
            $(SRC)/kernel/block_api.c \
            $(SRC)/kernel/netif.c \
            $(SRC)/kernel/netstack.c \
            $(SRC)/kernel/dhcp.c \
            $(SRC)/kernel/socket.c \
            $(SRC)/kernel/service.c \
            $(SRC)/kernel/ksym.c \
            $(SRC)/kernel/module.c \
            $(SRC)/kernel/mkdx_api.c \
            $(SRC)/kernel/mke.c \
            $(SRC)/kernel/boot_splash.c \
            $(SRC)/kernel/initrd_store.c \
            $(SRC)/kernel/uaccess.c

LIB_C    := $(SRC)/lib/string.c \
            $(SRC)/lib/libgcc.c

DRV_C    := $(SRC)/drivers/driver.c \
            $(SRC)/drivers/internal.c \
            $(SRC)/drivers/vga.c \
            $(SRC)/drivers/serial.c \
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

$(TARGET): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODCFLAGS) -c $< -o $@
