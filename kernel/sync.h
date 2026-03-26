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
    uint64_t          flags;  /* saved RFLAGS for interrupt restore */
} Spinlock;

#define SPINLOCK_INIT { .lock = 0, .flags = 0 }

static inline void spin_init(Spinlock *s) { s->lock = 0; s->flags = 0; }

static inline void spin_lock(Spinlock *s)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
    while (__sync_lock_test_and_set(&s->lock, 1))
        __asm__ volatile("pause");
    s->flags = flags;
}

static inline void spin_unlock(Spinlock *s)
{
    uint64_t flags = s->flags;
    __sync_lock_release(&s->lock);
    __asm__ volatile("push %0; popfq" :: "r"(flags));
}

static inline bool spin_try_lock(Spinlock *s)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
    if (__sync_lock_test_and_set(&s->lock, 1) == 0) {
        s->flags = flags;
        return true;
    }
    __asm__ volatile("push %0; popfq" :: "r"(flags));
    return false;
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
