/*
 * sync.h — Kernel synchronization primitives.
 */

#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>
#include <stdbool.h>

/* ── Spinlock ────────────────────────────────────────────────────────────── */

typedef struct {
    volatile uint32_t lock;
} Spinlock;

#define SPINLOCK_INIT { .lock = 0 }

static inline void spin_init(Spinlock *s) { s->lock = 0; }

static inline void spin_lock(Spinlock *s)
{
    while (__sync_lock_test_and_set(&s->lock, 1))
        __asm__ volatile("pause");
}

static inline void spin_unlock(Spinlock *s)
{
    __sync_lock_release(&s->lock);
}

static inline bool spin_try_lock(Spinlock *s)
{
    return __sync_lock_test_and_set(&s->lock, 1) == 0;
}

/* ── Mutex (sleeping lock) ───────────────────────────────────────────────── */

#define MUTEX_WAIT_QUEUE_SIZE 16

typedef struct {
    volatile uint32_t locked;
    volatile int      owner;      /* task index, -1 if unlocked */
    volatile int      wait_queue[MUTEX_WAIT_QUEUE_SIZE];
    volatile int      wait_count;
    Spinlock          guard;
} Mutex;

void mutex_init(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);
bool mutex_try_lock(Mutex *m);

/* ── Semaphore ───────────────────────────────────────────────────────────── */

#define SEM_WAIT_QUEUE_SIZE 16

typedef struct {
    volatile int      count;
    volatile int      wait_queue[SEM_WAIT_QUEUE_SIZE];
    volatile int      wait_count;
    Spinlock          guard;
} Semaphore;

void sem_init(Semaphore *s, int initial);
void sem_wait(Semaphore *s);
void sem_signal(Semaphore *s);
bool sem_try_wait(Semaphore *s);

#endif /* SYNC_H */
