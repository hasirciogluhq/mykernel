# Toolchain and compiler flags

AS      := nasm
CC      := i686-elf-gcc
CXX     := i686-elf-g++
LD      := i686-elf-ld
OBJCOPY := i686-elf-objcopy
NM      := i686-elf-nm
HOSTCC  := cc
QEMU    := qemu-system-i386

ASFLAGS := -f elf32
CFLAGS  := -std=c11 -ffreestanding -m32 -Wall -Wextra -Werror \
           -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
           -fno-common -I$(INC)
CXXFLAGS := -std=c++17 -ffreestanding -m32 -Wall -Wextra -Werror \
            -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
            -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics \
            -I$(INC) -DUSERMODE

# Separate usermode app tree (apps/imgui-demo) — C++23 + freestanding shims
IMGUI_DEMO_DIR := apps/imgui-demo
IMGUI_DIR      := $(IMGUI_DEMO_DIR)/third_party/imgui
IMGUI_CXXFLAGS := -std=c++23 -ffreestanding -m32 -Wall -Wextra \
                  -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers \
                  -Wno-invalid-offsetof -Wno-class-memaccess \
                  -O2 -fno-stack-protector -fno-pic -fno-builtin -nostdlib \
                  -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics \
                  -fcoroutines \
                  -I$(INC) -I$(IMGUI_DEMO_DIR) -I$(IMGUI_DEMO_DIR)/freestanding -I$(IMGUI_DIR) \
                  -DUSERMODE
MODCFLAGS := $(CFLAGS) -I$(SRC)/drivers/mkdx \
             -I$(SRC)/drivers/display/bga \
             -I$(SRC)/drivers/display/virtio_gpu
USERCFLAGS := $(CFLAGS) -DUSERMODE
LDFLAGS := -m elf_i386 -n -T linker.ld -nostdlib
MODLDFLAGS := -m elf_i386 -r
USERLDFLAGS := -m elf_i386 -nostdlib -T user.ld
