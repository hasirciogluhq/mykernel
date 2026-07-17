# Paths and build layout

BUILD   := build
SRC     := src
INC     := include
TARGET  := $(BUILD)/kernel.bin
DRIVERS := $(BUILD)/drivers
USEROUT := $(BUILD)/user
INITRD  := $(DRIVERS)/initrd.img
PACKER  := $(BUILD)/pack_initrd
PACK_MKE := $(BUILD)/pack_mke
MKFATIMG := $(BUILD)/mkfatimg
DISKIMG := disk.img
DISK_MB := 64
