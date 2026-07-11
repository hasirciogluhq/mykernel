; context_switch(uint32_t **old_esp, uint32_t *new_esp)
; cdecl: [esp+4]=old_esp, [esp+8]=new_esp

section .text
global context_switch

context_switch:
    mov eax, [esp + 4]      ; old_esp (uint32_t **)
    mov edx, [esp + 8]      ; new_esp (uint32_t *)

    push ebx
    push esi
    push edi
    push ebp

    mov [eax], esp          ; *old_esp = current esp
    mov esp, edx            ; switch

    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
