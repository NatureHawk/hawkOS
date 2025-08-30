global irq0_stub
extern irq0_handler_c

irq0_stub:
    pusha               ; save regs
    call irq0_handler_c ; C handler bumps ticks + EOI
    popa
    iret
