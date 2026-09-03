BITS 32

; void task_switch(uint32_t* save_slot, uint32_t new_esp)
;
; Saves the four callee-saved registers of the outgoing task onto its own
; kernel stack, stores that stack pointer through save_slot (which points at
; task_t::esp — the reason esp must stay the first member of task_t), then
; adopts the incoming task's stack and unwinds the mirror-image frame that
; was left there. The `ret` at the end returns into whatever the incoming
; task was doing when it last gave up the CPU. eax/ecx/edx are caller-saved
; under cdecl, so the compiler has already dealt with them.
global task_switch
task_switch:
    mov  eax, [esp+4]         ; &outgoing->esp
    mov  edx, [esp+8]         ; incoming->esp

    push ebp
    push ebx
    push esi
    push edi

    mov  [eax], esp           ; outgoing->esp = current stack pointer
    mov  esp, edx             ; adopt the incoming task's stack

    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret                       ; -> where the incoming task left off

; Entry point for a task that has never run. task_create() fakes a stack
; frame whose return address is this trampoline, with the entry function and
; its argument stacked just above it.
;
; The sti matters: a task is first switched to from inside the timer
; interrupt handler, where IF is clear. Without re-enabling interrupts here
; the new task would run with the timer masked and could never be preempted.
global task_trampoline
extern task_exit
task_trampoline:
    sti
    pop  eax                  ; entry function; esp now points at its argument
    call eax                  ; cdecl: the argument is already in place
    add  esp, 4               ; drop the argument
    call task_exit            ; a task that returns is a task that exited
.hang:
    hlt                       ; task_exit never comes back, but be explicit
    jmp  .hang

; Mark the stack non-executable; without this the linker warns and falls back
; to an executable stack for the whole image.
section .note.GNU-stack noalloc noexec nowrite progbits
