#pragma once
#include <stdint.h>

// Minimal signals.
//
// Each task (pid == task id) has a pending mask, a blocked mask and an ignored
// mask, kept in a small table in signal.c and created on first use. There are
// no user-space handlers: every supported signal's disposition is either its
// default (terminate the process) or ignored. That is enough for kill(),
// Ctrl-C and a well-behaved SIGTERM, which is what the shell needs.
//
// Where the rest of the kernel must call in (the syscall/proc code):
//   * SYS_KILL handler          -> signal_send(pid, sig)
//   * before returning to ring 3 from every system call (and after any
//     blocking syscall wakes) -> signal_check_current()   [does not return if
//                                a fatal signal was pending]
//   * a console Ctrl-C          -> signal_send(foreground_pid, SIGINT)
//   * proc_exit()/task reaping  -> signal_task_exit(pid), to free the state
// Blocking kernel loops (pipe_wait) poll signal_pending_current() and bail
// out with VFS_EINTR so a signalled process is not stuck asleep.

#define SIGINT   2
#define SIGKILL  9
#define SIGUSR1  10
#define SIGTERM  15
#define NSIG     32

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGNAL_ESRCH  (-3)
#define SIGNAL_EINVAL (-22)

#define SIGMASK(s) (1u << (s))

// Posts `sig` to task `pid`. sig 0 only tests that the task exists. A signal
// that is ignored by the target is discarded. Returns 0, SIGNAL_ESRCH (no
// such live task) or SIGNAL_EINVAL (unsupported signal number).
int      signal_send(uint32_t pid, int sig);

// Pending (posted, not yet consumed) signals of a task, as a bit mask.
uint32_t signal_pending(uint32_t pid);

// True if the calling task has a deliverable signal (pending, not blocked,
// not ignored; SIGKILL always deliverable). Cheap; safe from blocking loops.
int      signal_pending_current(void);

// Consumes and returns the highest-priority deliverable signal of the
// calling task (SIGKILL first, then lowest number), or 0. Does not act on it.
int      signal_dequeue_current(void);

// signal_dequeue_current() plus the default action: a process is torn down
// through proc_exit(128 + sig), a bare kernel task through task_exit().
// Returns only when nothing fatal was pending.
void     signal_check_current(void);

// Per-calling-task disposition. SIGKILL can be neither ignored nor blocked.
int      signal_ignore(int sig, int on);                 // 0 or SIGNAL_EINVAL
uint32_t signal_sigmask(int how, uint32_t mask);         // returns the previous blocked mask

// Forgets a task's signal state (call when it exits).
void     signal_task_exit(uint32_t pid);
