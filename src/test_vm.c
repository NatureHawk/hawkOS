// src/test_vm.c — virtual memory: regions, demand paging, stack growth, COW
//
// Faults are driven two ways. vm_handle_fault() is called directly with a
// synthetic error code, which is enough to test the policy on spaces that are
// not loaded. A few tests also load a space into CR3 and touch its memory from
// the kernel, so the real exception path -- vector 14, CR2, the registry
// lookup, the iret and retry -- is exercised as well. CR0.WP is set, so a
// kernel write to a COW page faults exactly as a user write would.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/paging.h"
#include "header/pmm.h"
#include "header/kheap.h"
#include "header/irqctl.h"
#include "header/vm.h"

#define BASE   0x40000000u
#define PAGE   4096u
#define ERR_W  (VM_ERR_WRITE | VM_ERR_USER)
#define ERR_R  (VM_ERR_USER)

static int present(vm_space_t* sp, uint32_t va){
    uint32_t* pte = paging_pte(vm_space_dir(sp), va, 0);
    return pte && (*pte & PAGE_PRESENT);
}

static uint32_t phys(vm_space_t* sp, uint32_t va){
    return paging_phys_of(vm_space_dir(sp), va);
}

KTEST(vm, demand_zero_fills_on_first_touch){
    uint32_t before = pmm_free_frames();
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_anon(sp, BASE, 3 * PAGE, VM_PROT_RW_USER), 0);

    KT_FALSE(present(sp, BASE + PAGE));                 // lazy: nothing yet
    KT_EQ(sp->pages, 0);
    KT_EQ(vm_handle_fault(sp, BASE + PAGE + 0x234, ERR_W), VM_FAULT_HANDLED);
    KT_TRUE(present(sp, BASE + PAGE));
    KT_FALSE(present(sp, BASE));                        // only the touched page
    KT_EQ(sp->pages, 1);

    // The page is zero-filled and user+writable in the tables.
    const uint8_t* k = (const uint8_t*)vm_kaddr(sp, BASE + PAGE, 0);
    KT_NOTNULL(k);
    int nz = 0;
    for (uint32_t i = 0; k && i < PAGE; i++) nz |= k[i];
    KT_EQ(nz, 0);
    uint32_t e = *paging_pte(vm_space_dir(sp), BASE + PAGE, 0);
    KT_TRUE((e & (PAGE_USER | PAGE_RW)) == (PAGE_USER | PAGE_RW));

    // Outside every region, and one byte past the end: fatal.
    KT_EQ(vm_handle_fault(sp, BASE - 1, ERR_R), VM_FAULT_KILLED);
    KT_EQ(vm_handle_fault(sp, BASE + 3 * PAGE, ERR_R), VM_FAULT_KILLED);
    KT_EQ(vm_handle_fault(sp, 0, ERR_R), VM_FAULT_KILLED);              // null
    KT_EQ(vm_handle_fault(sp, 0x1000, ERR_R), VM_FAULT_KILLED);         // kernel range
    KT_EQ(vm_handle_fault(sp, 0xC0000000u, ERR_R), VM_FAULT_KILLED);

    vm_space_destroy(sp);
    KT_EQ(pmm_free_frames(), before);
}

KTEST(vm, protection_is_enforced){
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_anon(sp, BASE, PAGE, VM_PROT_R | VM_PROT_USER), 0);          // read-only
    KT_EQ(vm_map_anon(sp, BASE + 2 * PAGE, PAGE, VM_PROT_USER), 0);           // guard: no access
    KT_EQ(vm_map_anon(sp, BASE + 4 * PAGE, PAGE, VM_PROT_RW_USER), 0);
    KT_EQ(vm_map_anon(sp, BASE + 6 * PAGE, PAGE, VM_PROT_RW_USER & ~VM_PROT_USER), 0);  // kernel-only

    KT_EQ(vm_handle_fault(sp, BASE, ERR_R), VM_FAULT_HANDLED);
    KT_EQ(vm_handle_fault(sp, BASE, ERR_W), VM_FAULT_KILLED);
    // Once present, a write to it is a protection fault, still fatal.
    KT_EQ(vm_handle_fault(sp, BASE, ERR_W | VM_ERR_PRESENT), VM_FAULT_KILLED);
    KT_EQ(vm_handle_fault(sp, BASE + 2 * PAGE, ERR_R), VM_FAULT_KILLED);
    KT_EQ(vm_handle_fault(sp, BASE + 2 * PAGE, ERR_W), VM_FAULT_KILLED);
    KT_EQ(vm_handle_fault(sp, BASE + 4 * PAGE, ERR_W), VM_FAULT_HANDLED);
    // Ring 3 may not touch a region without the user bit.
    KT_EQ(vm_handle_fault(sp, BASE + 6 * PAGE, ERR_W), VM_FAULT_KILLED);
    // A present-bit fault on a page that is not there is bogus.
    KT_EQ(vm_handle_fault(sp, BASE + 5 * PAGE, ERR_R | VM_ERR_PRESENT), VM_FAULT_KILLED);

    vm_space_destroy(sp);
}

