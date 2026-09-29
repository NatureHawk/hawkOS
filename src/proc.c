// src/proc.c — user processes: address spaces, loading, and the ring-3 entry
//
// A process is an address space (vm_space_t, see header/vm.h) plus a kernel
// task to host it. The task exists because the scheduler only knows how to
// switch kernel stacks; the address space exists because that is what makes
// one process unable to see another. Its files are a vfs_fdtable_t and its
// working directory a path string, so nothing here knows what a FAT32 cluster
// is.
//
// Two image formats load: ELF32 executables (PT_LOAD segments mapped at their
// p_vaddr inside the user range that starts at PROC_BASE) and, as a fallback
// for anything that does not carry the ELF magic, a flat binary linked at
// PROC_BASE. An image that claims to be ELF and is malformed is rejected, not
// reinterpreted as flat code: running the bytes of a broken header would turn
// a validation failure into arbitrary execution.
#include <stdint.h>
#include "header/proc.h"
#include "header/paging.h"
#include "header/pmm.h"
#include "header/gdt.h"
#include "header/task.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/vfs.h"
#include "header/vm.h"
#include "header/sync.h"
#include "header/signal.h"
#include "header/shell.h"
#include "header/kprintf.h"
#include "header/irqctl.h"
#include "header/rtc.h"

extern void usermode_jump(uint32_t entry, uint32_t stack_top);

static proc_t procs[PROC_MAX];
static uint32_t next_pid = 1;

// The process the CPU is currently executing, so a syscall can bounds-check
// pointers against the right image. Set on entry to ring 3 and whenever the
// scheduler switches address spaces.
static proc_t* current_proc = 0;

#define NO_TID 0xFFFFFFFFu     // "has not run yet"; never equals a task id

// What a finished child leaves behind for waitpid. A small ring: a parent
// that never waits loses the oldest record, not the ability to spawn.
#define EXIT_RECS 16
static struct { uint32_t pid, ppid; int code; int valid; } exits[EXIT_RECS];
static int exit_next = 0;

// Woken by every proc_exit; waitpid sleeps here instead of polling.
static waitq_t child_wq = WAITQ_INIT;

#define EFAULT_ (-14)

void proc_init(void){
    memset(procs, 0, sizeof(procs));
    memset(exits, 0, sizeof(exits));
    current_proc = 0;
}

static proc_t* by_tid(uint32_t tid){
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].used && procs[i].tid == tid) return &procs[i];
    return 0;
}

static proc_t* by_pid(uint32_t pid){
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return 0;
}

// Claims a free slot atomically, so two spawners cannot take the same one.
static proc_t* claim_slot(void){
    uint32_t f = irq_save();
    for (int i = 0; i < PROC_MAX; i++){
        if (!procs[i].used){
            memset(&procs[i], 0, sizeof(procs[i]));
            procs[i].used = 1;
            procs[i].tid  = NO_TID;
            irq_restore(f);
            return &procs[i];
        }
    }
    irq_restore(f);
    return 0;
}

int proc_count(void){
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) if (procs[i].used) n++;
    return n;
}

int proc_snapshot(proc_t* out, int max){
    int n = 0;
    for (int i = 0; i < PROC_MAX && n < max; i++)
        if (procs[i].used) out[n++] = procs[i];
    return n;
}

proc_t* proc_current(void){ return current_proc; }
uint32_t proc_current_pid(void){ return current_proc ? current_proc->pid : 0; }

uint32_t proc_tid_of(uint32_t pid){
    uint32_t f = irq_save();
    proc_t* p = by_pid(pid);
    uint32_t tid = p ? p->tid : NO_TID;
    irq_restore(f);
    return tid;
}

// ------------------------------------------------------------ validation

// The region list of the calling process's address space decides what is its
// to name: anything else -- a kernel address, the gap between heap and stack,
// memory past the break -- is refused, and so is a write to a read-only
// region. Pages inside a region need not be present yet; the copy helpers
// fault them in.
int proc_check_range(uint32_t addr, uint32_t len, int write){
    proc_t* p = current_proc;
    if (!p || !p->vm) return 0;
    return vm_range_ok(p->vm, addr, len, write);
}

