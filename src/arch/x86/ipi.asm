; LAPIC IPI / spurious / error stubs

section .text
global ipi_stub
global apic_spurious_stub
global apic_error_stub
extern ipi_dispatch
extern apic_error_dispatch

ipi_stub:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call ipi_dispatch
    popa
    iret

; Spurious interrupt: no EOI required by the APIC.
apic_spurious_stub:
    iret

apic_error_stub:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call apic_error_dispatch
    popa
    iret