KTEST(vm, stack_grows_down_to_its_limit){
    uint32_t before = pmm_free_frames();
    uint32_t top = BASE + 0x800000u;
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_stack(sp, top, 4 * PAGE, 16 * PAGE), 0);
    KT_EQ(sp->stack_limit, top - 16 * PAGE);

    uint32_t lo = top - 4 * PAGE;
    KT_EQ(vm_handle_fault(sp, top - 8, ERR_W), VM_FAULT_HANDLED);        // inside
    KT_EQ(vm_handle_fault(sp, lo - 4, ERR_W), VM_FAULT_HANDLED);         // just below
    const vm_region_t* r = vm_find_region(sp, lo - 4);
    KT_NOTNULL(r);
    if (r){
        KT_EQ(r->start, lo - PAGE);
        KT_EQ(r->kind, VM_KIND_STACK);
    }
    KT_TRUE(present(sp, lo - PAGE));

    // A jump several pages down grows the region across the gap.
    KT_EQ(vm_handle_fault(sp, top - 12 * PAGE + 8, ERR_W), VM_FAULT_HANDLED);
    r = vm_find_region(sp, top - 9 * PAGE);
    KT_NOTNULL(r);                                   // the gap is covered too

    // The last allowed page works; the one below it is the guard.
    KT_EQ(vm_handle_fault(sp, top - 16 * PAGE, ERR_W), VM_FAULT_HANDLED);
    KT_EQ(vm_handle_fault(sp, top - 16 * PAGE - 4, ERR_W), VM_FAULT_KILLED);
    KT_EQ(vm_handle_fault(sp, top - 64 * PAGE, ERR_W), VM_FAULT_KILLED);

    // The heap is kept clear of the stack's reach.
    KT_TRUE(sp->heap_limit + VM_HEAP_GAP <= sp->stack_limit);

    vm_space_destroy(sp);
    KT_EQ(pmm_free_frames(), before);
}

KTEST(vm, stack_growth_stops_at_a_neighbour){
    uint32_t top = BASE + 0x800000u;
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_stack(sp, top, 4 * PAGE, 32 * PAGE), 0);
    // An unrelated mapping sits four pages below the stack.
    KT_EQ(vm_map_anon(sp, top - 12 * PAGE, 2 * PAGE, VM_PROT_RW_USER), 0);

    KT_EQ(vm_handle_fault(sp, top - 6 * PAGE, ERR_W), VM_FAULT_HANDLED);   // room to grow
    KT_EQ(vm_handle_fault(sp, top - 12 * PAGE - 4, ERR_W), VM_FAULT_KILLED); // beyond the neighbour
    KT_EQ(vm_handle_fault(sp, top - 13 * PAGE, ERR_W), VM_FAULT_KILLED);
    vm_space_destroy(sp);
}

