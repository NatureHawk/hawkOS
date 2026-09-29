#pragma once
#include <stdint.h>

#define TASK_MAX          16
// 64 KB. It was 16, then 32 for a TLS handshake, and 32 was outgrown by
// stb_image's GIF decoder, which keeps its 34 KB LZW state in a local. A task
// stack is an ordinary heap block, so overrunning it wiped the header of the
// block below and silently cut the heap off a few hundred KB in. Every stack
// now also carries a guard word at its lowest address (TASK_STACK_GUARD),
// checked on each switch, so an overflow is reported instead of discovered.
#define TASK_STACK_SIZE   (64u * 1024u)
#define TASK_STACK_GUARD  0x57AC6A4Du
#define TASK_NAME_LEN     16

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_BLOCKED        // waiting on a wait queue (header/sync.h); appended so older values keep their numbers
} task_state_t;

// esp MUST stay the first member: switch.asm writes the saved stack pointer
// through a raw pointer to the struct, at offset 0.
typedef struct task {
    uint32_t     esp;
    uint32_t     id;
    task_state_t state;
    uint64_t     wake_tick;      // SLEEPING: when to wake. BLOCKED: timeout tick, 0 = none
    void*        wait_obj;       // BLOCKED: the wait queue this task sits on
    uint32_t     wait_ticket;    // arrival order on that queue, so wake_one is FIFO
    int          timed_out;      // the last blocking wait ended by its timeout
    uint8_t*     stack_base;     // kmalloc'd block to free when reaped (0 = kernel task)
    uint32_t     slices;         // timer slices this task has been scheduled for
    // Physical address of the page directory to run this task under. Kernel
    // threads all share the kernel's; a task hosting a user process gets its
    // own, and the scheduler reloads CR3 when it changes.
    uint32_t     page_dir;
    char         name[TASK_NAME_LEN];
    int          overflowed;     // stack guard found damaged (reported once)
    // x87/SSE registers while the task is switched out (see header/fpu.h).
    // fxsave faults on an address that is not 16-byte aligned, and the
    // attribute carries that alignment into every element of the task table.
    uint8_t      fpu[512] __attribute__((aligned(16)));
} task_t;

typedef void (*task_fn_t)(void* arg);

void     task_init(void);                                   // adopt the boot context as task 0
int      task_create(const char* name, task_fn_t fn, void* arg);  // -> task id, or -1
void     task_yield(void);                                  // give up the rest of this slice
void     task_sleep(uint32_t ms);                           // block this task for ms
void     task_exit(void);                                   // end the calling task
int      task_kill(uint32_t id);                            // 0 on success
void     task_ps(void);                                     // print the task table
uint32_t task_current_id(void);
// Read-only snapshot of the task table, for the task manager. Copied under
// an interrupt guard so the caller never sees a half-updated entry.
typedef struct {
    uint32_t id;
    int      state;
    uint32_t slices;
    int      is_current;
    char     name[TASK_NAME_LEN];
} task_info_t;

int      task_snapshot(task_info_t* out, int max);
const char* task_state_name(int state);

// Binds the running task to an address space. Called when a task takes
// ownership of a user process, so that every later switch back to it restores
// the right CR3.
void     task_set_address_space(uint32_t page_dir);

// Top of the running task's kernel stack — what the TSS has to point at so a
// trap from ring 3 lands somewhere this task owns.
uint32_t task_kernel_stack_top(void);

// Blocking primitives behind header/sync.h. task_block_locked must be called
// with interrupts off (irq_save); task_wake_obj is also safe from an interrupt
// handler, where it only marks tasks ready -- the switch happens on the way
// out of the timer tick, or at the next yield.
//   task_block_locked: park the caller on obj until woken, or until
//     timeout_ticks (0 = none) pass. Returns 1 if it timed out, 0 if woken.
//   task_wake_obj: ready the oldest waiter on obj, or all of them.
int      task_block_locked(void* obj, uint32_t timeout_ticks);
int      task_wake_obj(void* obj, int all);

void     sched_tick(void);

// Running totals of timer ticks spent idle and ticks overall, for a CPU load
// figure: sample twice and compare.
void     sched_load(uint32_t* idle, uint32_t* all);                                  // called from the IRQ0 handler
