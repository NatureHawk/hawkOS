// header/sync.h — wait queues, mutexes and semaphores
//
// A task that has nothing to do used to task_sleep(10) and look again. These
// let it go to TASK_BLOCKED instead and be woken by whoever changes the thing
// it is waiting for, so an idle system really is idle and a wake costs the
// time to the next scheduling point rather than up to 10 ms.
//
// Lost wakeups. A waiter normally checks a condition and then waits; a wake
// landing between the two would be missed. Two ways to close that window:
//   * take waitq_seq() BEFORE checking the condition and hand it to
//     waitq_wait_seq(), which returns at once if any wake has happened since;
//   * use waitq_wait_until(), which evaluates the condition with interrupts
//     off and blocks in the same critical section.
// Plain waitq_wait() is only for callers that do not care (a timed nap that
// an event may cut short).
//
// Context rules: waitq_wake_*, sem_post and sem_trywait may be called from an
// interrupt handler. Everything that can block must run in a task, and never
// with interrupts off or from an IRQ handler.
#pragma once
#include <stdint.h>

#define WAIT_FOREVER 0xFFFFFFFFu

typedef struct {
    volatile uint32_t seq;      // bumped by every wake, so a late waiter can tell
} waitq_t;

#define WAITQ_INIT { 0 }

void     waitq_init(waitq_t* q);
uint32_t waitq_seq(const waitq_t* q);

// Blocks until woken or timeout_ms passes (0 = do not block, WAIT_FOREVER =
// no timeout; granularity is the 10 ms tick). Returns 0 if woken, 1 on timeout.
int  waitq_wait(waitq_t* q, uint32_t timeout_ms);
// Same, but returns 0 immediately if a wake has happened since `seq` was read.
int  waitq_wait_seq(waitq_t* q, uint32_t seq, uint32_t timeout_ms);
// Blocks until cond(arg) is non-zero, evaluated with interrupts off (keep it
// short and free of side effects). Returns 0 once true, 1 on timeout.
int  waitq_wait_until(waitq_t* q, int (*cond)(void*), void* arg, uint32_t timeout_ms);

int  waitq_wake_one(waitq_t* q);        // oldest waiter; returns how many woke
int  waitq_wake_all(waitq_t* q);

// Sleeping, non-recursive mutex. Unlocking from a task that does not own it
// is refused (returns -1).
typedef struct {
    volatile int held;
    uint32_t     owner;         // task id, valid while held
    waitq_t      wq;
} mutex_t;

#define MUTEX_INIT { 0, 0, WAITQ_INIT }

void mutex_init(mutex_t* m);
void mutex_lock(mutex_t* m);
int  mutex_trylock(mutex_t* m);         // 0 if taken, -1 if busy
int  mutex_unlock(mutex_t* m);
int  mutex_owned(const mutex_t* m);     // held by the calling task

// Counting semaphore.
typedef struct {
    volatile int count;
    waitq_t      wq;
} sem_t;

void sem_init(sem_t* s, int initial);
void sem_wait(sem_t* s);
int  sem_timedwait(sem_t* s, uint32_t timeout_ms);   // 0 taken, 1 timed out
int  sem_trywait(sem_t* s);                          // 0 taken, -1 empty
void sem_post(sem_t* s);                             // interrupt-safe
