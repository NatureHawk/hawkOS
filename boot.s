BITS 32

SECTION .multiboot
align 4
MAG  equ 0x1BADB002
FLG  equ (1<<0) | (1<<1)
CHK  equ -(MAG + FLG)

dd MAG
dd FLG
dd CHK

SECTION .text
global _start
extern kernel_main
extern load_gdt
extern idt_init

_start:
    cli
    call load_gdt
    mov esp, stack_top
    call idt_init
    call kernel_main

.hang:
    hlt
    jmp .hang

SECTION .bss
align 16
stack:     resb 16384
stack_top:
