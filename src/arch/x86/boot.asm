MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
VIDEO    equ 1 << 2
FLAGS    equ MBALIGN | MEMINFO | VIDEO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    ; video mode (present because VIDEO flag is set)
    dd 0        ; mode_type: 0 = linear graphics
    dd 800      ; width
    dd 600      ; height
    dd 32       ; depth

section .text
global start
extern kernel_main

start:
    mov esp, stack_top
    ; Multiboot: eax = magic, ebx = info pointer
    push ebx
    push eax
    call kernel_main
    add esp, 8

.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:
