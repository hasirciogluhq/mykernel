# QEMU run target — SMP kernel (3 vCPUs: 1 BSP + 2 APs).
run: $(TARGET) $(INITRD) $(DISKIMG)
	$(QEMU) -kernel $(TARGET) -initrd $(INITRD) -m 1G -smp 3,sockets=1,cores=3,threads=1 -vga none \
		-serial stdio \
		-drive if=none,id=vd0,file=$(DISKIMG),format=raw,cache=writethrough \
		-device virtio-blk-pci,drive=vd0,disable-legacy=on \
		-device virtio-gpu-pci,xres=1920,yres=1080,bus=pci.0 \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on
