; void enter_usermode(uint32_t entry, uint32_t user_stack)
; Never returns.

section .text
global enter_usermode

enter_usermode:
    cli
    mov edx, [esp + 4]      ; entry EIP
    mov ecx, [esp + 8]      ; user ESP

    mov ax, 0x23            ; user data | RPL3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push dword 0x23         ; SS
    push ecx                ; ESP
    pushf
    pop eax
    and eax, 0xFFFFFDFF     ; keep IF clear (polling keyboard, no IRQ handlers)
    push eax                ; EFLAGS
    push dword 0x1B         ; CS user code | RPL3
    push edx                ; EIP
    iret
