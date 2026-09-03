global irq0_stub
extern irq0_handler_c

irq0_stub:
    pusha               ; save regs
    call irq0_handler_c ; C handler bumps ticks + EOI
    popa
    iret

global irq1_stub
extern keyboard_handler_c

irq1_stub:
    pusha
    call keyboard_handler_c
    popa
    iret

global irq12_stub
extern mouse_handler_c

irq12_stub:
    pusha
    call mouse_handler_c
    popa
    iret
section .note.GNU-stack noalloc noexec nowrite progbits
