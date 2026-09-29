// src/vm.c — per-process virtual memory: regions, demand paging, COW
//
// Everything here works on a page directory by its physical address and
// reaches frames through the kernel's identity map, so none of it needs the
// space it is editing to be the one loaded in CR3. That is what lets a fault
// be resolved for a task that is not running, and what lets the tests drive
// the fault path without a ring-3 program.
#include <stdint.h>
#include <stddef.h>
#include "header/vm.h"
#include "header/paging.h"
#include "header/pmm.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/irqctl.h"

#define PG        PMM_FRAME_SIZE
#define PG_MASK   (PMM_FRAME_SIZE - 1u)
#define FRAME(e)  ((e) & 0xFFFFF000u)

static vm_space_t* spaces = 0;      // every live space, for lookup by CR3

static inline uint32_t align_up(uint32_t v){ return (v + PG_MASK) & ~PG_MASK; }

// Below this is the kernel's identity map (whatever RAM the machine has,
// rounded up to a whole page-directory entry, since user tables are created
// per 4 MB slice).
static uint32_t user_min(void){
    return (pmm_ram_top() + 0x3FFFFFu) & ~0x3FFFFFu;
}

// ----------------------------------------------------------- region list

static int find_idx(const vm_space_t* sp, uint32_t addr){
    for (int i = 0; i < sp->nregions; i++)
        if (addr >= sp->regions[i].start && addr < sp->regions[i].end) return i;
    return -1;
}

const vm_region_t* vm_find_region(const vm_space_t* sp, uint32_t addr){
    int i = find_idx(sp, addr);
    return i < 0 ? 0 : &sp->regions[i];
}

static int overlaps(const vm_space_t* sp, uint32_t s, uint32_t e, int skip){
    for (int i = 0; i < sp->nregions; i++){
        if (i == skip) continue;
        if (sp->regions[i].start < e && s < sp->regions[i].end) return 1;
    }
    return 0;
}

static int insert_region(vm_space_t* sp, const vm_region_t* r){
    if (sp->nregions >= VM_MAX_REGIONS) return -1;
    int i = sp->nregions;
    while (i > 0 && sp->regions[i - 1].start > r->start){
        sp->regions[i] = sp->regions[i - 1];
        i--;
    }
    sp->regions[i] = *r;
    sp->nregions++;
    return i;
}

static void remove_region(vm_space_t* sp, int i){
    for (; i + 1 < sp->nregions; i++) sp->regions[i] = sp->regions[i + 1];
    sp->nregions--;
}

// ------------------------------------------------------------- page ops

static uint32_t pte_flags(uint32_t prot){
    return PAGE_PRESENT | ((prot & VM_PROT_W) ? PAGE_RW : 0)
                        | ((prot & VM_PROT_USER) ? PAGE_USER : 0);
}

// Drops the mapping of every present page in [s, e) and one reference on each
// frame. Skips whole 4 MB slices that have no page table.
static void release_range(vm_space_t* sp, uint32_t s, uint32_t e){
    uint32_t va = s;
    while (va < e){
        uint32_t* pte = paging_pte(sp->pd, va, 0);
        if (!pte){ va = (va | 0x3FFFFFu) + 1u; continue; }
        if (*pte & PAGE_PRESENT){
            pmm_free_frame((void*)FRAME(*pte));
            *pte = 0;
            paging_flush_page(va);
            if (sp->pages) sp->pages--;
        }
        va += PG;
    }
}

static int map_zero_page(vm_space_t* sp, uint32_t va, uint32_t prot){
    if (sp->pages >= sp->max_pages) return -1;
    void* f = pmm_alloc_frame();
    if (!f) return -1;
    memset(f, 0, PG);
    uint32_t* pte = paging_pte(sp->pd, va, 1);
    if (!pte){ pmm_free_frame(f); return -1; }
    *pte = (uint32_t)f | pte_flags(prot);
    paging_flush_page(va);
    sp->pages++;
    return 0;
}

// Write to a shared page. The last sharer just takes the page back; anyone
// else gets a private copy and lets go of the shared one.
static int break_cow(uint32_t* pte, uint32_t va){
    uint32_t old = FRAME(*pte);
    if (pmm_frame_refs((void*)old) <= 1){
        *pte = (*pte & ~PAGE_COW) | PAGE_RW;
    } else {
        void* n = pmm_alloc_frame();
        if (!n) return -1;
        memcpy(n, (void*)old, PG);
        *pte = ((uint32_t)n) | ((*pte & 0xFFFu) & ~PAGE_COW) | PAGE_RW;
        pmm_free_frame((void*)old);
    }
    paging_flush_page(va);
    return 0;
}

