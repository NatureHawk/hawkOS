BITS 32

SECTION .multiboot
align 4
MAG  equ 0x1BADB002
FLG  equ (1<<0) | (1<<1) | (1<<2)
CHK  equ -(MAG + FLG)

dd MAG
dd FLG
dd CHK

; video mode request (flag bit 2): ask GRUB for a linear graphics framebuffer.
; This is a hint only — GRUB does its best and reports what it actually set
; in the multiboot info struct handed to us; gfx_init() must check that
; before trusting it, never assume this exact mode was granted.
dd 0          ; mode_type: 0 = linear graphics framebuffer
dd 1024       ; width
dd 768        ; height
dd 32         ; depth (bits per pixel)

SECTION .text
global _start
extern kernel_main
extern load_gdt
_start:
    cli
    ; GRUB guarantees EAX=multiboot magic, EBX=phys addr of multiboot info
    ; struct at this exact point. Stash both before anything else can
    ; clobber them (call/ret and the stack switch below don't touch
    ; eax/ebx, but kernel_main's own prologue will).
    mov [mb_magic], eax
    mov [mb_info], ebx

    call load_gdt
    mov esp, stack_top

    push dword [mb_info]
    push dword [mb_magic]
    call kernel_main

.hang:
    hlt
    jmp .hang

SECTION .bss
align 16
stack:     resb 65536
stack_top:
align 4
mb_magic:  resd 1
mb_info:   resd 1
