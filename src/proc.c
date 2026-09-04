// src/proc.c — user processes: address spaces, loading, and the ring-3 entry
//
// A process is a page directory plus a kernel task to host it. The task exists
// because the scheduler only knows how to switch kernel stacks; the directory
// exists because that is what makes one process unable to see another.
//
// Loading is deliberately the simplest thing that is honest: a flat binary,
// linked to run at PROC_BASE, copied into freshly allocated frames. ELF adds
// a header parser and relocation handling without changing anything about the
// privilege boundary, which is the part worth building first.
#include <stdint.h>
#include "header/proc.h"
#include "header/paging.h"
#include "header/pmm.h"
#include "header/gdt.h"
#include "header/task.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/fat32.h"
#include "header/shell.h"
#include "header/kprintf.h"

extern void usermode_jump(uint32_t entry, uint32_t stack_top);

static proc_t procs[PROC_MAX];

// The process the CPU is currently executing, so a syscall can bounds-check
// pointers against the right image. Set on entry to ring 3 and whenever the
// scheduler switches address spaces.
static proc_t* current_proc = 0;

void proc_init(void){
    memset(procs, 0, sizeof(procs));
    current_proc = 0;
}

static proc_t* slot(void){
    for (int i = 0; i < PROC_MAX; i++) if (!procs[i].used) return &procs[i];
    return 0;
}

static proc_t* by_pid(uint32_t pid){
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
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

// ------------------------------------------------------------ validation

int proc_valid_range(uint32_t addr, uint32_t len){
    proc_t* p = current_proc;
    if (!p) return 0;
    if (len == 0) return 1;

    // Reject the wrap first: without this, addr near 4 GB with a large len
    // passes an end-of-range check that overflowed back to a small number.
    if (addr + len < addr) return 0;

    uint32_t img_lo = p->image_base, img_hi = p->image_base + p->image_len;
    uint32_t stk_lo = PROC_STACK_TOP - PROC_STACK_SIZE, stk_hi = PROC_STACK_TOP;

    if (addr >= img_lo && addr + len <= img_hi) return 1;
    if (addr >= stk_lo && addr + len <= stk_hi) return 1;
    return 0;
}

int proc_valid_string(uint32_t addr){
    // Walk it a byte at a time, checking each one, so a string that runs off
    // the end of the process's memory is rejected at the boundary rather than
    // after the kernel has already read past it.
    for (uint32_t i = 0; i < 256; i++){
        if (!proc_valid_range(addr + i, 1)) return 0;
        if (*(const char*)(addr + i) == 0) return 1;
    }
    return 0;   // unterminated within a sane length
}

// ------------------------------------------------------------ filesystem

// Names come from a user program, so they are checked before they reach the
// filesystem: 8.3 only, no path separators, nothing that could walk out of
// the working directory.
static int name_ok(const char* n){
    if (!n || !n[0]) return 0;
    uint32_t len = strlen(n);
    if (len > 12) return 0;
    for (uint32_t i = 0; i < len; i++){
        char c = n[i];
        if (c == '/' || c == '\\' || c == ':') return 0;
        if (c < 32 || c > 126) return 0;
    }
    return 1;
}

int proc_open(const char* name){
    if (!name_ok(name)) return -1;
    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), name, &f) != 0) return -1;
    if (f.is_dir) return -1;
    return (int)f.size;
}

uint32_t proc_read(const char* name, uint8_t* out, uint32_t cap){
    if (!name_ok(name)) return 0;
    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), name, &f) != 0) return 0;
    if (f.is_dir) return 0;
    return fat32_read_file(&f, out, cap);
}

// --------------------------------------------------------- address space

// Maps `bytes` of fresh, zeroed memory at `virt` in the given directory, and
// copies `src` into it if there is anything to copy. Returns 0 on success.
//
// The copy goes through the identity map rather than through the new address
// space: every physical frame is already reachable at its own address from
// the kernel's directory, so there is no need to switch CR3 to populate one.
static int map_and_fill(uint32_t pd, uint32_t virt, const uint8_t* src,
                        uint32_t src_len, uint32_t bytes){
    for (uint32_t off = 0; off < bytes; off += PMM_FRAME_SIZE){
        void* frame = pmm_alloc_frame();
        if (!frame) return -1;

        uint8_t* dst = (uint8_t*)frame;
        memset(dst, 0, PMM_FRAME_SIZE);
        if (src && off < src_len){
            uint32_t chunk = src_len - off;
            if (chunk > PMM_FRAME_SIZE) chunk = PMM_FRAME_SIZE;
            memcpy(dst, src + off, chunk);
        }

        paging_map_in(pd, (uint32_t)frame, virt + off,
                      PAGE_RW | PAGE_USER);
    }
    return 0;
}

