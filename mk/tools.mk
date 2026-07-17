# Host tools, disk image, initrd

ENV_ASSET := $(BUILD)/environment
INITRD_ASSETS := assets/wallpaper-default.bmp $(ENV_ASSET)

$(PACKER): tools/pack_initrd.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

$(ENV_ASSET): assets/etc/environment
	@mkdir -p $(dir $@)
	cp $< $@

$(PACK_MKE): tools/pack_mke.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

$(MKFATIMG): tools/mkfatimg.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

# Persistent virtio disk (kept outside build/ so `make clean` does not wipe data).
# Only create when missing — never rewrite an existing image (that wiped /root files).
$(DISKIMG): | $(MKFATIMG)
	@if [ ! -f $@ ]; then \
		echo "creating fresh $(DISKIMG) ($(DISK_MB) MiB)"; \
		$(MKFATIMG) $@ $(DISK_MB); \
	else \
		echo "keeping existing $(DISKIMG) (make disk-reset to wipe)"; \
	fi

disk: $(DISKIMG)

disk-reset:
	rm -f $(DISKIMG)
	$(MAKE) $(MKFATIMG)
	$(MKFATIMG) $(DISKIMG) $(DISK_MB)

$(INITRD): $(PACKER) $(KMODS) $(MKES) $(INITRD_ASSETS)
	$(PACKER) $@ $(KMODS) $(MKES) $(INITRD_ASSETS)