int proc_copy_string(uint32_t addr, char* out, uint32_t cap){
    proc_t* p = current_proc;
    if (!p || !p->vm) return EFAULT_;
    uint32_t i = 0;
    // A page at a time, each checked before it is read, so a string that runs
    // off the end of the process's memory is rejected at the boundary.
    while (i < cap){
        uint32_t a = addr + i;
        uint32_t n = PMM_FRAME_SIZE - (a & (PMM_FRAME_SIZE - 1));
        if (n > cap - i) n = cap - i;
        if (a + n < a || !vm_range_ok(p->vm, a, n, 0)) return EFAULT_;
        if (vm_copy_from(p->vm, a, out + i, n) != 0) return EFAULT_;
        for (uint32_t k = 0; k < n; k++)
            if (out[i + k] == 0) return 0;
        i += n;
    }
    out[cap - 1] = 0;
    return VFS_ENAMETOOLONG;
}

// ------------------------------------------------------------------ time

uint32_t proc_gettime(void){
    rtc_time_t t;
    rtc_read(&t);
    // Days from civil (Hinnant), so no month table and no leap-year branches.
    int32_t y = t.year;
    int32_t m = t.month ? t.month : 1;
    y -= m <= 2;
    int32_t era = y / 400;
    int32_t yoe = y - era * 400;
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int32_t days = era * 146097 + doe - 719468;
    return (uint32_t)days * 86400u + t.hour * 3600u + t.min * 60u + t.sec;
}

// -------------------------------------------------------------------- ELF

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf_ehdr_t;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed)) elf_phdr_t;

#define ET_EXEC   2
#define EM_386    3
#define PT_LOAD   1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define ELF_MAX_PH 16

int proc_elf_parse(const uint8_t* img, uint32_t len, proc_elf_t* out){
    if (!img || len < 4 || memcmp(img, "\x7f" "ELF", 4) != 0) return PROC_ELF_NOTELF;
    if (len < sizeof(elf_ehdr_t)) return PROC_ELF_TRUNC;

    elf_ehdr_t eh;
    memcpy(&eh, img, sizeof(eh));
    if (eh.ident[4] != 1 || eh.ident[5] != 1 || eh.ident[6] != 1 || eh.version != 1)
        return PROC_ELF_CLASS;
    if (eh.type != ET_EXEC || eh.machine != EM_386) return PROC_ELF_TYPE;
    if (eh.phentsize != sizeof(elf_phdr_t) || eh.phnum == 0 || eh.phnum > ELF_MAX_PH)
        return PROC_ELF_PHDR;

    uint32_t tbl = (uint32_t)eh.phnum * sizeof(elf_phdr_t);    // <= 512, cannot overflow
    if (eh.phoff > len || len - eh.phoff < tbl) return PROC_ELF_TRUNC;

    uint32_t lo[ELF_MAX_PH], hi[ELF_MAX_PH], fsz[ELF_MAX_PH];
    int n = 0;
    uint32_t span_end = PROC_BASE;

    for (uint32_t i = 0; i < eh.phnum; i++){
        elf_phdr_t ph;
        memcpy(&ph, img + eh.phoff + i * sizeof(ph), sizeof(ph));

        // A program that wants an interpreter or dynamic linking cannot be
        // run by a loader that has neither.
        if (ph.type == PT_DYNAMIC || ph.type == PT_INTERP) return PROC_ELF_TYPE;
        if (ph.type != PT_LOAD) continue;

        if (ph.filesz > ph.memsz) return PROC_ELF_FILESZ;
        if (ph.offset > len || len - ph.offset < ph.filesz) return PROC_ELF_FILESZ;
        if (ph.memsz == 0) continue;

        // The whole segment must lie in [PROC_BASE, PROC_BASE + PROC_IMAGE_MAX),
        // tested by subtraction so no sum can wrap.
        if (ph.vaddr < PROC_BASE) return PROC_ELF_SEGMENT;
        uint32_t rel = ph.vaddr - PROC_BASE;
        if (rel > PROC_IMAGE_MAX || ph.memsz > PROC_IMAGE_MAX - rel) return PROC_ELF_SEGMENT;

        uint32_t end = ph.vaddr + ph.memsz;
        for (int j = 0; j < n; j++)
            if (ph.vaddr < hi[j] && lo[j] < end) return PROC_ELF_SEGMENT;   // overlap

        lo[n] = ph.vaddr; hi[n] = end; fsz[n] = ph.filesz; n++;
        if (end > span_end) span_end = end;
    }
    if (n == 0) return PROC_ELF_NOLOAD;

    int entry_ok = 0;
    for (int j = 0; j < n; j++)
        if (eh.entry >= lo[j] && eh.entry - lo[j] < fsz[j]) entry_ok = 1;
    if (!entry_ok) return PROC_ELF_ENTRY;

    if (out){
        out->entry = eh.entry;
        out->span  = (span_end - PROC_BASE + PMM_FRAME_SIZE - 1) & ~(PMM_FRAME_SIZE - 1);
        out->nload = n;
    }
    return PROC_ELF_OK;
}

