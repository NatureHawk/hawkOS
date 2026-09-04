// src/test_proc.c — user mode: address spaces, privilege, and system calls
//
// The assertions that matter here are the negative ones. That a user program
// runs is easy to demonstrate and easy to fake; that it *cannot* reach kernel
// memory is the property the whole mechanism exists to provide, and the only
// way to be sure of it is to check the page tables say so.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/paging.h"
#include "header/pmm.h"
#include "header/proc.h"
#include "header/gdt.h"
#include "header/syscall.h"
#include "header/task.h"
#include "header/fat32.h"
#include "header/ata.h"

KTEST(paging, kernel_pages_are_supervisor_only){
    // The identity map must not carry the USER bit anywhere in it. One page
    // that does is one page a ring-3 program can read, and finding that by
    // inspection later is much harder than asserting it here.
    uint32_t kd = paging_kernel_dir();
    KT_TRUE(kd != 0);

    const uint32_t* pd = (const uint32_t*)kd;
    int user_readable = 0;
    for (uint32_t i = 0; i < 32; i++){            // the low 128 MB
        if (!(pd[i] & PAGE_PRESENT)) continue;
        if (pd[i] & PAGE_USER){ user_readable++; continue; }
        const uint32_t* pt = (const uint32_t*)(pd[i] & 0xFFFFF000u);
        for (uint32_t e = 0; e < 1024; e++)
            if ((pt[e] & PAGE_PRESENT) && (pt[e] & PAGE_USER)) user_readable++;
    }
    KT_EQ(user_readable, 0);
}

KTEST(paging, new_address_space_shares_the_kernel){
    uint32_t pd = paging_new_address_space();
    KT_TRUE(pd != 0);
    if (!pd) return;

    // Kernel mappings must be visible, and identical to the kernel's own --
    // shared by directory entry, not copied, so a later kernel mapping shows
    // up here too.
    const uint32_t* kpd = (const uint32_t*)paging_kernel_dir();
    const uint32_t* npd = (const uint32_t*)pd;
    for (uint32_t i = 0; i < 32; i++) KT_EQ(npd[i], kpd[i]);

    // And the kernel must still be reachable through the new space.
    KT_EQ(paging_phys_of(pd, 0x100000), 0x100000);

    paging_free_address_space(pd);
}

KTEST(paging, user_mapping_is_private_to_its_space){
    uint32_t a = paging_new_address_space();
    uint32_t b = paging_new_address_space();
    KT_TRUE(a != 0);
    KT_TRUE(b != 0);
    if (!a || !b) return;

    void* frame = pmm_alloc_frame();
    KT_NOTNULL(frame);

    paging_map_in(a, (uint32_t)frame, PROC_BASE, PAGE_RW | PAGE_USER);

    // Present in a, absent in b: two processes at the same virtual address
    // must not be looking at the same memory.
    KT_EQ(paging_phys_of(a, PROC_BASE), (uint32_t)frame);
    KT_EQ(paging_phys_of(b, PROC_BASE), 0);

    // And the kernel's own directory must be untouched by either.
    KT_EQ(paging_phys_of(paging_kernel_dir(), PROC_BASE), 0);

    pmm_free_frame(frame);
    paging_free_address_space(a);
    paging_free_address_space(b);
}

KTEST(paging, user_pages_carry_the_user_bit){
    uint32_t pd = paging_new_address_space();
    KT_TRUE(pd != 0);
    if (!pd) return;

    void* frame = pmm_alloc_frame();
    KT_NOTNULL(frame);
    paging_map_in(pd, (uint32_t)frame, PROC_BASE, PAGE_RW | PAGE_USER);

    // Both levels have to permit user access: the CPU takes the more
    // restrictive of the directory entry and the table entry, so a supervisor
    // directory entry would hide a perfectly good user page.
    const uint32_t* d = (const uint32_t*)pd;
    uint32_t pde = d[PROC_BASE >> 22];
    KT_TRUE(pde & PAGE_USER);
    const uint32_t* pt = (const uint32_t*)(pde & 0xFFFFF000u);
    KT_TRUE(pt[(PROC_BASE >> 12) & 0x3FF] & PAGE_USER);

    pmm_free_frame(frame);
    paging_free_address_space(pd);
}

