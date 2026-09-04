// src/task.c — preemptive round-robin scheduler
//
// Tasks are kernel threads: they share the kernel's address space and its
// privilege level, and differ only in having their own 16 KB stack. That is
// deliberate for this stage — ring-3 and per-process page directories come
// later, and layering them on top of a switcher that already works is much
// easier than debugging both at once.
//
// Preemption is driven by the existing 100 Hz PIT interrupt: irq0_handler_c
// sends the EOI and then calls sched_tick(), which may switch stacks before
// the handler's popa/iret runs. That works because every task suspended by
// the timer is suspended at exactly the same point, so the frame the popa
// unwinds always matches the stack it is unwound from.
#include <stdint.h>
#include <stddef.h>
#include "header/task.h"
#include "header/kheap.h"
#include "header/kprintf.h"
#include "header/irqctl.h"
#include "header/paging.h"
#include "header/gdt.h"

extern volatile unsigned long long ticks;

extern void task_switch(uint32_t* save_slot, uint32_t new_esp);
extern void task_trampoline(void);

// Defined in proc.c. Declared here rather than pulled in through proc.h so
// the scheduler does not gain a dependency on the whole process interface for
// one notification.
void proc_note_switch(uint32_t pid);

static task_t   tasks[TASK_MAX];
static int      current   = 0;
static int      idle_idx  = -1;
static int      sched_on  = 0;
static uint32_t next_id   = 0;

static void name_copy(char* dst, const char* src){
    int i = 0;
    if (src) for (; src[i] && i < TASK_NAME_LEN - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void idle_task(void* arg){
    (void)arg;
    // Nothing to run: park the CPU until the next interrupt rather than
    // spinning, so QEMU (and a real machine) stays cool and responsive.
    for(;;) __asm__ __volatile__("hlt");
}

void task_init(void){
    for (int i = 0; i < TASK_MAX; i++) tasks[i].state = TASK_UNUSED;

    // Task 0 is not created — it is adopted. We are already running on the
    // boot stack, so its esp gets filled in the first time we switch away.
    tasks[0].id         = next_id++;
    tasks[0].state      = TASK_RUNNING;
    tasks[0].stack_base = 0;            // boot stack: not heap, never freed
    tasks[0].slices     = 0;
    tasks[0].esp        = 0;
    tasks[0].page_dir   = paging_kernel_dir();
    name_copy(tasks[0].name, "kernel");
    current = 0;

    idle_idx = task_create("idle", idle_task, 0);
    sched_on = 1;
    kprintf("[sched] preemptive round-robin online (%d slots, %u KB stacks)\n",
            TASK_MAX, TASK_STACK_SIZE / 1024u);
}

int task_create(const char* name, task_fn_t fn, void* arg){
    uint32_t f = irq_save();

    int slot = -1;
    for (int i = 0; i < TASK_MAX; i++) if (tasks[i].state == TASK_UNUSED) { slot = i; break; }
    if (slot < 0) { irq_restore(f); return -1; }

    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!stack) { irq_restore(f); return -1; }

    // Fake the frame task_switch() expects to unwind, with task_trampoline
    // as the return address so a first switch to this task lands there.
    uint32_t* sp = (uint32_t*)(stack + TASK_STACK_SIZE);
    *--sp = (uint32_t)arg;                  // argument to fn
    *--sp = (uint32_t)fn;                   // trampoline pops this into eax
    *--sp = (uint32_t)task_trampoline;      // task_switch's ret target
    *--sp = 0;                              // ebp
    *--sp = 0;                              // ebx
    *--sp = 0;                              // esi
    *--sp = 0;                              // edi

    task_t* t = &tasks[slot];
    t->esp        = (uint32_t)sp;
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->wake_tick  = 0;
    t->stack_base = stack;
    t->slices     = 0;
    t->page_dir   = paging_kernel_dir();   // a user process replaces this later
    name_copy(t->name, name);

    irq_restore(f);
    return slot;
}

// Round-robin from the slot after the current one, so every runnable task
// gets a turn before any task gets a second one. The idle task is skipped
// here and only used as a last resort, otherwise it would soak up a full
// slice on every lap of the table.
static int pick_next(void){
    for (int n = 1; n <= TASK_MAX; n++) {
        int i = (current + n) % TASK_MAX;
        if (i == idle_idx) continue;
        if (tasks[i].state == TASK_READY) return i;
    }
    if (tasks[current].state == TASK_RUNNING) return current;   // nobody else wants it
    return idle_idx;
}

// Must be called with interrupts disabled.
static void schedule(void){
    if (!sched_on) return;

    unsigned long long now = ticks;
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_tick)
            tasks[i].state = TASK_READY;

        // Reap an exited task once we are no longer standing on its stack.
        if (tasks[i].state == TASK_ZOMBIE && i != current) {
            if (tasks[i].stack_base) kfree(tasks[i].stack_base);
            tasks[i].stack_base = 0;
            tasks[i].state      = TASK_UNUSED;
        }
    }

    int next = pick_next();
    if (next < 0 || next == current) return;

    task_t* prev = &tasks[current];
    task_t* nxt  = &tasks[next];

    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    nxt->state = TASK_RUNNING;
    nxt->slices++;
    current = next;

    // Everything that has to be true before the next task's first instruction:
    // its address space loaded, and the TSS pointing at its kernel stack so a
    // trap out of ring 3 lands somewhere it owns. Both are skipped when the
    // address space is unchanged, because reloading CR3 flushes the TLB and
    // kernel threads share one directory.
    if (nxt->page_dir && nxt->page_dir != prev->page_dir) paging_switch(nxt->page_dir);
    if (nxt->stack_base)
        tss_set_kernel_stack((uint32_t)nxt->stack_base + TASK_STACK_SIZE);
    proc_note_switch(nxt->id);

    task_switch(&prev->esp, nxt->esp);
    // Execution resumes here whenever this task is scheduled again.
}