// --------------------------------------------------------- address space

// The image has been through proc_elf_parse, so its segments are known to be
// in range and non-overlapping. The whole span is one demand-zero region and
// only the file-backed bytes are copied in: bss (memsz beyond filesz) and any
// gap between segments simply arrive as zero pages when first touched.
static int load_elf(vm_space_t* sp, const uint8_t* img){
    elf_ehdr_t eh;
    memcpy(&eh, img, sizeof(eh));
    for (uint32_t i = 0; i < eh.phnum; i++){
        elf_phdr_t ph;
        memcpy(&ph, img + eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type != PT_LOAD || ph.memsz == 0 || ph.filesz == 0) continue;
        if (vm_copy_to(sp, ph.vaddr, img + ph.offset, ph.filesz) != 0) return -1;
    }
    return 0;
}

// ----------------------------------------------------------------- argv

// NUL-separated strings, argv[0] first, as built by proc_spawn_args.
typedef struct {
    int  argc;
    uint32_t used;
    char text[PROC_ARGS_BYTES + 16];
} argblock_t;

static int args_add(argblock_t* a, const char* s, uint32_t n){
    if (a->argc >= PROC_ARGS_MAX) return -1;
    if (a->used + n + 1 > sizeof(a->text)) return -1;
    memcpy(a->text + a->used, s, n);
    a->text[a->used + n] = 0;
    a->used += n + 1;
    a->argc++;
    return 0;
}

static int args_build(argblock_t* a, const char* path, const char* args){
    memset(a, 0, sizeof(*a));
    if (args_add(a, path, strlen(path)) != 0) return -1;
    while (args && *args){
        while (*args == ' ' || *args == '\t') args++;
        if (!*args) break;
        const char* e = args;
        while (*e && *e != ' ' && *e != '\t') e++;
        if (args_add(a, args, (uint32_t)(e - args)) != 0) return -1;
        args = e;
    }
    return 0;
}

// Lays argc/argv out at the top of the stack and returns the initial esp, or
// 0 if the pages could not be had. The frame is what a call to
// `void _start(int argc, char** argv)` would have left: a dummy return
// address, then the two arguments, with argc on a 16-byte boundary as the
// i386 ABI expects. It is built in a scratch page and copied in through the
// VM, which faults the stack page in.
static uint32_t stack_setup(vm_space_t* space, const argblock_t* a){
    uint8_t page[PMM_FRAME_SIZE];
    uint32_t page_v = PROC_STACK_TOP - PMM_FRAME_SIZE;
    uint32_t sp = PMM_FRAME_SIZE;
    uint32_t argv_v[PROC_ARGS_MAX];
    memset(page, 0, sizeof(page));

    const char* s = a->text;
    const char* ptr[PROC_ARGS_MAX];
    for (int i = 0; i < a->argc; i++){ ptr[i] = s; s += strlen(s) + 1; }

    for (int i = a->argc - 1; i >= 0; i--){
        uint32_t l = strlen(ptr[i]) + 1;
        sp -= l;
        memcpy(page + sp, ptr[i], l);
        argv_v[i] = page_v + sp;
    }
    sp &= ~3u;
    sp -= 4u * ((uint32_t)a->argc + 1);
    uint32_t* av = (uint32_t*)(page + sp);
    for (int i = 0; i < a->argc; i++) av[i] = argv_v[i];
    av[a->argc] = 0;
    uint32_t argv_ptr = page_v + sp;

    uint32_t A = (sp - 8) & ~15u;
    *(uint32_t*)(page + A - 4) = 0;                 // return address: none
    *(uint32_t*)(page + A)     = (uint32_t)a->argc;
    *(uint32_t*)(page + A + 4) = argv_ptr;
    if (vm_copy_to(space, page_v + A - 4, page + A - 4, PMM_FRAME_SIZE - (A - 4)) != 0) return 0;
    return page_v + A - 4;
}

