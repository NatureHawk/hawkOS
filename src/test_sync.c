// src/test_sync.c — wait queues, mutexes, semaphores
//
// What is being checked is behaviour a polling loop could never get wrong:
// that a wake is not lost whichever side of the wait it lands on, that a
// blocked task really gives the CPU away, and that a mutex holds even when
// the holder is forcibly switched out inside its critical section.
#include "header/ktest.h"
#include "header/sync.h"
#include "header/task.h"
#include "header/irqctl.h"

extern volatile unsigned long long ticks;

// ------------------------------------------------------------------ mutex

static mutex_t     mx;
static volatile int shared;
static volatile int workers_done;

#define MX_ROUNDS 150

static void mx_worker(void* arg){
    (void)arg;
    for (int i = 0; i < MX_ROUNDS; i++){
        mutex_lock(&mx);
        int v = shared;
        task_yield();               // hand the CPU over while holding the lock
        shared = v + 1;
        mutex_unlock(&mx);
    }
    __atomic_add_fetch(&workers_done, 1, __ATOMIC_SEQ_CST);
}

KTEST(sync, mutex_excludes_across_tasks){
    mutex_init(&mx);
    shared = 0;
    workers_done = 0;
    for (int i = 0; i < 3; i++) KT_TRUE(task_create("mx", mx_worker, 0) >= 0);

    unsigned long long deadline = ticks + 3000;
    while (workers_done < 3 && ticks < deadline) task_sleep(10);
    KT_EQ(workers_done, 3);
    KT_EQ(shared, 3 * MX_ROUNDS);
}

KTEST(sync, mutex_ownership_rules){
    mutex_t m;
    mutex_init(&m);
    KT_EQ(mutex_unlock(&m), -1);        // not held
    KT_EQ(mutex_trylock(&m), 0);
    KT_TRUE(mutex_owned(&m));
    KT_EQ(mutex_trylock(&m), -1);       // non-recursive
    KT_EQ(mutex_unlock(&m), 0);
    KT_FALSE(mutex_owned(&m));
}

// -------------------------------------------------------------- semaphore

#define SEM_N 60
static sem_t       items, slots;
static int         ring[4];
static volatile int prod_i, cons_sum;

static void producer(void* arg){
    (void)arg;
    for (int i = 1; i <= SEM_N; i++){
        sem_wait(&slots);
        ring[prod_i++ & 3] = i;
        sem_post(&items);
    }
}

KTEST(sync, semaphore_producer_consumer){
    sem_init(&items, 0);
    sem_init(&slots, 4);                // a 4-deep ring forces the producer to block too
    prod_i = 0;
    cons_sum = 0;
    KT_TRUE(task_create("producer", producer, 0) >= 0);

    int cons_i = 0, in_order = 1;
    for (int n = 1; n <= SEM_N; n++){
        KT_EQ(sem_timedwait(&items, 2000), 0);
        int v = ring[cons_i++ & 3];
        if (v != n) in_order = 0;
        cons_sum += v;
        sem_post(&slots);
    }
    KT_TRUE(in_order);
    KT_EQ(cons_sum, SEM_N * (SEM_N + 1) / 2);
    KT_EQ(sem_trywait(&items), -1);
}

// ------------------------------------------------------------ wait queues

KTEST(sync, waitq_times_out){
    waitq_t q = WAITQ_INIT;
    unsigned long long t0 = ticks;
    int r = waitq_wait(&q, 50);
    unsigned long long dt = ticks - t0;
    KT_EQ(r, 1);
    KT_TRUE(dt >= 4 && dt <= 12);       // 5 ticks, give or take a tick either way
    KT_EQ(waitq_wait(&q, 0), 1);        // zero timeout never blocks
}

KTEST(sync, wake_before_wait_is_not_lost){
    waitq_t q = WAITQ_INIT;
    uint32_t seq = waitq_seq(&q);
    waitq_wake_all(&q);                 // lands between "read the condition" and "wait"
    unsigned long long t0 = ticks;
    int r = waitq_wait_seq(&q, seq, 2000);
    KT_EQ(r, 0);
    KT_TRUE(ticks - t0 <= 1);           // returned at once, did not sleep the 2 s
}

static int cond_flag;
static int cond_fn(void* a){ (void)a; return cond_flag; }

KTEST(sync, wait_until_sees_condition_set_without_wake){
    waitq_t q = WAITQ_INIT;
    cond_flag = 1;
    KT_EQ(waitq_wait_until(&q, cond_fn, 0, 1000), 0);
    cond_flag = 0;
    KT_EQ(waitq_wait_until(&q, cond_fn, 0, 30), 1);
}

static waitq_t      wq_irq;
static volatile int woke;

static void waiter(void* arg){
    (void)arg;
    if (waitq_wait(&wq_irq, 2000) == 0) woke = 1; else woke = -1;
}

static int state_of(const char* name){
    task_info_t rows[TASK_MAX];
    int n = task_snapshot(rows, TASK_MAX);
    for (int i = 0; i < n; i++){
        const char* a = rows[i].name; const char* b = name;
        while (*a && *a == *b){ a++; b++; }
        if (*a == 0 && *b == 0) return rows[i].state;
    }
    return -1;
}

KTEST(sync, wake_from_irq_off_context){
    waitq_init(&wq_irq);
    woke = 0;
    KT_TRUE(task_create("waiter", waiter, 0) >= 0);
    task_sleep(40);
    KT_EQ(state_of("waiter"), TASK_BLOCKED);
    KT_EQ(woke, 0);

    // Interrupts off, exactly as inside a handler: the wake must only mark the
    // task ready, never try to switch away from here.
    uint32_t f = irq_save();
    int n = waitq_wake_one(&wq_irq);
    irq_restore(f);
    KT_EQ(n, 1);

    unsigned long long t0 = ticks;
    while (woke == 0 && ticks - t0 < 50) task_sleep(10);
    KT_EQ(woke, 1);
    KT_TRUE(ticks - t0 <= 3);           // a few ticks, not the 2 s timeout
}

static waitq_t      wq_idle;
static volatile int idle_release;

static void sleeper(void* arg){
    (void)arg;
    waitq_wait(&wq_idle, 5000);
    idle_release = 1;
}

KTEST(sync, blocked_tasks_leave_the_cpu_to_idle){
    waitq_init(&wq_idle);
    idle_release = 0;
    for (int i = 0; i < 4; i++) KT_TRUE(task_create("sleeper", sleeper, 0) >= 0);

    uint32_t i0, a0, i1, a1;
    task_sleep(20);
    sched_load(&i0, &a0);
    task_sleep(300);
    sched_load(&i1, &a1);
    uint32_t idle = i1 - i0, all = a1 - a0;
    KT_TRUE(all >= 25);
    KT_TRUE(idle * 10 >= all * 8);      // at least 80% idle with four tasks parked

    KT_EQ(waitq_wake_all(&wq_idle), 4);
    task_sleep(50);
    KT_EQ(idle_release, 1);
}