KTEST(vm, cow_isolates_parent_and_child){
    uint32_t before = pmm_free_frames();
    vm_space_t* p = vm_space_create(BASE);
    KT_NOTNULL(p);
    if (!p) return;
    KT_EQ(vm_map_anon(p, BASE, 2 * PAGE, VM_PROT_RW_USER), 0);

    uint32_t a = 0x11111111u, b = 0x22222222u;
    KT_EQ(vm_copy_to(p, BASE, &a, 4), 0);
    KT_EQ(vm_copy_to(p, BASE + PAGE, &b, 4), 0);
    uint32_t pa = phys(p, BASE);

    vm_space_t* c = vm_fork_space(p);
    KT_NOTNULL(c);
    if (!c){ vm_space_destroy(p); return; }

    // Shared: one frame, two references, read-only and marked in both tables.
    KT_EQ(phys(c, BASE), pa);
    KT_EQ(pmm_frame_refs((void*)pa), 2);
    uint32_t pe = *paging_pte(vm_space_dir(p), BASE, 0);
    uint32_t ce = *paging_pte(vm_space_dir(c), BASE, 0);
    KT_TRUE((pe & PAGE_COW) && (ce & PAGE_COW));
    KT_FALSE((pe & PAGE_RW) || (ce & PAGE_RW));

    // The child writes: it gets its own frame and the parent keeps its data.
    uint32_t v = 0xC0FFEEu;
    KT_EQ(vm_copy_to(c, BASE, &v, 4), 0);
    KT_TRUE(phys(c, BASE) != pa);
    KT_EQ(phys(p, BASE), pa);
    KT_EQ(pmm_frame_refs((void*)pa), 1);
    uint32_t rp = 0, rc = 0;
    KT_EQ(vm_copy_from(p, BASE, &rp, 4), 0);
    KT_EQ(vm_copy_from(c, BASE, &rc, 4), 0);
    KT_EQ(rp, 0x11111111u);
    KT_EQ(rc, 0xC0FFEEu);

    // The parent is now the sole owner: writing takes the page back in place.
    v = 0xABCDu;
    KT_EQ(vm_copy_to(p, BASE, &v, 4), 0);
    KT_EQ(phys(p, BASE), pa);
    KT_TRUE(*paging_pte(vm_space_dir(p), BASE, 0) & PAGE_RW);
    KT_FALSE(*paging_pte(vm_space_dir(p), BASE, 0) & PAGE_COW);

    // The untouched second page is still shared, and still reads correctly.
    KT_EQ(phys(p, BASE + PAGE), phys(c, BASE + PAGE));
    KT_EQ(vm_copy_from(c, BASE + PAGE, &rc, 4), 0);
    KT_EQ(rc, 0x22222222u);

    // A COW fault on a page the region never allowed writing is still fatal.
    vm_space_destroy(c);
    KT_EQ(pmm_frame_refs((void*)phys(p, BASE + PAGE)), 1);
    vm_space_destroy(p);
    KT_EQ(pmm_free_frames(), before);
}

KTEST(vm, read_only_pages_are_shared_not_copied){
    vm_space_t* p = vm_space_create(BASE);
    KT_NOTNULL(p);
    if (!p) return;
    KT_EQ(vm_map_anon(p, BASE, PAGE, VM_PROT_R | VM_PROT_USER), 0);
    KT_EQ(vm_handle_fault(p, BASE, ERR_R), VM_FAULT_HANDLED);
    vm_space_t* c = vm_fork_space(p);
    KT_NOTNULL(c);
    if (c){
        KT_EQ(phys(c, BASE), phys(p, BASE));
        KT_FALSE(*paging_pte(vm_space_dir(c), BASE, 0) & PAGE_COW);
        KT_EQ(vm_handle_fault(c, BASE, ERR_W | VM_ERR_PRESENT), VM_FAULT_KILLED);
        vm_space_destroy(c);
    }
    vm_space_destroy(p);
}

