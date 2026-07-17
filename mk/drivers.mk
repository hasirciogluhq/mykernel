# Loadable drivers (.kmod = relocatable ELF)
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
VIRTIO_NET_SRCS := $(SRC)/drivers/net/virtio_net/virtio_net.c

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
             $(SRC)/drivers/mkdx/console.c \
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
VIRTIO_NET_OBJS := $(call kmod_objs,VIRTIO_NET_SRCS)
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
KMOD_VIRTIO_NET := $(DRIVERS)/virtio_net.kmod
KMOD_BGA    := $(DRIVERS)/display_bga.kmod
KMOD_VIRTIO := $(DRIVERS)/display_virtio.kmod
KMOD_MKDX   := $(DRIVERS)/mkdx.kmod

# Load order: FS/block first, then display/MKDX (before net — keeps heap for FB), then net
KMODS := \
	$(KMOD_BLOCK) $(KMOD_VFS) \
	$(KMOD_PART_GPT) $(KMOD_PART_MBR) \
	$(KMOD_RAMFS) $(KMOD_DEVTMPFS) \
	$(KMOD_RAMDISK) $(KMOD_LOOP) $(KMOD_VIRTIO_BLK) \
	$(KMOD_FAT) \
	$(KMOD_TMPFS) $(KMOD_PROCFS) $(KMOD_SYSFS) $(KMOD_INITRDFS) \
	$(KMOD_EXFAT) $(KMOD_EXT) $(KMOD_ISO9660) $(KMOD_UDF) $(KMOD_NTFS) \
	$(KMOD_AHCI) $(KMOD_NVME) \
	$(KMOD_BGA) $(KMOD_VIRTIO) $(KMOD_MKDX) \
	$(KMOD_VIRTIO_NET)

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
$(eval $(call link_kmod,$(KMOD_VIRTIO_NET),$(VIRTIO_NET_OBJS)))
$(eval $(call link_kmod,$(KMOD_BGA),$(BGA_OBJS)))
$(eval $(call link_kmod,$(KMOD_VIRTIO),$(VIRTIO_OBJS)))
$(eval $(call link_kmod,$(KMOD_MKDX),$(MKDX_OBJS)))
