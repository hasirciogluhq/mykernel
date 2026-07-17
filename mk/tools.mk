# Host tools, disk image, initrd

ENV_ASSET := $(BUILD)/environment
# Desktop wallpaper for mkdx (BMP; decoded by load_default_wallpaper).
# Source art: assets/os/default-wallpaper.png — regenerate with:
#   magick assets/os/default-wallpaper.png -resize '1280x720^' -gravity center \
#     -extent 1280x720 -type TrueColor BMP3:assets/os/wallpaper-default.bmp
INITRD_ASSETS := assets/os/wallpaper-default.bmp $(ENV_ASSET)

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

# Rebuild Inter AA bitmap for Surface::text (host tool; not part of default build).
BAKE_UGX_FONT := $(BUILD)/bake_ugx_font
$(BAKE_UGX_FONT): tools/bake_ugx_font.c tools/stb_truetype.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ tools/bake_ugx_font.c -lm

.PHONY: ugx-font
ugx-font: $(BAKE_UGX_FONT)
	$(BAKE_UGX_FONT) assets/fonts/Inter-Regular.ttf src/user/ugx_font.inc 16.0

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