// The real exception path. A space is loaded and its memory touched from the
// kernel with interrupts off (a scheduler switch would otherwise run other
// tasks on this CR3): each access below faults in hardware and must come back
// to the instruction that caused it.
KTEST(vm, hardware_faults_are_resolved_and_retried){
    uint32_t before = pmm_free_frames();
    vm_space_t* p = vm_space_create(BASE);
    KT_NOTNULL(p);
    if (!p) return;
    KT_EQ(vm_map_anon(p, BASE, 2 * PAGE, VM_PROT_RW_USER), 0);
    KT_EQ(vm_map_stack(p, BASE + 0x800000u, 2 * PAGE, 8 * PAGE), 0);

    volatile uint32_t* d = (volatile uint32_t*)(BASE + 8);
    volatile uint32_t* s = (volatile uint32_t*)(BASE + 0x800000u - 3 * PAGE);   // below the stack
    uint32_t got0, got1, got2, got3;

    uint32_t fl = irq_save();
    paging_switch(vm_space_dir(p));
    got0 = *d;                    // demand-zero read
    *d = 0x1234u;                 // now present, plain write
    *s = 0x77u;                   // stack growth
    got1 = *d;
    paging_switch(paging_kernel_dir());
    irq_restore(fl);

    KT_EQ(got0, 0);
    KT_EQ(got1, 0x1234u);
    KT_TRUE(present(p, BASE));
    KT_EQ(vm_find_region(p, (uint32_t)s)->start, BASE + 0x800000u - 3 * PAGE);

    vm_space_t* c = vm_fork_space(p);
    KT_NOTNULL(c);
    if (c){
        // Kernel write into a COW page of the child, and the parent's copy
        // must not change. Needs CR0.WP, or the write would go straight
        // through the read-only mapping into the shared frame.
        fl = irq_save();
        paging_switch(vm_space_dir(c));
        *d = 0x9999u;
        got2 = *d;
        paging_switch(vm_space_dir(p));
        got3 = *d;
        paging_switch(paging_kernel_dir());
        irq_restore(fl);
        KT_EQ(got2, 0x9999u);
        KT_EQ(got3, 0x1234u);
        vm_space_destroy(c);
    }
    vm_space_destroy(p);
    KT_EQ(pmm_free_frames(), before);
}

KTEST(vm, no_frames_leak_over_create_destroy_cycles){
    // Interrupts off: the count is compared across a stretch long enough for the
    // scheduler to run other tasks, and a process exiting elsewhere frees frames.
    uint32_t fl = irq_save();
    uint32_t before = pmm_free_frames();

    for (int round = 0; round < 8; round++){
        vm_space_t* p = vm_space_create(BASE + 0x2000);
        KT_NOTNULL(p);
        if (!p){ irq_restore(fl); return; }
        KT_EQ(vm_map_anon(p, BASE, 8 * PAGE, VM_PROT_RW_USER), 0);
        KT_EQ(vm_map_stack(p, BASE + 0x800000u, 4 * PAGE, 32 * PAGE), 0);
        for (uint32_t i = 0; i < 8; i++)
            KT_EQ(vm_handle_fault(p, BASE + i * PAGE, ERR_W), VM_FAULT_HANDLED);
        KT_EQ(vm_handle_fault(p, BASE + 0x800000u - 6 * PAGE, ERR_W), VM_FAULT_HANDLED);
        KT_EQ(vm_brk(p, p->heap_start + 5 * PAGE), p->heap_start + 5 * PAGE);
        KT_EQ(vm_handle_fault(p, p->heap_start + 4 * PAGE, ERR_W), VM_FAULT_HANDLED);
        uint32_t m = vm_mmap_anon(p, 3 * PAGE, VM_PROT_RW_USER);
        KT_TRUE(m != 0);
        KT_EQ(vm_handle_fault(p, m + PAGE, ERR_W), VM_FAULT_HANDLED);

        vm_space_t* c = vm_fork_space(p);
        KT_NOTNULL(c);
        if (c){
            uint32_t x = 5;
            KT_EQ(vm_copy_to(c, BASE, &x, 4), 0);
            KT_EQ(vm_copy_to(c, m + PAGE, &x, 4), 0);
            KT_EQ(vm_copy_to(p, BASE + 3 * PAGE, &x, 4), 0);
            vm_space_t* g = vm_fork_space(c);        // grandchild
            KT_NOTNULL(g);
            if (g) vm_space_destroy(g);
            // Destroy in the order that leaves the parent last, then the
            // reverse on odd rounds: the count must not depend on it.
            if (round & 1){ vm_space_destroy(p); vm_space_destroy(c); }
            else          { vm_space_destroy(c); vm_space_destroy(p); }
        } else {
            vm_space_destroy(p);
        }
    }
    uint32_t after = pmm_free_frames();
    irq_restore(fl);
    KT_EQ(after, before);
}

