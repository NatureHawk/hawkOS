// src/test_mem.c — the physical frame allocator and the kernel heap
//
// The memory reporting tests here exist because of a real bug: the taskbar
// meter read pmm_free_frames() and therefore sat at 16% from boot to
// shutdown, because the heap is a static array the frame allocator reserves
// once and never sees again. pmm_used_kb() is the fix, and
// used_kb_tracks_kmalloc is the assertion that would have caught it.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/pmm.h"
#include "header/kheap.h"

KTEST(pmm, reports_a_plausible_machine){
    KT_TRUE(pmm_total_kb() > 16u * 1024u);      // more than 16 MB
    KT_TRUE(pmm_total_kb() <= 256u * 1024u);    // within the tracking cap
    KT_TRUE(pmm_used_kb() > 0);
    KT_TRUE(pmm_used_kb() < pmm_total_kb());
}

KTEST(pmm, frame_alloc_is_unique_and_aligned){
    #define N 32
    void* f[N];
    for (int i = 0; i < N; i++){
        f[i] = pmm_alloc_frame();
        KT_NOTNULL(f[i]);
        KT_EQ(((uint32_t)f[i]) & (PMM_FRAME_SIZE - 1), 0);
    }
    // No frame handed out twice.
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++)
            KT_TRUE(f[i] != f[j]);

    for (int i = 0; i < N; i++) pmm_free_frame(f[i]);
    #undef N
}

KTEST(pmm, freeing_returns_frames_to_the_pool){
    uint32_t before = pmm_free_frames();
    void* p = pmm_alloc_frame();
    KT_NOTNULL(p);
    KT_EQ(pmm_free_frames(), before - 1);
    pmm_free_frame(p);
    KT_EQ(pmm_free_frames(), before);
}

KTEST(pmm, used_kb_tracks_kmalloc){
    // The regression test for the frozen taskbar meter. A megabyte off the
    // heap has to move the system figure, even though it moves no frames.
    uint32_t before = pmm_used_kb();
    void* big = kmalloc(1024u * 1024u);
    KT_NOTNULL(big);
    uint32_t during = pmm_used_kb();
    kfree(big);
    uint32_t after = pmm_used_kb();

    KT_TRUE(during >= before + 900);        // ~1 MB, allowing for rounding
    KT_TRUE(after <= before + 8);           // and released again
}

KTEST(kheap, alloc_is_aligned_and_writable){
    for (int i = 1; i <= 64; i++){
        void* p = kmalloc((size_t)i * 7);
        KT_NOTNULL(p);
        KT_EQ(((uint32_t)p) & 15u, 0);      // 16-byte aligned
        memset(p, 0x5A, (size_t)i * 7);     // faults or corrupts if the size is a lie
        kfree(p);
    }
}

KTEST(kheap, allocations_do_not_overlap){
    #define M 24
    uint8_t* p[M];
    for (int i = 0; i < M; i++){
        p[i] = (uint8_t*)kmalloc(64);
        KT_NOTNULL(p[i]);
        memset(p[i], i + 1, 64);            // stamp each block with its own value
    }
    // If two blocks overlapped, an earlier stamp would have been overwritten.
    for (int i = 0; i < M; i++)
        for (int k = 0; k < 64; k++)
            KT_EQ(p[i][k], i + 1);

    for (int i = 0; i < M; i++) kfree(p[i]);
    #undef M
}

KTEST(kheap, stats_move_with_use){
    size_t u0 = 0, f0 = 0, u1 = 0, f1 = 0;
    kheap_stats(&u0, &f0);
    void* p = kmalloc(4096);
    KT_NOTNULL(p);
    kheap_stats(&u1, &f1);
    KT_TRUE(u1 >= u0 + 4096);
    KT_TRUE(f1 <= f0 - 4096 + 64);          // minus the block header
    kfree(p);
}

KTEST(kheap, free_coalesces){
    // Three adjacent blocks freed in order must merge back into something
    // large enough to satisfy their combined size, or the heap fragments
    // itself to death over a browsing session.
    size_t before = 0, dummy = 0;
    kheap_stats(&dummy, &before);

    void* a = kmalloc(8192);
    void* b = kmalloc(8192);
    void* c = kmalloc(8192);
    KT_NOTNULL(a); KT_NOTNULL(b); KT_NOTNULL(c);
    kfree(a); kfree(b); kfree(c);

    void* big = kmalloc(24000);
    KT_NOTNULL(big);
    kfree(big);

    size_t after = 0;
    kheap_stats(&dummy, &after);
    KT_TRUE(after >= before - 128);          // no net loss beyond header churn
}

KTEST(kheap, zero_and_null_are_safe){
    KT_TRUE(kmalloc(0) == 0);
    kfree(0);                                // must not fault
}