void sched_tick(void){
    if (!sched_on) return;
    schedule();               // already inside an interrupt gate: IF is clear
}

void task_yield(void){
    uint32_t f = irq_save();
    schedule();
    irq_restore(f);
}

void task_sleep(uint32_t ms){
    if (!sched_on) return;
    uint32_t f = irq_save();
    tasks[current].wake_tick = ticks + (ms + 9u) / 10u;   // PIT runs at 100 Hz
    tasks[current].state     = TASK_SLEEPING;
    schedule();
    irq_restore(f);
}

void task_exit(void){
    uint32_t f = irq_save();
    tasks[current].state = TASK_ZOMBIE;
    irq_restore(f);
    for(;;) task_yield();     // never picked again; the reaper frees the stack
}

int task_kill(uint32_t id){
    uint32_t f = irq_save();
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].id != id) continue;
        if (i == 0 || i == idle_idx) { irq_restore(f); return -2; }   // kernel/idle are not killable
        if (i == current) { irq_restore(f); task_exit(); }
        if (tasks[i].stack_base) kfree(tasks[i].stack_base);
        tasks[i].stack_base = 0;
        tasks[i].state      = TASK_UNUSED;
        irq_restore(f);
        return 0;
    }
    irq_restore(f);
    return -1;
}

uint32_t task_current_id(void){ return tasks[current].id; }

void task_set_address_space(uint32_t page_dir){
    uint32_t f = irq_save();
    tasks[current].page_dir = page_dir;
    irq_restore(f);
}

uint32_t task_kernel_stack_top(void){
    if (!tasks[current].stack_base) return 0;   // task 0 runs on the boot stack
    return (uint32_t)tasks[current].stack_base + TASK_STACK_SIZE;
}

void task_ps(void){
    static const char* st[] = { "unused", "ready", "running", "sleeping", "zombie" };
    uint32_t f = irq_save();
    kprintf("  id  state     slices  name\n");
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED) continue;
        kprintf("  %u   %s", tasks[i].id, st[tasks[i].state]);
        for (int p = 0; p < 10 - (int)0; p++) { }   // states are short; one space is enough
        kprintf("\t%u\t%s%s\n", tasks[i].slices, tasks[i].name,
                i == current ? "  <- current" : "");
    }
    irq_restore(f);
}

const char* task_state_name(int state){
    static const char* st[] = { "unused", "ready", "running", "sleeping", "zombie" };
    return (state >= 0 && state <= 4) ? st[state] : "?";
}

int task_snapshot(task_info_t* out, int max){
    uint32_t f = irq_save();
    int n = 0;
    for (int i = 0; i < TASK_MAX && n < max; i++){
        if (tasks[i].state == TASK_UNUSED) continue;
        out[n].id         = tasks[i].id;
        out[n].state      = (int)tasks[i].state;
        out[n].slices     = tasks[i].slices;
        out[n].is_current = (i == current);
        for (int c = 0; c < TASK_NAME_LEN; c++) out[n].name[c] = tasks[i].name[c];
        n++;
    }
    irq_restore(f);
    return n;
}