// ---------------------------------------------------------------- faults

int vm_handle_fault(vm_space_t* sp, uint32_t addr, uint32_t err){
    if (addr >= VM_USER_TOP || addr < user_min()) return VM_FAULT_KILLED;
    uint32_t page  = addr & ~PG_MASK;
    int      write = (err & VM_ERR_WRITE) != 0;

    int idx = find_idx(sp, addr);
    if (idx < 0){
        // Just below a stack that is allowed to grow: extend the region down
        // to the faulting page. Pages in between are demand-zero like the rest.
        for (int i = 0; i < sp->nregions; i++){
            vm_region_t* r = &sp->regions[i];
            if (!(r->flags & VM_FLAG_GROWSDOWN)) continue;
            if (page < r->start && page >= sp->stack_limit
                && !overlaps(sp, page, r->start, i)){
                r->start = page;
                idx = i;
            }
            break;
        }
        if (idx < 0) return VM_FAULT_KILLED;
    }
    const vm_region_t* r = &sp->regions[idx];

    if (write ? !(r->prot & VM_PROT_W) : !(r->prot & (VM_PROT_R | VM_PROT_X)))
        return VM_FAULT_KILLED;
    if ((err & VM_ERR_USER) && !(r->prot & VM_PROT_USER)) return VM_FAULT_KILLED;

    uint32_t* pte = paging_pte(sp->pd, page, 0);
    if (pte && (*pte & PAGE_PRESENT)){
        if (!write || (*pte & PAGE_RW)) return VM_FAULT_HANDLED;   // raced/stale TLB
        if (!(*pte & PAGE_COW)) return VM_FAULT_KILLED;            // genuinely read-only
        return break_cow(pte, page) == 0 ? VM_FAULT_HANDLED : VM_FAULT_KILLED;
    }
    if (err & VM_ERR_PRESENT) return VM_FAULT_KILLED;   // present bit but no PTE: stale

    // Anonymous and stack pages start zeroed. File regions are a stub for
    // now and behave the same way.
    return map_zero_page(sp, page, r->prot) == 0 ? VM_FAULT_HANDLED : VM_FAULT_KILLED;
}

int vm_page_fault(uint32_t addr, uint32_t err){
    uint32_t cr3 = paging_current_dir();
    vm_space_t* sp = 0;
    uint32_t f = irq_save();
    for (vm_space_t* s = spaces; s; s = s->next)
        if (s->pd == cr3){ sp = s; break; }
    irq_restore(f);
    if (!sp) return VM_FAULT_KILLED;
    return vm_handle_fault(sp, addr, err);
}

// ------------------------------------------------------------- lifecycle

vm_space_t* vm_space_create(uint32_t image_end){
    vm_space_t* sp = (vm_space_t*)kmalloc(sizeof(*sp));
    if (!sp) return 0;
    memset(sp, 0, sizeof(*sp));
    sp->pd = paging_new_address_space();
    if (!sp->pd){ kfree(sp); return 0; }

    sp->heap_start  = align_up(image_end) + VM_HEAP_GAP;
    sp->brk         = sp->heap_start;
    sp->heap_limit  = VM_MMAP_BASE - VM_HEAP_GAP;
    sp->mmap_base   = VM_MMAP_BASE;
    sp->mmap_limit  = VM_MMAP_LIMIT;
    sp->max_pages   = VM_MAX_PAGES;

    uint32_t f = irq_save();
    sp->next = spaces;
    spaces = sp;
    irq_restore(f);
    return sp;
}

void vm_space_destroy(vm_space_t* sp){
    if (!sp) return;
    if (paging_current_dir() == sp->pd) paging_switch(paging_kernel_dir());

    uint32_t f = irq_save();
    for (vm_space_t** pp = &spaces; *pp; pp = &(*pp)->next)
        if (*pp == sp){ *pp = sp->next; break; }
    irq_restore(f);

    for (int i = 0; i < sp->nregions; i++)
        release_range(sp, sp->regions[i].start, sp->regions[i].end);
    paging_free_address_space(sp->pd);
    kfree(sp);
}

uint32_t vm_space_dir(const vm_space_t* sp){ return sp->pd; }

