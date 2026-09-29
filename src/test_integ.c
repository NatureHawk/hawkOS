// src/test_integ.c — the pieces working together
//
// Each subsystem has its own tests; these are the ones that only make sense
// with the process layer on top of the VM, the VFS, pipes and signals at
// once: real programs spawned from disk, killed while blocked, waited for
// without polling, and torn down without leaving frames behind.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/pmm.h"
#include "header/proc.h"
#include "header/syscall.h"
#include "header/task.h"
#include "header/sync.h"
#include "header/signal.h"
#include "header/pipe.h"
#include "header/vfs.h"
#include "header/fat32.h"
#include "header/ata.h"

extern volatile unsigned long long ticks;

static int have(const char* name){
    fat32_dirent_t f;
    return ata_present() && fat32_find(fat32_root_cluster(), name, &f) == 0;
}

KTEST(integ, utest_elf_exits_zero){
    if (!have("UTEST.ELF")){ ktest_skip("UTEST.ELF not on the disk image (run: make disk-sync)"); return; }
    int pid = proc_spawn("UTEST.ELF");
    KT_TRUE(pid > 0);
    if (pid <= 0) return;
    int code = -99;
    KT_EQ(proc_wait(pid, &code), pid);
    KT_EQ(code, 0);                              // the number of failed checks
    fat32_delete(fat32_root_cluster(), "UTEST.TMP");
}

KTEST(integ, spawn_finds_programs_through_the_vfs){
    if (!have("HI.ELF")){ ktest_skip("HI.ELF not on the disk image"); return; }
    static const char* const names[] = { "HI.ELF", "hi.elf", "/HI.ELF", "./hi.elf", "/dev/../Hi.Elf" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++){
        int pid = proc_spawn(names[i]);
        KT_TRUE(pid > 0);
        if (pid <= 0) continue;
        int code = -99;
        KT_EQ(proc_wait(pid, &code), pid);
        KT_EQ(code, 0);
    }
    KT_EQ(proc_spawn("/dev/console"), -1);       // a device is not a program
    KT_EQ(proc_spawn("/"), -1);
}

// A test process whose descriptor 0 is the read end of a pipe, and the pipe's
// write end; spawns `prog` as its child with those descriptors inherited.
typedef struct {
    proc_t* parent;
    int     wfd;
    int     child;
} rig_t;

static int rig_start(rig_t* r, const char* prog){
    r->parent = proc_test_create();
    if (!r->parent) return -1;
    proc_test_bind(r->parent);
    int fds[2];
    if (pipe_open_fds(&r->parent->fdt, fds) < 0) return -1;
    r->wfd = fds[1];
    vfs_dup2(&r->parent->fdt, fds[0], 0);
    vfs_close(&r->parent->fdt, fds[0]);
    r->child = proc_spawn_ex(prog, 0, 0, PROC_SPAWN_INHERIT);
    return r->child > 0 ? 0 : -1;
}

static void rig_end(rig_t* r){
    if (r->parent) proc_test_destroy(r->parent);
    r->parent = 0;
}

KTEST(integ, spawn_and_kill_a_blocked_child){
    if (!have("WC.ELF")){ ktest_skip("WC.ELF not on the disk image"); return; }
    rig_t r;
    KT_EQ(rig_start(&r, "WC.ELF"), 0);
    if (r.child > 0){
        task_sleep(100);                         // the child is asleep in read()
        KT_EQ(signal_send(proc_tid_of((uint32_t)r.child), SIGTERM), 0);
        int code = -99;
        KT_EQ(proc_wait(r.child, &code), r.child);
        KT_EQ(code, 128 + SIGTERM);
    }
    rig_end(&r);
    KT_EQ(proc_count(), 0);
}

KTEST(integ, pipe_eof_lets_the_child_finish){
    if (!have("WC.ELF")){ ktest_skip("WC.ELF not on the disk image"); return; }
    rig_t r;
    KT_EQ(rig_start(&r, "WC.ELF"), 0);
    if (r.child > 0){
        task_sleep(60);
        KT_EQ(vfs_write(&r.parent->fdt, r.wfd, "a b c\n", 6), 6);
        KT_EQ(vfs_close(&r.parent->fdt, r.wfd), 0);   // the only write end: EOF
        int code = -99;
        KT_EQ(proc_wait(r.child, &code), r.child);
        KT_EQ(code, 0);
    }
    rig_end(&r);
}