static void free_address_space(uint32_t pd, uint32_t image_len){
    // Only the pages this process owns: the kernel's tables are shared with
    // every other directory and must not be released with one of them.
    for (uint32_t off = 0; off < image_len; off += PMM_FRAME_SIZE){
        uint32_t phys = paging_phys_of(pd, PROC_BASE + off);
        if (phys) pmm_free_frame((void*)phys);
    }
    for (uint32_t v = PROC_STACK_TOP - PROC_STACK_SIZE; v < PROC_STACK_TOP;
         v += PMM_FRAME_SIZE){
        uint32_t phys = paging_phys_of(pd, v);
        if (phys) pmm_free_frame((void*)phys);
    }
    paging_free_address_space(pd);
}

// ------------------------------------------------------------- lifecycle

typedef struct {
    proc_t*  p;
    uint32_t entry;
} launch_t;

// The task body. It runs in ring 0 just long enough to install its own
// address space and tell the CPU which stack to use for traps, then drops to
// ring 3 and never returns.
static void proc_task(void* arg){
    launch_t* l = (launch_t*)arg;
    proc_t*   p = l->p;
    uint32_t  entry = l->entry;
    kfree(l);

    p->pid = task_current_id();
    task_set_address_space(p->page_dir);
    current_proc = p;

    // Traps from ring 3 land on this task's own kernel stack. Anything else
    // would have two tasks sharing one stack the moment both take an
    // interrupt.
    tss_set_kernel_stack(task_kernel_stack_top());

    paging_switch(p->page_dir);
    usermode_jump(entry, PROC_STACK_TOP);

    // usermode_jump does not return; if it somehow does, do not fall off the
    // end of the task into whatever follows it in memory.
    proc_exit(-1);
}

int proc_spawn(const char* path){
    if (!name_ok(path)) return -1;

    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), path, &f) != 0){
        kprintf("[proc] %s: not found\n", path);
        return -1;
    }
    if (f.is_dir || f.size == 0 || f.size > PROC_IMAGE_MAX){
        kprintf("[proc] %s: not a loadable image (%u bytes)\n", path, f.size);
        return -1;
    }

    uint8_t* image = (uint8_t*)kmalloc(f.size);
    if (!image) return -1;
    uint32_t got = fat32_read_file(&f, image, f.size);
    if (got != f.size){ kfree(image); return -1; }

    proc_t* p = slot();
    if (!p){ kfree(image); return -1; }

    uint32_t pd = paging_new_address_space();
    if (!pd){ kfree(image); return -1; }

    uint32_t image_pages = (f.size + PMM_FRAME_SIZE - 1) & ~(PMM_FRAME_SIZE - 1);

    if (map_and_fill(pd, PROC_BASE, image, f.size, image_pages) != 0 ||
        map_and_fill(pd, PROC_STACK_TOP - PROC_STACK_SIZE, 0, 0, PROC_STACK_SIZE) != 0){
        kfree(image);
        free_address_space(pd, image_pages);
        return -1;
    }
    kfree(image);

    memset(p, 0, sizeof(*p));
    p->used       = 1;
    p->page_dir   = pd;
    p->image_base = PROC_BASE;
    p->image_len  = image_pages;
    p->brk        = PROC_BASE + image_pages;
    strncpy(p->name, path, sizeof(p->name) - 1);

    launch_t* l = (launch_t*)kmalloc(sizeof(launch_t));
    if (!l){ p->used = 0; free_address_space(pd, image_pages); return -1; }
    l->p = p;
    l->entry = PROC_BASE;

    int id = task_create(path, proc_task, l);
    if (id < 0){
        kfree(l);
        p->used = 0;
        free_address_space(pd, image_pages);
        return -1;
    }

    kprintf("[proc] %s loaded: %u bytes at %p, task %d\n",
            path, f.size, (void*)PROC_BASE, id);
    return id;
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
        p->exit_code = code;
        p->exited    = 1;
        free_address_space(p->page_dir, p->image_len);
        p->used = 0;
    }
    task_exit();
}

// Called by the scheduler after it switches stacks, so the pointer used for
// syscall bounds checks always describes the task that is actually running.
void proc_note_switch(uint32_t pid){
    current_proc = by_pid(pid);
}
