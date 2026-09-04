BITS 32

; int 0x80 entry, and the one-way trip into ring 3.

section .text

extern syscall_dispatch
global syscall_stub

; The CPU has already switched to the kernel stack named by the TSS and pushed
; ss/esp/eflags/cs/eip. pusha then puts the user's registers on that stack in a
; known order, which is exactly the layout of syscall_regs_t -- so the handler
; can both read arguments out of it and write a return value back into the
; slot that will be popped into eax.
syscall_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    ; Segment registers still hold the user's selectors. Load the kernel's
    ; before touching anything, or every memory reference in C resolves
    ; through a ring-3 descriptor.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                    ; the register block, as syscall_regs_t*
    call syscall_dispatch
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iret

; usermode_jump(entry, user_stack_top)
;
; Fakes the stack frame an iret from a ring-3 interrupt would have left, and
; executes it. This is the only way into ring 3 on x86: there is no
; instruction that lowers privilege directly, so the CPU has to be persuaded
; it is returning to code that was already there.
global usermode_jump
usermode_jump:
    mov eax, [esp + 4]          ; entry point
    mov ebx, [esp + 8]          ; user stack top

    mov cx, 0x23                ; user data selector, RPL 3
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx

    push 0x23                   ; ss
    push ebx                    ; esp
    pushf                       ; eflags
    pop ecx
    or ecx, 0x200               ; force IF on: a ring-3 task that ran with
    push ecx                    ; interrupts masked could never be preempted
    push 0x1B                   ; cs, user code selector, RPL 3
    push eax                    ; eip
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
