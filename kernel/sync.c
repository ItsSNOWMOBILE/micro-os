/*
 * sync.c — Kernel synchronization primitives.
 *
 * Mutex and semaphore use a wait queue: blocked tasks are put to sleep
 * and woken when the resource becomes available, avoiding busy-waiting.
 */

#include "sync.h"
#include "sched/task.h"
#include "kernel.h"

/* ── Mutex ───────────────────────────────────────────────────────────────── */

void
mutex_init(Mutex *m)
{
    m->locked = 0;
    m->owner  = -1;
    m->wait_count = 0;
    spin_init(&m->guard);
}

void
mutex_lock(Mutex *m)
{
    for (;;) {
        spin_lock(&m->guard);

        if (!m->locked) {
            m->locked = 1;
            m->owner  = (int)sched_current()->id;
            spin_unlock(&m->guard);
            return;
        }

        /* Add ourselves to the wait queue. */
        if (m->wait_count < MUTEX_WAIT_QUEUE_SIZE) {
            m->wait_queue[m->wait_count++] = (int)sched_current()->id;
        }
        spin_unlock(&m->guard);

        /* Sleep briefly and retry. */
        sched_sleep(1);
    }
}

void
mutex_unlock(Mutex *m)
{
    spin_lock(&m->guard);
    m->locked = 0;
    m->owner  = -1;

    /* Wake the first waiter if any. */
    if (m->wait_count > 0) {
        /* Shift queue down. */
        for (int i = 0; i < m->wait_count - 1; i++)
            m->wait_queue[i] = m->wait_queue[i + 1];
        m->wait_count--;
    }

    spin_unlock(&m->guard);
}

bool
mutex_try_lock(Mutex *m)
{
    spin_lock(&m->guard);
    if (!m->locked) {
        m->locked = 1;
        m->owner  = (int)sched_current()->id;
        spin_unlock(&m->guard);
        return true;
    }
    spin_unlock(&m->guard);
    return false;
}

/* ── Semaphore ───────────────────────────────────────────────────────────── */

void
sem_init(Semaphore *s, int initial)
{
    s->count = initial;
    s->wait_count = 0;
    spin_init(&s->guard);
}

void
sem_wait(Semaphore *s)
{
    for (;;) {
        spin_lock(&s->guard);

        if (s->count > 0) {
            s->count--;
            spin_unlock(&s->guard);
            return;
        }

        if (s->wait_count < SEM_WAIT_QUEUE_SIZE) {
            s->wait_queue[s->wait_count++] = (int)sched_current()->id;
        }
        spin_unlock(&s->guard);

        sched_sleep(1);
    }
}

void
sem_signal(Semaphore *s)
{
    spin_lock(&s->guard);
    s->count++;

    if (s->wait_count > 0) {
        for (int i = 0; i < s->wait_count - 1; i++)
            s->wait_queue[i] = s->wait_queue[i + 1];
        s->wait_count--;
    }

    spin_unlock(&s->guard);
}

bool
sem_try_wait(Semaphore *s)
{
    spin_lock(&s->guard);
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->guard);
        return true;
    }
    spin_unlock(&s->guard);
    return false;
}
