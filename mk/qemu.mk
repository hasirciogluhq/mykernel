# QEMU run target

run: $(TARGET) $(INITRD) $(DISKIMG)
	$(QEMU) -kernel $(TARGET) -initrd $(INITRD) -m 256M -vga std \
		-serial stdio \
		-drive if=none,id=vd0,file=$(DISKIMG),format=raw,cache=writethrough \
		-device virtio-blk-pci,drive=vd0,disable-legacy=on \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on
