// src/sync.c — wait queues, mutexes and semaphores
//
// Everything is built on two scheduler primitives: task_block_locked parks the
// caller on an address, task_wake_obj readies whoever is parked there. The
// decision to sleep is always made with interrupts off, in the same critical
// section as the check that justified it, so a wake from another task or from
// an interrupt handler cannot fall into the gap.
#include <stdint.h>
#include "header/sync.h"
#include "header/task.h"
#include "header/irqctl.h"

extern volatile unsigned long long ticks;

static uint32_t to_ticks(uint32_t ms){
    if (ms == WAIT_FOREVER) return 0;               // 0 = no timeout to the scheduler
    uint32_t t = (ms + 9u) / 10u;                   // PIT runs at 100 Hz
    return t ? t : 1;
}

void     waitq_init(waitq_t* q){ q->seq = 0; }
uint32_t waitq_seq(const waitq_t* q){ return q->seq; }

int waitq_wake_one(waitq_t* q){
    uint32_t f = irq_save();
    q->seq++;
    int n = task_wake_obj(q, 0);
    irq_restore(f);
    return n;
}

int waitq_wake_all(waitq_t* q){
    uint32_t f = irq_save();
    q->seq++;
    int n = task_wake_obj(q, 1);
    irq_restore(f);
    return n;
}

int waitq_wait_seq(waitq_t* q, uint32_t seq, uint32_t timeout_ms){
    if (timeout_ms == 0) return 1;
    uint32_t f = irq_save();
    if (q->seq != seq){ irq_restore(f); return 0; }     // a wake already happened
    int r = task_block_locked(q, to_ticks(timeout_ms));
    irq_restore(f);
    return r;
}

int waitq_wait(waitq_t* q, uint32_t timeout_ms){
    return waitq_wait_seq(q, q->seq, timeout_ms);
}

int waitq_wait_until(waitq_t* q, int (*cond)(void*), void* arg, uint32_t timeout_ms){
    unsigned long long deadline = ticks + to_ticks(timeout_ms);
    for (;;){
        uint32_t f = irq_save();
        if (cond(arg)){ irq_restore(f); return 0; }
        if (timeout_ms == 0){ irq_restore(f); return 1; }
        uint32_t left = 0;
        if (timeout_ms != WAIT_FOREVER){
            if (ticks >= deadline){ irq_restore(f); return 1; }
            left = (uint32_t)(deadline - ticks);
        }
        task_block_locked(q, left);
        irq_restore(f);
    }
}

// ------------------------------------------------------------------ mutex

void mutex_init(mutex_t* m){ m->held = 0; m->owner = 0; waitq_init(&m->wq); }

int mutex_trylock(mutex_t* m){
    uint32_t f = irq_save();
    int busy = m->held;
    if (!busy){ m->held = 1; m->owner = task_current_id(); }
    irq_restore(f);
    return busy ? -1 : 0;
}

void mutex_lock(mutex_t* m){
    for (;;){
        uint32_t f = irq_save();
        if (!m->held){
            m->held  = 1;
            m->owner = task_current_id();
            irq_restore(f);
            return;
        }
        task_block_locked(&m->wq, 0);       // woken by mutex_unlock; then re-check
        irq_restore(f);
    }
}

int mutex_unlock(mutex_t* m){
    uint32_t f = irq_save();
    if (!m->held || m->owner != task_current_id()){ irq_restore(f); return -1; }
    m->held = 0;
    task_wake_obj(&m->wq, 0);
    irq_restore(f);
    return 0;
}

int mutex_owned(const mutex_t* m){
    return m->held && m->owner == task_current_id();
}

// -------------------------------------------------------------- semaphore

void sem_init(sem_t* s, int initial){ s->count = initial; waitq_init(&s->wq); }

int sem_trywait(sem_t* s){
    uint32_t f = irq_save();
    int ok = s->count > 0;
    if (ok) s->count--;
    irq_restore(f);
    return ok ? 0 : -1;
}

int sem_timedwait(sem_t* s, uint32_t timeout_ms){
    unsigned long long deadline = ticks + to_ticks(timeout_ms);
    for (;;){
        uint32_t f = irq_save();
        if (s->count > 0){ s->count--; irq_restore(f); return 0; }
        uint32_t left = 0;
        if (timeout_ms != WAIT_FOREVER){
            if (timeout_ms == 0 || ticks >= deadline){ irq_restore(f); return 1; }
            left = (uint32_t)(deadline - ticks);
        }
        task_block_locked(&s->wq, left);
        irq_restore(f);
    }
}

void sem_wait(sem_t* s){ sem_timedwait(s, WAIT_FOREVER); }

void sem_post(sem_t* s){
    uint32_t f = irq_save();
    s->count++;
    task_wake_obj(&s->wq, 0);
    irq_restore(f);
}