// Kills `arg`'s process after a delay, from a task of its own.
typedef struct { int pid; uint32_t delay; volatile unsigned long long sent; } killer_t;

static void killer_task(void* arg){
    killer_t* k = (killer_t*)arg;
    task_sleep(k->delay);
    k->sent = ticks;
    signal_send(proc_tid_of((uint32_t)k->pid), SIGKILL);
}

static uint32_t my_slices(void){
    task_info_t info[TASK_MAX];
    int n = task_snapshot(info, TASK_MAX);
    for (int i = 0; i < n; i++) if (info[i].is_current) return info[i].slices;
    return 0;
}

KTEST(integ, waitpid_sleeps_and_wakes_promptly){
    if (!have("WC.ELF")){ ktest_skip("WC.ELF not on the disk image"); return; }
    rig_t r;
    KT_EQ(rig_start(&r, "WC.ELF"), 0);
    if (r.child > 0){
        static killer_t k;
        k.pid = r.child; k.delay = 400; k.sent = 0;
        KT_TRUE(task_create("killer", killer_task, &k) >= 0);

        uint32_t s0 = my_slices();
        int code = -99;
        KT_EQ(proc_wait(r.child, &code), r.child);
        unsigned long long done = ticks;
        uint32_t used = my_slices() - s0;

        KT_EQ(code, 128 + SIGKILL);
        KT_TRUE(k.sent != 0);
        // From the kill to the waiter running again: the child notices within
        // one pipe timeout (5 ticks), the waiter is woken by its exit. A lost
        // wake would show up as the wait's own 100 ms timeout on top.
        KT_TRUE(done - k.sent < 12);
        // 400 ms asleep on a wait queue is a handful of slices; polling every
        // 10 ms would be forty.
        KT_TRUE(used < 15);
    }
    rig_end(&r);
}

static void signaller_task(void* arg){
    uint32_t victim = (uint32_t)arg;
    task_sleep(80);
    signal_send(victim, SIGUSR1);
}

KTEST(integ, blocked_pipe_read_returns_eintr){
    vfs_file_t *rd, *wr;
    KT_EQ(pipe_create(&rd, &wr), 0);
    KT_TRUE(task_create("signaller", signaller_task, (void*)task_current_id()) >= 0);

    char c;
    unsigned long long t0 = ticks;
    KT_EQ(vfs_file_read(rd, &c, 1), VFS_EINTR);
    KT_TRUE(ticks - t0 < 30);
    KT_EQ(signal_dequeue_current(), SIGUSR1);    // consume it, or it would end this test task

    vfs_file_put(rd);
    vfs_file_put(wr);
}

KTEST(integ, spawn_exit_cycles_do_not_leak_frames){
    if (!have("HI.ELF")){ ktest_skip("HI.ELF not on the disk image"); return; }
    // One round first, so lazily grown kernel state is not mistaken for a leak.
    int pid = proc_spawn("HI.ELF");
    KT_TRUE(pid > 0);
    int code;
    if (pid > 0) proc_wait(pid, &code);
    task_sleep(150);

    uint32_t before = pmm_free_frames();
    for (int i = 0; i < 12; i++){
        pid = proc_spawn_args("HI.ELF", "a b c");
        KT_TRUE(pid > 0);
        if (pid <= 0) break;
        KT_EQ(proc_wait(pid, &code), pid);
        KT_EQ(code, 0);
    }
    KT_EQ(proc_count(), 0);
    task_sleep(150);                             // the reaper frees kernel stacks on its own schedule
    uint32_t after = pmm_free_frames();
    // A process costs a directory, page tables, and image/stack pages: a leak
    // of even one frame per cycle would be twelve.
    KT_TRUE(after + 3 >= before);
}

KTEST(integ, fsdemo_program_passes){
    if (!have("FSDEMO.ELF")){ ktest_skip("FSDEMO.ELF not on the disk image"); return; }
    if (!fat32_writable()){ ktest_skip("no writable disk"); return; }
    int pid = proc_spawn("FSDEMO.ELF");
    KT_TRUE(pid > 0);
    if (pid <= 0) return;
    int code = -99;
    KT_EQ(proc_wait(pid, &code), pid);
    KT_EQ(code, 0);
}
