; Embed the real-mode trampoline binary built by nasm -f bin.
section .rodata
global smp_trampoline_start
global smp_trampoline_end
global smp_trampoline_size

smp_trampoline_start:
    incbin "smp_trampoline.bin"
smp_trampoline_end:

smp_trampoline_size:
    dd smp_trampoline_end - smp_trampoline_start
