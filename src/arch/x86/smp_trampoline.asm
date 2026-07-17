; Real-mode AP trampoline — copied to physical 0x8000.
; Boot info at physical 0x7000 (see smp.c):
;   +0  uint32 entry
;   +4  uint32 stack
;   +8  gdtr (uint16 limit, uint32 base)
;   +14 uint32 cpu_index

[bits 16]
[org 0x8000]

SMP_INFO equ 0x7000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    lgdt [SMP_INFO + 8]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Explicit 32-bit far jump into protected mode (avoid NASM encoding quirks).
    db 0x66, 0xEA
    dd pm_entry
    dw 0x08

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, [SMP_INFO + 4]
    and esp, 0xFFFFFFF0
    mov eax, [SMP_INFO + 14]
    push eax
    mov eax, [SMP_INFO + 0]
    call eax
.hang:
    cli
    hlt
    jmp .hang
