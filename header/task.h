#pragma once
#include <stdint.h>

#define TASK_MAX          16
// 32 KB, not 16: a TLS handshake and the HTTP header parser both
// run on a task stack, and BearSSL uses several KB of locals.
#define TASK_STACK_SIZE   (32u * 1024u)
#define TASK_NAME_LEN     16

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE
} task_state_t;

// esp MUST stay the first member: switch.asm writes the saved stack pointer
// through a raw pointer to the struct, at offset 0.
typedef struct task {
    uint32_t     esp;
    uint32_t     id;
    task_state_t state;
    uint64_t     wake_tick;      // valid while TASK_SLEEPING
    uint8_t*     stack_base;     // kmalloc'd block to free when reaped (0 = kernel task)
    uint32_t     slices;         // timer slices this task has been scheduled for
    // Physical address of the page directory to run this task under. Kernel
    // threads all share the kernel's; a task hosting a user process gets its
    // own, and the scheduler reloads CR3 when it changes.
    uint32_t     page_dir;
    char         name[TASK_NAME_LEN];
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

void     sched_tick(void);                                  // called from the IRQ0 handler
