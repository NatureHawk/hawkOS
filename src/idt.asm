BITS 32
global idt_load
global isr_stub

idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret

isr_stub:
    cli
.h: hlt
    jmp .h
