#pragma once
#include <stdint.h>

// Virtual memory for user processes: what an address space is allowed to
// contain (a sorted list of regions), and the machinery that fills it in
// lazily -- demand-zero pages, a stack that grows downward, and
// copy-on-write sharing after a fork.
//
// The page tables stay the single source of truth for what is *mapped*; the
// region list only says what is *allowed to be*. A fault on an address inside
// a region gets a page; a fault outside every region kills the process.
//
// Typical use by the process layer:
//   spawn:  sp = vm_space_create(image_end);
//           vm_map_anon(sp, PROC_BASE, image_len, VM_PROT_RWX_USER);
//           vm_copy_to(sp, PROC_BASE, image, len);         // faults pages in
//           vm_map_stack(sp, PROC_STACK_TOP, PROC_STACK_SIZE, VM_STACK_MAX);
//           paging_switch(vm_space_dir(sp));
//   exit:   paging_switch(paging_kernel_dir()); vm_space_destroy(sp);
//   brk:    vm_brk(sp, addr)          mmap: vm_mmap_anon(sp, len, prot)
//   fork:   child = vm_fork_space(sp)

#define VM_PROT_R     1u
#define VM_PROT_W     2u
#define VM_PROT_X     4u      // recorded only: 32-bit non-PAE paging has no NX bit
#define VM_PROT_USER  8u
#define VM_PROT_RW_USER   (VM_PROT_R | VM_PROT_W | VM_PROT_USER)
#define VM_PROT_RWX_USER  (VM_PROT_R | VM_PROT_W | VM_PROT_X | VM_PROT_USER)

typedef enum {
    VM_KIND_ANON  = 0,    // zero-filled on first touch
    VM_KIND_STACK = 1,    // as anon, and the region may grow downward
    VM_KIND_FILE  = 2,    // file-backed; STUB: pages are zero-filled for now
} vm_kind_t;

#define VM_FLAG_GROWSDOWN 1u
#define VM_FLAG_HEAP      2u    // the brk region

typedef struct {
    uint32_t start, end;        // [start, end), page aligned
    uint8_t  prot;              // VM_PROT_*
    uint8_t  kind;              // vm_kind_t
    uint8_t  flags;             // VM_FLAG_*
    uint32_t file_id;           // VM_KIND_FILE only (unused by the stub)
    uint32_t file_off;
} vm_region_t;

#define VM_MAX_REGIONS 24

// Layout policy. The kernel occupies the identity map at the bottom of the
// address space; user regions must lie above it and below VM_USER_TOP.
#define VM_USER_TOP      0xC0000000u
#define VM_HEAP_GAP      0x00010000u   // unmapped guard between image end and heap
#define VM_STACK_MAX     0x00100000u   // default cap on stack growth (1 MB)
#define VM_MMAP_BASE     0x50000000u
#define VM_MMAP_LIMIT    0x70000000u
#define VM_MAX_PAGES     16384u        // resident-page cap per space (64 MB)

typedef struct vm_space {
    uint32_t    pd;                    // physical address of the page directory
    vm_region_t regions[VM_MAX_REGIONS];   // sorted by start, non-overlapping
    int         nregions;
    uint32_t    heap_start, brk, heap_limit;
    uint32_t    mmap_base, mmap_limit;
    uint32_t    stack_top, stack_limit;    // stack may grow down to stack_limit
    uint32_t    pages;                 // resident user pages (rss)
    uint32_t    max_pages;
    struct vm_space* next;             // registry link (fault lookup by CR3)
} vm_space_t;

// --------------------------------------------------------------- lifecycle

// A fresh, empty user address space. The heap will start VM_HEAP_GAP above
// `image_end` (rounded up to a page). NULL if out of memory.
vm_space_t* vm_space_create(uint32_t image_end);

// Releases every user page (respecting COW sharers), the page tables and the
// directory. Switches to the kernel directory first if `sp` is loaded.
void        vm_space_destroy(vm_space_t* sp);

// A copy-on-write clone: same regions, same brk, every present page shared
// read-only and marked COW (in the parent too). NULL if out of memory, with
// the parent left consistent.
vm_space_t* vm_fork_space(vm_space_t* parent);

uint32_t    vm_space_dir(const vm_space_t* sp);

// ----------------------------------------------------------------- regions
// All addresses and lengths are page aligned (lengths are rounded up). Return
// 0 on success, -1 on a bad range, overlap, kernel range, or no free slot.

int  vm_map_anon(vm_space_t* sp, uint32_t addr, uint32_t len, uint32_t prot);
int  vm_map_file(vm_space_t* sp, uint32_t addr, uint32_t len, uint32_t prot,
                 uint32_t file_id, uint32_t file_off);        // stub: zero-filled
// The stack region [top - initial, top), growing down to top - max_size.
int  vm_map_stack(vm_space_t* sp, uint32_t top, uint32_t initial, uint32_t max_size);

// Removes [addr, addr+len) from the space, splitting regions as needed and
// dropping one reference on every page in it. Refuses the brk region (use
// vm_brk). Unmapped holes in the range are fine.
int  vm_unmap(vm_space_t* sp, uint32_t addr, uint32_t len);

// The program break. 0 queries. Returns the break after the call; if it is
// not the value asked for, the request was refused (below the heap start,
// above the hard limit, or into another region) and nothing changed.
uint32_t vm_brk(vm_space_t* sp, uint32_t new_end);

// A fresh anonymous mapping somewhere in the mmap area, with an unmapped page
// on each side. Returns its address, or 0.
uint32_t vm_mmap_anon(vm_space_t* sp, uint32_t len, uint32_t prot);

const vm_region_t* vm_find_region(const vm_space_t* sp, uint32_t addr);

// ------------------------------------------------------------------ faults

#define VM_FAULT_HANDLED  0     // retry the instruction
#define VM_FAULT_KILLED  (-1)   // not resolvable: terminate the process

// Error-code bits of a #PF.
#define VM_ERR_PRESENT 1u
#define VM_ERR_WRITE   2u
#define VM_ERR_USER    4u

// Resolves a fault at `addr` in `sp`: demand-zero, stack growth, COW break.
// Works on any space, loaded or not, so it can be driven from a test.
int  vm_handle_fault(vm_space_t* sp, uint32_t addr, uint32_t err);

// The exception path: finds the space whose directory is in CR3 and handles
// the fault there. KILLED if CR3 is not a registered space.
int  vm_page_fault(uint32_t addr, uint32_t err);

// ---------------------------------------------------------- kernel access
// For syscalls and loaders that touch a process's memory from the kernel.

// True if [addr, addr+len) lies in user regions (all writable if `write`).
// Adjacent regions count as one; nothing is mapped or grown.
int  vm_range_ok(const vm_space_t* sp, uint32_t addr, uint32_t len, int write);

// Faults the pages in (breaking COW if `write`) and copies. 0 or -1.
int  vm_copy_to(vm_space_t* sp, uint32_t dst, const void* src, uint32_t len);
int  vm_copy_from(vm_space_t* sp, uint32_t src, void* dst, uint32_t len);

// A kernel pointer (identity-mapped frame) to the byte at `va`, after making
// the page present and, if `write`, private. NULL if the fault is fatal.
void* vm_kaddr(vm_space_t* sp, uint32_t va, int write);