vm_space_t* vm_fork_space(vm_space_t* parent){
    vm_space_t* child = (vm_space_t*)kmalloc(sizeof(*child));
    if (!child) return 0;
    *child = *parent;
    child->pd = paging_new_address_space();
    if (!child->pd){ kfree(child); return 0; }
    child->pages = 0;

    uint32_t f = irq_save();
    child->next = spaces;
    spaces = child;
    irq_restore(f);

    int ok = 1;
    for (int i = 0; i < parent->nregions && ok; i++){
        const vm_region_t* r = &parent->regions[i];
        uint32_t va = r->start;
        while (va < r->end){
            uint32_t* src = paging_pte(parent->pd, va, 0);
            if (!src){ va = (va | 0x3FFFFFu) + 1u; continue; }
            if (*src & PAGE_PRESENT){
                uint32_t* dst = paging_pte(child->pd, va, 1);
                if (!dst){ ok = 0; break; }
                uint32_t e = *src;
                // A writable page becomes read-only-and-shared in both; the
                // first writer on either side pays for the copy. Pages the
                // region never allowed writing to are simply shared.
                if ((e & PAGE_RW) && (r->prot & VM_PROT_W)){
                    e = (e & ~PAGE_RW) | PAGE_COW;
                    *src = e;
                }
                pmm_ref_frame((void*)FRAME(e));
                *dst = e;
                child->pages++;
            }
            va += PG;
        }
    }
    paging_flush_if_active(parent->pd);    // its PTEs just lost their RW bits

    if (!ok){ vm_space_destroy(child); return 0; }
    return child;
}

// --------------------------------------------------------------- mapping

static int map_region(vm_space_t* sp, uint32_t addr, uint32_t len, uint32_t prot,
                      uint32_t kind, uint32_t flags, uint32_t fid, uint32_t foff){
    if ((addr & PG_MASK) || len == 0 || (prot & ~0xFu)) return -1;
    uint32_t end = addr + align_up(len);
    if (end <= addr || end > VM_USER_TOP || addr < user_min()) return -1;
    if (overlaps(sp, addr, end, -1)) return -1;
    vm_region_t r = { addr, end, (uint8_t)prot, (uint8_t)kind, (uint8_t)flags, fid, foff };
    return insert_region(sp, &r) < 0 ? -1 : 0;
}

int vm_map_anon(vm_space_t* sp, uint32_t addr, uint32_t len, uint32_t prot){
    return map_region(sp, addr, len, prot, VM_KIND_ANON, 0, 0, 0);
}

int vm_map_file(vm_space_t* sp, uint32_t addr, uint32_t len, uint32_t prot,
                uint32_t file_id, uint32_t file_off){
    return map_region(sp, addr, len, prot, VM_KIND_FILE, 0, file_id, file_off);
}

int vm_map_stack(vm_space_t* sp, uint32_t top, uint32_t initial, uint32_t max_size){
    initial = align_up(initial);
    max_size = align_up(max_size);
    if (initial == 0 || initial > max_size || max_size > top) return -1;
    if (map_region(sp, top - initial, initial, VM_PROT_RW_USER, VM_KIND_STACK,
                   VM_FLAG_GROWSDOWN, 0, 0) != 0) return -1;
    sp->stack_top   = top;
    sp->stack_limit = top - max_size;
    if (sp->stack_limit < user_min()) sp->stack_limit = user_min();
    // The heap must never be able to grow into the stack's reach.
    if (sp->stack_limit > sp->heap_start && sp->stack_limit - VM_HEAP_GAP < sp->heap_limit)
        sp->heap_limit = sp->stack_limit - VM_HEAP_GAP;
    return 0;
}

int vm_unmap(vm_space_t* sp, uint32_t addr, uint32_t len){
    if ((addr & PG_MASK) || len == 0) return -1;
    uint32_t end = addr + align_up(len);
    if (end <= addr || end > VM_USER_TOP) return -1;

    // Validate before touching anything, so a refusal leaves the space as it was.
    for (int i = 0; i < sp->nregions; i++){
        const vm_region_t* r = &sp->regions[i];
        if (r->end <= addr || r->start >= end) continue;
        if (r->flags & VM_FLAG_HEAP) return -1;
        if (r->start < addr && r->end > end && sp->nregions >= VM_MAX_REGIONS) return -1;
    }

    for (int i = 0; i < sp->nregions; i++){
        vm_region_t* r = &sp->regions[i];
        if (r->end <= addr || r->start >= end) continue;
        uint32_t os = r->start > addr ? r->start : addr;
        uint32_t oe = r->end   < end  ? r->end   : end;
        release_range(sp, os, oe);

        if (os <= r->start && oe >= r->end){
            remove_region(sp, i);
            i--;
        } else if (os <= r->start){
            r->file_off += oe - r->start;
            r->start = oe;
        } else if (oe >= r->end){
            r->end = os;
        } else {
            vm_region_t tail = *r;
            tail.start = oe;
            tail.file_off += oe - r->start;
            r->end = os;
            insert_region(sp, &tail);      // slot checked above
        }
    }
    return 0;
}