KTEST(vm, region_bounds_and_overlap){
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_anon(sp, BASE + 1, PAGE, VM_PROT_RW_USER), -1);         // unaligned
    KT_EQ(vm_map_anon(sp, BASE, 0, VM_PROT_RW_USER), -1);                // empty
    KT_EQ(vm_map_anon(sp, 0, PAGE, VM_PROT_RW_USER), -1);                // null page
    KT_EQ(vm_map_anon(sp, 0x100000u, PAGE, VM_PROT_RW_USER), -1);        // kernel's identity map
    KT_EQ(vm_map_anon(sp, VM_USER_TOP - PAGE, 2 * PAGE, VM_PROT_RW_USER), -1);   // past the top
    KT_EQ(vm_map_anon(sp, 0xFFFFF000u, 2 * PAGE, VM_PROT_RW_USER), -1);  // wraps
    KT_EQ(vm_map_anon(sp, BASE, PAGE, 0x40), -1);                        // junk prot bits

    KT_EQ(vm_map_anon(sp, BASE + 4 * PAGE, 4 * PAGE, VM_PROT_RW_USER), 0);
    KT_EQ(vm_map_anon(sp, BASE + 4 * PAGE, PAGE, VM_PROT_RW_USER), -1);            // same start
    KT_EQ(vm_map_anon(sp, BASE + 2 * PAGE, 3 * PAGE, VM_PROT_RW_USER), -1);        // overlaps the head
    KT_EQ(vm_map_anon(sp, BASE + 7 * PAGE, 2 * PAGE, VM_PROT_RW_USER), -1);        // overlaps the tail
    KT_EQ(vm_map_anon(sp, BASE, 12 * PAGE, VM_PROT_RW_USER), -1);                  // swallows it
    KT_EQ(vm_map_anon(sp, BASE, 4 * PAGE, VM_PROT_RW_USER), 0);                    // adjacent is fine
    KT_EQ(sp->nregions, 2);
    KT_TRUE(sp->regions[0].start < sp->regions[1].start);                          // stays sorted

    // Fill the table: the next mapping must fail cleanly.
    int n = sp->nregions;
    for (; n < VM_MAX_REGIONS; n++)
        KT_EQ(vm_map_anon(sp, BASE + (20 + 2 * (uint32_t)n) * PAGE, PAGE, VM_PROT_RW_USER), 0);
    KT_EQ(vm_map_anon(sp, BASE + 200 * PAGE, PAGE, VM_PROT_RW_USER), -1);
    vm_space_destroy(sp);
}

KTEST(vm, unmap_splits_and_releases){
    uint32_t before = pmm_free_frames();
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_anon(sp, BASE, 8 * PAGE, VM_PROT_RW_USER), 0);
    for (uint32_t i = 0; i < 8; i++) vm_handle_fault(sp, BASE + i * PAGE, ERR_W);
    KT_EQ(sp->pages, 8);

    KT_EQ(vm_unmap(sp, BASE + 3 * PAGE, 2 * PAGE), 0);       // punch a hole
    KT_EQ(sp->nregions, 2);
    KT_EQ(sp->pages, 6);
    KT_FALSE(present(sp, BASE + 3 * PAGE));
    KT_TRUE(present(sp, BASE + 2 * PAGE));
    KT_TRUE(present(sp, BASE + 5 * PAGE));
    KT_EQ(vm_handle_fault(sp, BASE + 3 * PAGE, ERR_R), VM_FAULT_KILLED);
    KT_EQ(vm_handle_fault(sp, BASE + 5 * PAGE, ERR_R), VM_FAULT_HANDLED);

    KT_EQ(vm_unmap(sp, BASE, 3 * PAGE), 0);                  // trim the head off the left piece
    KT_EQ(vm_unmap(sp, BASE + 5 * PAGE, 3 * PAGE), 0);       // remove the right piece whole
    KT_EQ(sp->nregions, 0);
    KT_EQ(sp->pages, 0);
    KT_EQ(vm_unmap(sp, BASE + 100 * PAGE, PAGE), 0);         // nothing there: not an error
    KT_EQ(vm_unmap(sp, BASE + 1, PAGE), -1);
    vm_space_destroy(sp);
    KT_EQ(pmm_free_frames(), before);
}