// ------------------------------------------------------------- lifecycle

// fds 0/1/2 on /dev/console: 0 read-only, 1 and 2 (one description) write-only.
static void console_fds(proc_t* p){
    vfs_fd_init(&p->fdt);
    vfs_open(&p->fdt, "/", "/dev/console", VFS_O_RDONLY);
    vfs_open(&p->fdt, "/", "/dev/console", VFS_O_WRONLY);
    vfs_dup2(&p->fdt, 1, 2);
}

typedef struct {
    proc_t*    p;
    uint32_t   entry;
    uint32_t   esp;
} launch_t;

// The task body. It runs in ring 0 just long enough to install its own
// address space and tell the CPU which stack to use for traps, then drops to
// ring 3 and never returns.
static void proc_task(void* arg){
    launch_t* l = (launch_t*)arg;
    proc_t*   p = l->p;
    uint32_t  entry = l->entry;
    uint32_t  esp = l->esp;
    kfree(l);

    p->tid = task_current_id();
    task_set_address_space(p->page_dir);
    current_proc = p;

    // Traps from ring 3 land on this task's own kernel stack. Anything else
    // would have two tasks sharing one stack the moment both take an
    // interrupt.
    tss_set_kernel_stack(task_kernel_stack_top());

    paging_switch(p->page_dir);
    usermode_jump(entry, esp);

    // usermode_jump does not return; if it somehow does, do not fall off the
    // end of the task into whatever follows it in memory.
    proc_exit(-1);
}

static const char* elf_err(int rc){
    switch (rc){
        case PROC_ELF_TRUNC:   return "truncated header";
        case PROC_ELF_CLASS:   return "not ELF32 little-endian v1";
        case PROC_ELF_TYPE:    return "not a static i386 executable";
        case PROC_ELF_PHDR:    return "bad program header table";
        case PROC_ELF_SEGMENT: return "segment outside the user range";
        case PROC_ELF_FILESZ:  return "segment data past end of file";
        case PROC_ELF_ENTRY:   return "entry outside loadable code";
        case PROC_ELF_NOLOAD:  return "no loadable segment";
        default:               return "invalid";
    }
}

int proc_spawn(const char* path){ return proc_spawn_ex(path, 0, 0, 0); }
int proc_spawn_args(const char* path, const char* args){ return proc_spawn_ex(path, args, 0, 0); }

// Drops everything a half-built process owns.
static void proc_abort(proc_t* p){
    vfs_fd_closeall(&p->fdt);
    if (p->vm) vm_space_destroy(p->vm);
    p->vm = 0;
    p->used = 0;
}

