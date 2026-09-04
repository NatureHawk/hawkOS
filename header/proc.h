#pragma once
#include <stdint.h>

// User processes.
//
// Until now every "app" was a kernel thread: same address space, same
// privilege level, nothing between a bug in one and the rest of the system.
// A process here is different in the two ways that matter — it runs at CPL 3,
// and it has its own page directory — so it can only reach memory the kernel
// mapped for it, and can only ask the kernel for anything through int 0x80.
//
// The kernel's own mappings are shared into every process directory but carry
// no USER bit, which is what makes them unreachable from ring 3 rather than
// merely inconvenient to find.

// Where a program's image is placed in its own address space. Chosen well
// clear of the identity-mapped kernel region (0..128 MB) so the two can never
// overlap however much RAM the machine reports.
#define PROC_BASE       0x40000000u
#define PROC_STACK_TOP  0x40800000u
#define PROC_STACK_SIZE (16u * 1024u)
#define PROC_IMAGE_MAX  (512u * 1024u)

typedef struct {
    int      used;
    uint32_t pid;              // the hosting task's id
    uint32_t page_dir;         // physical address of this process's directory
    uint32_t image_base;       // always PROC_BASE
    uint32_t image_len;        // bytes of program image mapped
    uint32_t brk;              // end of the mapped image, page aligned
    int      exit_code;
    int      exited;
    char     name[16];
} proc_t;

#define PROC_MAX 8

void proc_init(void);

// Loads a flat binary from the filesystem and runs it in ring 3 on a task of
// its own. Returns the new task id, or -1.
int  proc_spawn(const char* path);

// Called from the SYS_EXIT handler; does not return to the caller's program.
void proc_exit(int code);

// Bounds checks for syscall arguments. Both answer the question "does this
// address belong to the process that is currently making a system call",
// which is the only question that makes a pointer from ring 3 safe to touch.
int  proc_valid_range(uint32_t addr, uint32_t len);
int  proc_valid_string(uint32_t addr);

// Filesystem access on behalf of a process. Kept here rather than in the
// syscall layer because both need the same "is this a sane name" rules.
int      proc_open(const char* name);
uint32_t proc_read(const char* name, uint8_t* out, uint32_t cap);

// Snapshot for the task manager.
int  proc_snapshot(proc_t* out, int max);
int  proc_count(void);