KTEST(vm, brk_grows_shrinks_and_has_hard_limits){
    uint32_t before = pmm_free_frames();
    vm_space_t* sp = vm_space_create(BASE + 0x3000);
    KT_NOTNULL(sp);
    if (!sp) return;
    uint32_t hs = BASE + 0x3000 + VM_HEAP_GAP;
    KT_EQ(sp->heap_start, hs);
    KT_EQ(vm_brk(sp, 0), hs);                                 // query
    KT_EQ(vm_handle_fault(sp, hs, ERR_W), VM_FAULT_KILLED);   // nothing yet
    KT_EQ(vm_handle_fault(sp, hs - PAGE, ERR_W), VM_FAULT_KILLED);   // the gap is a guard

    KT_EQ(vm_brk(sp, hs + 100), hs + 100);
    KT_EQ(vm_handle_fault(sp, hs + 50, ERR_W), VM_FAULT_HANDLED);
    KT_EQ(vm_handle_fault(sp, hs + PAGE, ERR_W), VM_FAULT_KILLED);   // page-granular end

    KT_EQ(vm_brk(sp, hs + 5 * PAGE), hs + 5 * PAGE);
    for (uint32_t i = 0; i < 5; i++) vm_handle_fault(sp, hs + i * PAGE, ERR_W);
    KT_EQ(sp->pages, 5);

    KT_EQ(vm_brk(sp, hs - 1), hs + 5 * PAGE);                 // below the start: refused
    KT_EQ(vm_brk(sp, sp->heap_limit + 1), hs + 5 * PAGE);     // above the limit: refused
    KT_EQ(vm_brk(sp, 0xFFFFFFFFu), hs + 5 * PAGE);

    KT_EQ(vm_brk(sp, hs + 2 * PAGE), hs + 2 * PAGE);          // shrink frees the tail
    KT_EQ(sp->pages, 2);
    KT_EQ(vm_handle_fault(sp, hs + 3 * PAGE, ERR_W), VM_FAULT_KILLED);

    KT_EQ(vm_unmap(sp, hs, PAGE), -1);                        // heap is brk's to manage

    KT_EQ(vm_brk(sp, sp->heap_limit), sp->heap_limit);        // the limit itself is allowed
    KT_EQ(vm_brk(sp, hs), hs);                                // back to empty
    KT_EQ(sp->nregions, 0);
    KT_EQ(sp->pages, 0);

    // Something else in the way stops the heap short.
    KT_EQ(vm_map_anon(sp, hs + 8 * PAGE, PAGE, VM_PROT_RW_USER), 0);
    KT_EQ(vm_brk(sp, hs + 4 * PAGE), hs + 4 * PAGE);
    KT_EQ(vm_brk(sp, hs + 9 * PAGE), hs + 4 * PAGE);
    vm_space_destroy(sp);
    KT_EQ(pmm_free_frames(), before);
}

KTEST(vm, mmap_finds_holes_with_guards){
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    uint32_t a = vm_mmap_anon(sp, 3 * PAGE, VM_PROT_RW_USER);
    uint32_t b = vm_mmap_anon(sp, PAGE + 1, VM_PROT_RW_USER);       // rounds up to 2 pages
    uint32_t c = vm_mmap_anon(sp, PAGE, VM_PROT_RW_USER);
    KT_TRUE(a >= VM_MMAP_BASE && a < VM_MMAP_LIMIT);
    KT_TRUE(b >= a + 3 * PAGE + PAGE);                              // guard page between
    KT_TRUE(c >= b + 2 * PAGE + PAGE);
    KT_EQ(a & 0xFFF, 0);
    KT_EQ(vm_handle_fault(sp, a + 3 * PAGE, ERR_R), VM_FAULT_KILLED);   // the guard
    KT_EQ(vm_handle_fault(sp, b + PAGE, ERR_W), VM_FAULT_HANDLED);      // rounded-up tail

    // Free the middle one; an equal-or-smaller request lands in the hole.
    KT_EQ(vm_unmap(sp, b, 2 * PAGE), 0);
    uint32_t d = vm_mmap_anon(sp, 2 * PAGE, VM_PROT_RW_USER);
    KT_EQ(d, b);
    KT_EQ(vm_mmap_anon(sp, 0, VM_PROT_RW_USER), 0);
    KT_EQ(vm_mmap_anon(sp, VM_MMAP_LIMIT, VM_PROT_RW_USER), 0);     // cannot fit
    vm_space_destroy(sp);
}

