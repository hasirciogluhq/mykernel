; int 0x80 — Linux-like syscall entry

section .text
global isr_syscall
extern syscall_isr_handler

isr_syscall:
    push dword 0            ; fake error code
    push dword 0x80         ; interrupt number

    pusha                   ; edi esi ebp esp ebx edx ecx eax

    ; Ensure kernel data segments (may still be user 0x23 after ring3 entry)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                ; pointer to register frame
    call syscall_isr_handler
    add esp, 4

    popa                    ; eax = return value from frame
    add esp, 8              ; drop int_no + err
    iret