uint32_t vm_brk(vm_space_t* sp, uint32_t new_end){
    if (new_end == 0 || new_end < sp->heap_start || new_end > sp->heap_limit)
        return sp->brk;

    uint32_t want = align_up(new_end);
    int hi = -1;
    for (int i = 0; i < sp->nregions; i++)
        if (sp->regions[i].flags & VM_FLAG_HEAP){ hi = i; break; }
    uint32_t cur = hi >= 0 ? sp->regions[hi].end : sp->heap_start;

    if (want > cur){
        if (overlaps(sp, cur, want, hi)) return sp->brk;
        if (hi >= 0){
            sp->regions[hi].end = want;
        } else {
            vm_region_t r = { sp->heap_start, want, VM_PROT_RW_USER, VM_KIND_ANON,
                              VM_FLAG_HEAP, 0, 0 };
            if (insert_region(sp, &r) < 0) return sp->brk;
        }
    } else if (want < cur){
        release_range(sp, want, cur);
        if (want == sp->heap_start) remove_region(sp, hi);
        else sp->regions[hi].end = want;
    }
    sp->brk = new_end;
    return sp->brk;
}

uint32_t vm_mmap_anon(vm_space_t* sp, uint32_t len, uint32_t prot){
    if (len == 0 || (prot & ~0xFu)) return 0;
    len = align_up(len);

    // First fit over the sorted list, keeping a page of air on both sides of
    // the new mapping so a run off either end faults instead of walking into
    // a neighbour.
    uint32_t cand = sp->mmap_base;
    for (int i = 0; i < sp->nregions; i++){
        const vm_region_t* r = &sp->regions[i];
        if (r->end + PG <= cand) continue;
        if (cand + len + PG <= r->start) break;
        if (r->end + PG > cand) cand = r->end + PG;
    }
    if (cand + len < cand || cand + len > sp->mmap_limit) return 0;
    return map_region(sp, cand, len, prot, VM_KIND_ANON, 0, 0, 0) == 0 ? cand : 0;
}

// ---------------------------------------------------------- kernel access

int vm_range_ok(const vm_space_t* sp, uint32_t addr, uint32_t len, int write){
    if (len == 0) return 1;
    uint32_t end = addr + len;
    if (end < addr) return 0;
    uint32_t cur = addr;
    while (cur < end){
        const vm_region_t* r = vm_find_region(sp, cur);
        if (!r || !(r->prot & VM_PROT_USER)) return 0;
        if (write ? !(r->prot & VM_PROT_W) : !(r->prot & VM_PROT_R)) return 0;
        cur = r->end;
    }
    return 1;
}

void* vm_kaddr(vm_space_t* sp, uint32_t va, int write){
    uint32_t page = va & ~PG_MASK;
    uint32_t* pte = paging_pte(sp->pd, page, 0);
    int present = pte && (*pte & PAGE_PRESENT);
    if (!present || (write && !(*pte & PAGE_RW))){
        if (vm_handle_fault(sp, va, (present ? VM_ERR_PRESENT : 0u)
                                   | (write ? VM_ERR_WRITE : 0u)) != VM_FAULT_HANDLED)
            return 0;
        pte = paging_pte(sp->pd, page, 0);
        if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    }
    return (void*)(FRAME(*pte) | (va & PG_MASK));
}

int vm_copy_to(vm_space_t* sp, uint32_t dst, const void* src, uint32_t len){
    const uint8_t* s = (const uint8_t*)src;
    while (len){
        uint32_t n = PG - (dst & PG_MASK);
        if (n > len) n = len;
        uint8_t* k = (uint8_t*)vm_kaddr(sp, dst, 1);
        if (!k) return -1;
        memcpy(k, s, n);
        dst += n; s += n; len -= n;
    }
    return 0;
}

int vm_copy_from(vm_space_t* sp, uint32_t src, void* dst, uint32_t len){
    uint8_t* d = (uint8_t*)dst;
    while (len){
        uint32_t n = PG - (src & PG_MASK);
        if (n > len) n = len;
        const uint8_t* k = (const uint8_t*)vm_kaddr(sp, src, 0);
        if (!k) return -1;
        memcpy(d, k, n);
        src += n; d += n; len -= n;
    }
    return 0;
}