int proc_spawn_ex(const char* path, const char* args, const char* cwd, int flags){
    proc_t* par = current_proc;
    if (!path || !path[0]) return -1;
    const char* base = cwd ? cwd : (par ? par->cwd : "/");

    vfs_file_t* vf;
    int rc = vfs_open_file(base, path, VFS_O_RDONLY, &vf);
    if (rc < 0){
        kprintf("[proc] %s: cannot open (%d)\n", path, rc);
        return -1;
    }
    vfs_stat_t st;
    vfs_file_fstat(vf, &st);
    // ELF files can carry more than they load (headers, symbols), hence the
    // looser bound here; the segments themselves are held to PROC_IMAGE_MAX.
    if (st.type != VFS_T_FILE || st.size == 0 || st.size > 2u * PROC_IMAGE_MAX){
        kprintf("[proc] %s: not a loadable image (%u bytes)\n", path, st.size);
        vfs_file_put(vf);
        return -1;
    }

    uint8_t* image = (uint8_t*)kmalloc(st.size);
    if (!image){ vfs_file_put(vf); return -1; }
    uint32_t got = 0;
    while (got < st.size){
        int n = vfs_file_read(vf, image + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    vfs_file_put(vf);
    if (got != st.size){ kfree(image); return -1; }

    // The process is named after the last component of the resolved path.
    char full[VFS_PATH_MAX];
    const char* leaf = path;
    if (vfs_normalize(base, path, full, sizeof(full)) == 0){
        leaf = full;
        for (const char* c = full; *c; c++) if (*c == '/' && c[1]) leaf = c + 1;
    }

    proc_elf_t ei;
    rc = proc_elf_parse(image, st.size, &ei);
    int is_elf = (rc == PROC_ELF_OK);
    if (rc == PROC_ELF_NOTELF){
        if (st.size > PROC_IMAGE_MAX){
            kprintf("[proc] %s: flat image too large (%u bytes)\n", path, st.size);
            kfree(image); return -1;
        }
        ei.entry = PROC_BASE;
        ei.span  = (st.size + PMM_FRAME_SIZE - 1) & ~(PMM_FRAME_SIZE - 1);
    } else if (rc != PROC_ELF_OK){
        kprintf("[proc] %s: bad ELF (%s)\n", path, elf_err(rc));
        kfree(image); return -1;
    }

    argblock_t* ab = (argblock_t*)kmalloc(sizeof(argblock_t));
    launch_t* l = (launch_t*)kmalloc(sizeof(launch_t));
    if (!ab || !l || args_build(ab, path, args) != 0){
        if (ab && l) kprintf("[proc] %s: arguments too long\n", path);
        kfree(ab); kfree(l); kfree(image); return -1;
    }

    proc_t* p = claim_slot();
    if (!p){ kfree(ab); kfree(l); kfree(image); return -1; }

    p->vm = vm_space_create(PROC_BASE + ei.span);
    if (!p->vm){ p->used = 0; kfree(ab); kfree(l); kfree(image); return -1; }
    p->page_dir = vm_space_dir(p->vm);
    p->ppid     = par ? par->pid : 0;
    p->pid      = next_pid++;
    strncpy(p->name, leaf, sizeof(p->name) - 1);
    strncpy(p->cwd, base, sizeof(p->cwd) - 1);
    if ((flags & PROC_SPAWN_INHERIT) && par) vfs_fd_clone(&p->fdt, &par->fdt);
    else console_fds(p);

    int lrc = vm_map_anon(p->vm, PROC_BASE, ei.span, VM_PROT_RWX_USER);
    if (lrc == 0)
        lrc = is_elf ? load_elf(p->vm, image) : vm_copy_to(p->vm, PROC_BASE, image, st.size);
    kfree(image);
    if (lrc == 0) lrc = vm_map_stack(p->vm, PROC_STACK_TOP, PROC_STACK_SIZE, VM_STACK_MAX);
    uint32_t esp = 0;
    if (lrc == 0) esp = stack_setup(p->vm, ab);
    kfree(ab);
    if (lrc != 0 || !esp){
        proc_abort(p);
        kfree(l);
        return -1;
    }

    l->p = p;
    l->entry = ei.entry;
    l->esp = esp;

    int id = task_create(p->name, proc_task, l);
    if (id < 0){
        proc_abort(p);
        kfree(l);
        return -1;
    }
    // Known from here on, not just once the task first runs, so a kill that
    // arrives before then still finds its target.
    p->tid = (uint32_t)id;

    kprintf("[proc] %s loaded (%s): %u bytes, entry %p, pid %u\n",
            p->name, is_elf ? "elf" : "flat", st.size, (void*)ei.entry, p->pid);
    return (int)p->pid;
}

static void record_exit(const proc_t* p, int code){
    exits[exit_next].pid   = p->pid;
    exits[exit_next].ppid  = p->ppid;
    exits[exit_next].code  = code;
    exits[exit_next].valid = 1;
    exit_next = (exit_next + 1) % EXIT_RECS;
}

void proc_exit(int code){
    proc_t* p = current_proc;

    // Back to the kernel's own directory before releasing this one, or the
    // next instruction executes with CR3 pointing at freed memory.
    paging_switch(paging_kernel_dir());
    task_set_address_space(paging_kernel_dir());
    current_proc = 0;

    if (p){
        kprintf("[proc] %s exited with %d\n", p->name, code);
        vfs_fd_closeall(&p->fdt);         // flushes files; closing a pipe end wakes its peer
        signal_task_exit(task_current_id());
        p->exit_code = code;
        p->exited    = 1;
        vm_space_destroy(p->vm);
        p->vm = 0;
        uint32_t f = irq_save();          // record and slot release appear together
        record_exit(p, code);
        p->used = 0;
        irq_restore(f);
        waitq_wake_all(&child_wq);
    }
    task_exit();
}

int proc_wait(int pid, int* code){
    if (pid <= 0) return -1;
    uint32_t me = proc_current_pid();
    for (;;){
        // Read before looking, so an exit landing between the look and the
        // sleep makes the sleep return at once instead of being missed.
        uint32_t seq = waitq_seq(&child_wq);
        uint32_t f = irq_save();
        for (int i = 0; i < EXIT_RECS; i++){
            if (exits[i].valid && exits[i].pid == (uint32_t)pid && exits[i].ppid == me){
                if (code) *code = exits[i].code;
                exits[i].valid = 0;
                irq_restore(f);
                return pid;
            }
        }
        proc_t* c = by_pid((uint32_t)pid);
        int mine = c && c->ppid == me;
        irq_restore(f);
        if (!mine) return -1;             // never existed, not ours, or already reaped
        if (signal_pending_current()) return VFS_EINTR;
        // The timeout is only so a signal aimed at the waiter is noticed.
        waitq_wait_seq(&child_wq, seq, 100);
    }
}

// Called by the scheduler after it switches stacks, so the pointer used for
// syscall bounds checks always describes the task that is actually running.
void proc_note_switch(uint32_t tid){
    current_proc = by_tid(tid);
}

// ------------------------------------------------------------ test hooks

proc_t* proc_test_create(void){
    proc_t* p = claim_slot();
    if (!p) return 0;
    p->vm = vm_space_create(PROC_BASE + PMM_FRAME_SIZE);
    if (!p->vm){ p->used = 0; return 0; }
    p->page_dir = vm_space_dir(p->vm);
    p->pid = next_pid++;
    strncpy(p->name, "ktest", sizeof(p->name) - 1);
    strcpy(p->cwd, "/");
    console_fds(p);
    if (vm_map_anon(p->vm, PROC_BASE, PMM_FRAME_SIZE, VM_PROT_RWX_USER) != 0 ||
        vm_map_stack(p->vm, PROC_STACK_TOP, PROC_STACK_SIZE, VM_STACK_MAX) != 0){
        proc_test_destroy(p);
        return 0;
    }
    return p;
}

// Binding makes the calling kernel task look like the process's thread: the
// scheduler then finds it through tid and restores current_proc and CR3 on
// every switch, exactly as it would for a real one.
void proc_test_bind(proc_t* p){
    if (p){
        p->tid = task_current_id();
        task_set_address_space(p->page_dir);
        current_proc = p;
        paging_switch(p->page_dir);
    } else {
        if (current_proc) current_proc->tid = NO_TID;
        current_proc = 0;
        task_set_address_space(paging_kernel_dir());
        paging_switch(paging_kernel_dir());
    }
}

void proc_test_destroy(proc_t* p){
    if (!p) return;
    if (current_proc == p) proc_test_bind(0);
    proc_abort(p);
}