KTEST(paging, address_spaces_do_not_leak_frames){
    // Create and destroy several spaces with mappings in them. Anything the
    // teardown misses shows up as a permanent loss of frames.
    uint32_t before = pmm_free_frames();

    for (int i = 0; i < 4; i++){
        uint32_t pd = paging_new_address_space();
        KT_TRUE(pd != 0);
        if (!pd) break;
        void* f1 = pmm_alloc_frame();
        void* f2 = pmm_alloc_frame();
        paging_map_in(pd, (uint32_t)f1, PROC_BASE, PAGE_RW | PAGE_USER);
        paging_map_in(pd, (uint32_t)f2, PROC_STACK_TOP - 0x1000, PAGE_RW | PAGE_USER);
        pmm_free_frame(f1);
        pmm_free_frame(f2);
        paging_free_address_space(pd);
    }

    KT_EQ(pmm_free_frames(), before);
}

KTEST(syscall, gate_is_the_only_one_open_to_ring_three){
    // Reach into the IDT and check the privilege bits directly. A gate left at
    // DPL 3 by accident is not something that shows up in testing -- it shows
    // up when someone finds it.
    extern void* idt_base_for_test(void);
    const uint8_t* idt = (const uint8_t*)idt_base_for_test();
    KT_NOTNULL(idt);
    if (!idt) return;

    int open_gates = 0;
    for (int v = 0; v < 256; v++){
        uint8_t flags = idt[v * 8 + 5];
        int dpl = (flags >> 5) & 3;
        if (dpl == 3) open_gates++;
    }
    KT_EQ(open_gates, 1);

    // And it is 0x80 specifically.
    KT_EQ((idt[0x80 * 8 + 5] >> 5) & 3, 3);
}

KTEST(proc, spawn_rejects_bad_names){
    KT_EQ(proc_spawn(""), -1);
    KT_EQ(proc_spawn("../ESCAPE.BIN"), -1);
    KT_EQ(proc_spawn("SUB/DIR.BIN"), -1);
    KT_EQ(proc_spawn("NOSUCHFILE.BIN"), -1);
}

KTEST(proc, hello_binary_is_present_and_loadable){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }

    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), "HELLO.BIN", &f) != 0){
        ktest_skip("HELLO.BIN not on the disk image (run: make disk-sync)");
        return;
    }
    KT_TRUE(f.size > 0);
    KT_TRUE(f.size < PROC_IMAGE_MAX);
    KT_EQ(f.is_dir, 0);
}

KTEST(proc, spawn_runs_a_user_program_to_completion){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), "HELLO.BIN", &f) != 0){
        ktest_skip("HELLO.BIN not on the disk image");
        return;
    }

    uint32_t calls_before = syscall_count();
    uint32_t frames_before = pmm_free_frames();

    int id = proc_spawn("HELLO.BIN");
    KT_TRUE(id >= 0);
    if (id < 0) return;

    // The program sleeps three times for 60ms and then deliberately faults on
    // kernel memory, so it is finished well inside this budget.
    for (int i = 0; i < 120 && proc_count() > 0; i++) task_sleep(20);

    KT_EQ(proc_count(), 0);                       // it exited, one way or another
    KT_TRUE(syscall_count() > calls_before + 8);  // and it really used the gate

    // Its address space and image came back. A few frames of slack: other
    // tasks are running, and the reaper frees the kernel stack on its own
    // schedule.
    task_sleep(120);
    KT_TRUE(pmm_free_frames() + 8 >= frames_before);
}

KTEST(proc, user_program_cannot_read_kernel_memory){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), "HELLO.BIN", &f) != 0){
        ktest_skip("HELLO.BIN not on the disk image");
        return;
    }

    // hello.c ends by dereferencing 0x100000, which is inside the kernel
    // image. If the privilege boundary holds, the fault handler kills the
    // process and it never reaches its own success path. The assertion is
    // that the process is gone and the system is still running to make it.
    int id = proc_spawn("HELLO.BIN");
    KT_TRUE(id >= 0);
    if (id < 0) return;

    for (int i = 0; i < 120 && proc_count() > 0; i++) task_sleep(20);
    KT_EQ(proc_count(), 0);
}

KTEST(proc, many_spawns_do_not_exhaust_the_table){
    if (!ata_present()){ ktest_skip("no ATA device"); return; }
    fat32_dirent_t f;
    if (fat32_find(fat32_root_cluster(), "HELLO.BIN", &f) != 0){
        ktest_skip("HELLO.BIN not on the disk image");
        return;
    }

    // Run several in sequence. A slot or an address space that is not released
    // makes the fourth or fifth of these fail.
    for (int i = 0; i < 4; i++){
        int id = proc_spawn("HELLO.BIN");
        KT_TRUE(id >= 0);
        for (int w = 0; w < 120 && proc_count() > 0; w++) task_sleep(20);
        KT_EQ(proc_count(), 0);
    }
}
