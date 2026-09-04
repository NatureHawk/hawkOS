; src/isr_exn.asm
[BITS 32]
global isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31
extern isr_handler_c

; The handler is passed the code selector in force when the fault happened, as
; well as the vector and error code. Its low two bits are the privilege level,
; which is what distinguishes "the kernel has a bug" from "a user program did
; something it is not allowed to do" -- the first has to halt, the second must
; not.
;
; Offsets are from the top of the pusha block: the CPU pushed eip/cs/eflags
; (and an error code first, for the vectors that have one) before entering.

%macro EXC_NOERR 1
isr%1:
    pusha
    mov eax,[esp+36]        ; cs
    push eax
    push dword 0            ; no error code for this vector
    push dword %1
    call isr_handler_c
    add esp,12
    popa
    iret
%endmacro

%macro EXC_ERR 1
isr%1:
    pusha
    mov eax,[esp+40]        ; cs
    push eax
    mov eax,[esp+36]        ; error code (esp moved by the push above)
    push eax
    push dword %1
    call isr_handler_c
    add esp,12
    popa
    add esp,4               ; discard the error code the CPU pushed
    iret
%endmacro

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
EXC_NOERR 20
EXC_NOERR 21
EXC_NOERR 22
EXC_NOERR 23
EXC_NOERR 24
EXC_NOERR 25
EXC_NOERR 26
EXC_NOERR 27
EXC_NOERR 28
EXC_NOERR 29
EXC_ERR   30
EXC_NOERR 31

section .note.GNU-stack noalloc noexec nowrite progbits
