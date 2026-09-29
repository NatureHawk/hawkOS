#pragma once
#include <stdint.h>
#include "header/vfs.h"
#include "header/vm.h"

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
//
// The address space is a vm_space_t (header/vm.h): regions say what may exist,
// pages arrive on first touch, the stack grows downward on demand. Files,
// pipes and the console are vfs_file_t descriptions in a per-process
// vfs_fdtable_t (header/vfs.h), and a process carries its own cwd string.

// Where a program's image is placed in its own address space. Chosen well
// clear of the identity-mapped kernel region (0..128 MB) so the two can never
// overlap however much RAM the machine reports.
//
// Layout of the user range, low to high:
//   PROC_BASE .. +image      ELF segments (or the flat binary), <= PROC_IMAGE_MAX
//   heap_start .. brk        VM_HEAP_GAP above the image; grown by SYS_BRK / SYS_SBRK
//   stack                    PROC_STACK_SIZE below PROC_STACK_TOP, grows down to VM_STACK_MAX
//   VM_MMAP_BASE ..          SYS_MMAP area
#define PROC_BASE       0x40000000u
#define PROC_STACK_TOP  0x40800000u
#define PROC_STACK_SIZE (16u * 1024u)     // initial; grows on demand
#define PROC_IMAGE_MAX  (512u * 1024u)
#define PROC_USER_END   PROC_STACK_TOP

#define PROC_ARGS_MAX   8      // argv entries handed to a new process
#define PROC_ARGS_BYTES 256    // total bytes of argument text

// Spawn flags.
#define PROC_SPAWN_INHERIT 1   // child shares the parent's open descriptors (like fork+exec)

// open() flags, deliberately the familiar values (same as VFS_O_*).
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x040
#define O_TRUNC   0x200
#define O_APPEND  0x400

typedef struct {
    int      used;
    uint32_t pid;              // process id: what spawn returns and waitpid/kill take
    uint32_t tid;              // the hosting task's id (set at spawn)
    uint32_t ppid;             // spawning process, 0 for the kernel
    vm_space_t* vm;            // the address space
    uint32_t page_dir;         // physical address of its directory
    int      exit_code;
    int      exited;
    char     name[16];
    vfs_fdtable_t fdt;         // 0/1/2 start on /dev/console
    char     cwd[VFS_PATH_MAX];
} proc_t;

#define PROC_MAX 8

// ------------------------------------------------------------------ ELF

// What the loader needs out of an executable, after strict validation.
typedef struct {
    uint32_t entry;
    uint32_t span;             // bytes from PROC_BASE to the end of the last segment
    int      nload;
} proc_elf_t;

// Validates an ELF32 image held in memory. Returns 0 and fills `out`, or a
// negative code naming the first thing wrong (see PROC_ELF_* below). Touches
// nothing but the buffer, so it is safe to call on hostile input.
int  proc_elf_parse(const uint8_t* img, uint32_t len, proc_elf_t* out);

#define PROC_ELF_OK        0
#define PROC_ELF_NOTELF   -1   // no ELF magic (the flat-binary path)
#define PROC_ELF_TRUNC    -2   // header or program-header table past the end
#define PROC_ELF_CLASS    -3   // not 32-bit little-endian, version 1
#define PROC_ELF_TYPE     -4   // not ET_EXEC / EM_386
#define PROC_ELF_PHDR     -5   // bad program-header size or count
#define PROC_ELF_SEGMENT  -6   // segment outside the user range, overflow, or overlap
#define PROC_ELF_FILESZ   -7   // segment data past the end of the file, or filesz > memsz
#define PROC_ELF_ENTRY    -8   // entry point not inside a loadable segment
#define PROC_ELF_NOLOAD   -9   // no PT_LOAD at all

void proc_init(void);

// Loads an executable through the VFS (ELF32, else a flat binary linked at
// PROC_BASE) and runs it in ring 3 on a task of its own. Returns the new
// process id, or -1. `args` is an optional space-separated argument string;
// argv[0] is always the path as given. The path is resolved against the
// spawning process's cwd ("/" for the kernel).
int  proc_spawn(const char* path);
int  proc_spawn_args(const char* path, const char* args);
// As above, with an explicit cwd for both the lookup and the child (NULL = the
// spawner's) and PROC_SPAWN_* flags.
int  proc_spawn_ex(const char* path, const char* args, const char* cwd, int flags);

// Called from the SYS_EXIT handler; does not return to the caller's program.
void proc_exit(int code);

// Waits (asleep on a wait queue that proc_exit wakes) for child `pid` of the
// calling process (0 = the kernel) to finish and stores its exit code.
// Returns pid, -1 if there is no such child, or VFS_EINTR if a signal is
// waiting for the caller.
int  proc_wait(int pid, int* code);

// The process whose address space is active (0 for a kernel task).
proc_t* proc_current(void);
uint32_t proc_current_pid(void);
// pid -> hosting task id, or 0xFFFFFFFF if there is no such live process.
uint32_t proc_tid_of(uint32_t pid);

// Argument checks for syscalls, against the calling process's regions. Both
// reject kernel addresses, wraps and unmapped holes. The range check does not
// map anything; copies (vm_copy_to/from) demand-fault the pages in.
int  proc_check_range(uint32_t addr, uint32_t len, int write);
// Copies a NUL-terminated string of at most cap-1 bytes out of the process.
// 0, VFS_ENAMETOOLONG or -14 (EFAULT).
int  proc_copy_string(uint32_t addr, char* out, uint32_t cap);

// Seconds since 1970 from the RTC.
uint32_t proc_gettime(void);

// Test hooks: a process with an address space but no task, bound as the
// current one, so syscall handlers can be driven from a kernel test.
proc_t* proc_test_create(void);
void    proc_test_bind(proc_t* p);      // 0 unbinds
void    proc_test_destroy(proc_t* p);

// Snapshot for the task manager.
int  proc_snapshot(proc_t* out, int max);
int  proc_count(void);