KTEST(vm, file_regions_are_zero_filled_stubs){
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_file(sp, BASE, 2 * PAGE, VM_PROT_R | VM_PROT_USER, 7, 4096), 0);
    const vm_region_t* r = vm_find_region(sp, BASE);
    KT_NOTNULL(r);
    if (r){ KT_EQ(r->kind, VM_KIND_FILE); KT_EQ(r->file_id, 7); KT_EQ(r->file_off, 4096); }
    KT_EQ(vm_handle_fault(sp, BASE + PAGE, ERR_R), VM_FAULT_HANDLED);
    KT_EQ(*(const uint32_t*)vm_kaddr(sp, BASE + PAGE, 0), 0);
    vm_space_destroy(sp);
}

KTEST(vm, kernel_copies_cross_pages_and_check_ranges){
    vm_space_t* sp = vm_space_create(BASE);
    KT_NOTNULL(sp);
    if (!sp) return;
    KT_EQ(vm_map_anon(sp, BASE, 2 * PAGE, VM_PROT_RW_USER), 0);
    KT_EQ(vm_map_anon(sp, BASE + 2 * PAGE, PAGE, VM_PROT_R | VM_PROT_USER), 0);

    uint8_t out[300], in[300];
    for (int i = 0; i < 300; i++) out[i] = (uint8_t)(i * 7 + 1);
    KT_EQ(vm_copy_to(sp, BASE + PAGE - 100, out, 300), 0);          // straddles a page boundary
    KT_EQ(vm_copy_from(sp, BASE + PAGE - 100, in, 300), 0);
    KT_MEMEQ(in, out, 300);
    KT_EQ(sp->pages, 2);

    KT_EQ(vm_copy_to(sp, BASE + 2 * PAGE, out, 4), -1);             // read-only
    KT_EQ(vm_copy_from(sp, BASE + 3 * PAGE, in, 4), -1);            // unmapped

    KT_TRUE(vm_range_ok(sp, BASE, 3 * PAGE, 0));                    // spans two regions
    KT_FALSE(vm_range_ok(sp, BASE, 3 * PAGE, 1));                   // last one is read-only
    KT_TRUE(vm_range_ok(sp, BASE, 2 * PAGE, 1));
    KT_FALSE(vm_range_ok(sp, BASE, 3 * PAGE + 1, 0));
    KT_FALSE(vm_range_ok(sp, 0xFFFFFFF0u, 0x100, 0));               // wraps
    vm_space_destroy(sp);
}

KTEST(pmm, refcounts_gate_the_release){
    uint32_t before = pmm_free_frames();
    void* f = pmm_alloc_frame();
    KT_NOTNULL(f);
    KT_EQ(pmm_frame_refs(f), 1);
    KT_EQ(pmm_ref_frame(f), 0);
    KT_EQ(pmm_frame_refs(f), 2);
    pmm_free_frame(f);                                  // one sharer left: still allocated
    KT_EQ(pmm_free_frames(), before - 1);
    KT_EQ(pmm_frame_refs(f), 1);
    pmm_free_frame(f);
    KT_EQ(pmm_free_frames(), before);
    KT_EQ(pmm_frame_refs(f), 0);
    KT_EQ(pmm_ref_frame(f), -1);                        // cannot share a free frame
}

KTEST(pmm, contiguous_runs){
    uint32_t before = pmm_free_frames();
    uint8_t* p = (uint8_t*)pmm_alloc_contig(40);
    KT_NOTNULL(p);
    if (!p) return;
    KT_EQ(pmm_free_frames(), before - 40);
    KT_EQ((uint32_t)p & 0xFFF, 0);
    for (uint32_t i = 0; i < 40; i++) KT_EQ(pmm_frame_refs(p + i * PAGE), 1);
    memset(p, 0x5A, 40 * PAGE);                         // identity-mapped: usable directly
    pmm_free_contig(p, 40);
    KT_EQ(pmm_free_frames(), before);
    KT_TRUE(pmm_alloc_contig(0) == 0);
    KT_TRUE(pmm_alloc_contig(0x7FFFFFFFu) == 0);
}

