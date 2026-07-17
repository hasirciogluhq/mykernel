# mykernel — bare-metal i386 Multiboot + MKDX (.kmod) + usermode C++ (.mke)

include mk/config.mk
include mk/toolchain.mk
include mk/kernel.mk
include mk/drivers.mk
include mk/userapps.mk
include mk/tools.mk
include mk/qemu.mk

.PHONY: all drivers userapps run clean disk disk-reset

all: $(TARGET) $(INITRD) $(DISKIMG)

drivers: $(KMODS)

userapps: $(MKES)

clean:
	rm -rf $(BUILD)
