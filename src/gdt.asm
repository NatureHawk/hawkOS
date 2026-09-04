BITS 32

; The GDT itself is built in C (src/gdt.c) now that it has to carry a TSS
; descriptor holding a runtime address. What is left here is the part that has
; to be assembly: loading the register and reloading the segment selectors,
; which cannot be expressed in C.

section .text

global gdt_flush
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]

    mov ax, 0x10                 ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; CS cannot be loaded with a mov; a far jump is the only way to make the
    ; new code descriptor take effect.
    jmp 0x08:.flush
.flush:
    ret

global tss_flush
tss_flush:
    mov eax, [esp + 4]
    ltr ax
    ret

; Called from boot.s before the C world exists. The real table is installed
; later by gdt_init(); this only needs to get the CPU off GRUB's GDT, which we
; do not own and which GRUB is free to reuse once it hands over.
global load_gdt
load_gdt:
    lgdt [boot_gdtr]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush
.flush:
    ret

section .data
align 8
boot_gdt:
    dq 0
    ; 0x08 flat ring-0 code, 0x10 flat ring-0 data
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
boot_gdt_end:

boot_gdtr:
    dw boot_gdt_end - boot_gdt - 1
    dd boot_gdt

section .note.GNU-stack noalloc noexec nowrite progbits
