; int 0x80 — Linux-like syscall entry (same privilege)

section .text
global isr_syscall
extern syscall_isr_handler

isr_syscall:
    push dword 0            ; fake error code
    push dword 0x80         ; interrupt number

    pusha                   ; edi esi ebp esp ebx edx ecx eax

    push esp                ; pointer to register frame
    call syscall_isr_handler
    add esp, 4

    popa                    ; eax holds return value
    add esp, 8              ; drop int_no + err
    iret