KTEST(kheap, large_requests_bypass_the_free_list){
    size_t u0 = 0, f0 = 0, u1 = 0, f1 = 0;
    kheap_stats(&u0, &f0);
    uint32_t fr0 = pmm_free_frames();
    uint32_t kb0 = pmm_used_kb();

    size_t sz = 300u * 1024u;                           // over the 256 KB threshold
    uint8_t* p = (uint8_t*)kmalloc(sz);
    KT_NOTNULL(p);
    if (!p) return;
    KT_EQ((uint32_t)p & 0xFFF, 0);                      // page-granular
    KT_EQ(pmm_free_frames(), fr0 - 75);
    KT_EQ(kheap_large_bytes(), 75u * PAGE);
    kheap_stats(&u1, &f1);
    KT_EQ(u1, u0);                                      // the array was not touched
    KT_EQ(f1, f0);
    KT_TRUE(pmm_used_kb() >= kb0 + 300 - 4);            // memory meters still see it
    memset(p, 0xA5, sz);

    // realloc keeps the contents; growing moves within the page path.
    uint8_t* q = (uint8_t*)krealloc(p, 600u * 1024u);
    KT_NOTNULL(q);
    if (!q){ kfree(p); return; }
    KT_EQ(q[0], 0xA5);
    KT_EQ(q[sz - 1], 0xA5);
    KT_EQ(kheap_large_bytes(), 150u * PAGE);
    KT_TRUE(krealloc(q, 1000) == q);                    // shrinking never moves
    kfree(q);
    KT_EQ(pmm_free_frames(), fr0);
    KT_EQ(kheap_large_bytes(), 0);
    KT_EQ(pmm_used_kb(), kb0);
    KT_EQ(kheap_check(), 0);

    // Just under the threshold still comes from the array.
    void* s = kmalloc(256u * 1024u - 16u);
    KT_NOTNULL(s);
    KT_EQ(pmm_free_frames(), fr0);
    kfree(s);
}

KTEST(kheap, page_api_and_repeated_large_cycles){
    uint32_t fr0 = pmm_free_frames();
    void* a = kmalloc_pages(5000);                      // two pages
    void* b = kmalloc_pages(PAGE);
    KT_NOTNULL(a); KT_NOTNULL(b);
    KT_EQ(pmm_free_frames(), fr0 - 3);
    KT_TRUE(a != b);
    kfree_pages(a);
    kfree(b);                                           // kfree accepts page allocations
    KT_EQ(pmm_free_frames(), fr0);
    KT_TRUE(kmalloc_pages(0) == 0);
    kfree_pages(0);

    // A video-ring-like pattern: allocate, free, repeat with growing sizes.
    // The array must not have been carved up by any of it.
    size_t u0, f0, u1, f1;
    kheap_stats(&u0, &f0);
    for (int i = 0; i < 20; i++){
        void* r = kmalloc((size_t)(300 + 40 * i) * 1024u);
        KT_NOTNULL(r);
        kfree(r);
    }
    kheap_stats(&u1, &f1);
    KT_EQ(u1, u0);
    KT_EQ(f1, f0);
    KT_EQ(pmm_free_frames(), fr0);
}

// The CPU sets the Accessed bit in the directory it walks, so a spaces

// The CPU sets the Accessed bit in the directory it walks, so a space's copy
// of a kernel entry and the kernel's own entry stop being bit-identical. The
// teardown used to compare whole entries and, seeing a difference, free the
// kernel page table as though the space had made it.
KTEST(paging, teardown_spares_kernel_tables_whose_accessed_bit_differs){
    uint32_t* kpd = (uint32_t*)paging_kernel_dir();
    uint32_t pd = paging_new_address_space();
    KT_TRUE(pd != 0);
    if (!pd) return;

    uint32_t saved = kpd[5];
    kpd[5] |= 0x20u;                                    // only the kernel dir has A set
    uint32_t table = kpd[5] & 0xFFFFF000u;
    KT_TRUE(pmm_frame_refs((void*)table) >= 1);
    uint32_t before = pmm_free_frames();

    paging_free_address_space(pd);
    KT_EQ(pmm_free_frames(), before + 1);               // just the directory itself
    KT_TRUE(pmm_frame_refs((void*)table) >= 1);         // the kernel table is still allocated
    kpd[5] = saved;
}
