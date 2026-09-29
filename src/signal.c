// src/signal.c — per-task pending/blocked/ignored signal masks and the default
// (terminate) action. See header/signal.h for who is expected to call what.
#include <stdint.h>
#include "header/signal.h"
#include "header/task.h"
#include "header/proc.h"
#include "header/kprintf.h"

#define SIG_SLOTS 32

typedef struct {
    int      used;
    uint32_t pid;
    uint32_t pending, blocked, ignored;
} sigstate_t;

static sigstate_t table[SIG_SLOTS];

// The signals that exist. Anything else is refused rather than silently
// stored, so a typo in a caller shows up as an error.
static int sig_supported(int sig){
    return sig == SIGINT || sig == SIGKILL || sig == SIGUSR1 || sig == SIGTERM;
}

static int task_alive(uint32_t pid){
    task_info_t info[TASK_MAX];
    int n = task_snapshot(info, TASK_MAX);
    for (int i = 0; i < n; i++)
        if (info[i].id == pid) return info[i].state != TASK_ZOMBIE;
    return 0;
}

static sigstate_t* find(uint32_t pid){
    for (int i = 0; i < SIG_SLOTS; i++)
        if (table[i].used && table[i].pid == pid) return &table[i];
    return 0;
}

static sigstate_t* get(uint32_t pid){
    sigstate_t* s = find(pid);
    if (s) return s;
    for (int pass = 0; pass < 2; pass++){
        for (int i = 0; i < SIG_SLOTS; i++){
            if (table[i].used) continue;
            table[i].used = 1; table[i].pid = pid;
            table[i].pending = table[i].blocked = table[i].ignored = 0;
            return &table[i];
        }
        // Full: reclaim entries for tasks that are gone (a killed task never
        // got to call signal_task_exit).
        for (int i = 0; i < SIG_SLOTS; i++)
            if (table[i].used && !task_alive(table[i].pid)) table[i].used = 0;
    }
    return 0;
}

static uint32_t deliverable(const sigstate_t* s){
    uint32_t mask = s->pending & ~s->ignored & ~s->blocked;
    return mask | (s->pending & SIGMASK(SIGKILL));
}

int signal_send(uint32_t pid, int sig){
    if (sig != 0 && !sig_supported(sig)) return SIGNAL_EINVAL;
    if (!task_alive(pid)) return SIGNAL_ESRCH;
    if (sig == 0) return 0;

    sigstate_t* s = get(pid);
    if (!s) return SIGNAL_ESRCH;
    if (sig != SIGKILL && (s->ignored & SIGMASK(sig))) return 0;   // discarded
    s->pending |= SIGMASK(sig);
    return 0;
}

uint32_t signal_pending(uint32_t pid){
    sigstate_t* s = find(pid);
    return s ? s->pending : 0;
}

int signal_pending_current(void){
    sigstate_t* s = find(task_current_id());
    return s && deliverable(s) != 0;
}

int signal_dequeue_current(void){
    sigstate_t* s = find(task_current_id());
    if (!s) return 0;
    uint32_t mask = deliverable(s);
    if (!mask) return 0;

    int sig = 0;
    if (mask & SIGMASK(SIGKILL)) sig = SIGKILL;
    else for (int i = 1; i < NSIG; i++) if (mask & SIGMASK(i)){ sig = i; break; }
    s->pending &= ~SIGMASK(sig);
    return sig;
}

void signal_check_current(void){
    int sig = signal_dequeue_current();
    if (!sig) return;

    uint32_t pid = task_current_id();
    kprintf("[signal] pid %u terminated by signal %d\n", pid, sig);
    signal_task_exit(pid);

    // A process gets the shell convention for "killed by signal N"; a bare
    // kernel thread just ends. Neither call returns. (The signal table is
    // keyed by task id, but a process id is a different number, so ask proc.c
    // which process this task is hosting instead of comparing them.)
    if (proc_current()) proc_exit(128 + sig);
    task_exit();
}

int signal_ignore(int sig, int on){
    if (!sig_supported(sig) || sig == SIGKILL) return SIGNAL_EINVAL;
    sigstate_t* s = get(task_current_id());
    if (!s) return SIGNAL_EINVAL;
    if (on){ s->ignored |= SIGMASK(sig); s->pending &= ~SIGMASK(sig); }
    else s->ignored &= ~SIGMASK(sig);
    return 0;
}

uint32_t signal_sigmask(int how, uint32_t mask){
    sigstate_t* s = get(task_current_id());
    if (!s) return 0;
    uint32_t old = s->blocked;
    mask &= ~SIGMASK(SIGKILL);
    if (how == SIG_BLOCK) s->blocked |= mask;
    else if (how == SIG_UNBLOCK) s->blocked &= ~mask;
    else if (how == SIG_SETMASK) s->blocked = mask;
    return old;
}

void signal_task_exit(uint32_t pid){
    sigstate_t* s = find(pid);
    if (s) s->used = 0;
}
