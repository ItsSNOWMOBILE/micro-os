/*
 * task.c — Priority-based preemptive scheduler.
 *
 * Tasks are stored in a fixed array.  The scheduler picks the highest
 * priority READY task; within the same priority, it uses round-robin.
 * Preemption happens on each timer tick.  Tasks can also yield
 * voluntarily.  Dead tasks are cleaned up (stack freed) on the next
 * scheduling pass.
 */

#include "task.h"
#include <stdbool.h>
#include "../kernel.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../console.h"
#include "../drivers/timer.h"

static Task  tasks[MAX_TASKS];
static int   task_count;
static int   current_task;
static uint64_t next_id = 1;

/* ── Init ────────────────────────────────────────────────────────────────── */

void
sched_init(void)
{
    memset(tasks, 0, sizeof(tasks));
    task_count = 0;
    current_task = 0;

    /* Task 0: the current execution context (becomes the idle task). */
    Task *idle = &tasks[0];
    idle->id    = 0;
    idle->state = TASK_RUNNING;
    idle->name  = "idle";
    idle->priority = PRIORITY_IDLE;
    idle->stack_base = NULL;  /* uses the boot stack */
    task_count = 1;
}

/* ── Task creation ───────────────────────────────────────────────────────── */

static void
task_exit_trampoline(void)
{
    tasks[current_task].state = TASK_DEAD;
    tasks[current_task].exit_code = 0;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

static Task *
do_task_create(const char *name, void (*entry)(void), int priority)
{
    if (task_count >= MAX_TASKS)
        return NULL;

    int slot = task_count++;
    Task *t = &tasks[slot];

    t->id    = next_id++;
    t->state = TASK_READY;
    t->name  = name;
    t->priority = priority;
    t->exit_code = 0;
    t->parent_id = tasks[current_task].id;
    t->wait_for_id = 0;

    /* Allocate a stack. */
    t->stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!t->stack_base)
        return NULL;

    uint64_t stack_top = (uint64_t)(t->stack_base + TASK_STACK_SIZE);
    stack_top &= ~(uint64_t)0xF;

    /* Push return address (trampoline) and entry point. */
    stack_top -= 8;
    *(uint64_t *)stack_top = (uint64_t)task_exit_trampoline;
    stack_top -= 8;
    *(uint64_t *)stack_top = (uint64_t)entry;

    /* Room for the 7 registers that context_switch pops. */
    t->ctx.rsp    = stack_top - 7 * 8;
    t->ctx.rip    = (uint64_t)entry;
    t->ctx.rbx    = 0;
    t->ctx.rbp    = 0;
    t->ctx.r12    = 0;
    t->ctx.r13    = 0;
    t->ctx.r14    = 0;
    t->ctx.r15    = 0;
    t->ctx.rflags = 0x202;  /* IF set */

    /* Pre-fill register slots on the stack. */
    uint64_t *sp = (uint64_t *)t->ctx.rsp;
    sp[0] = 0;      /* r15 */
    sp[1] = 0;      /* r14 */
    sp[2] = 0;      /* r13 */
    sp[3] = 0;      /* r12 */
    sp[4] = 0;      /* rbp */
    sp[5] = 0;      /* rbx */
    sp[6] = 0x202;  /* rflags (IF set) */

    return t;
}

Task *
task_create(const char *name, void (*entry)(void))
{
    return do_task_create(name, entry, PRIORITY_NORMAL);
}

Task *
task_create_prio(const char *name, void (*entry)(void), int priority)
{
    if (priority < 0) priority = 0;
    if (priority >= NUM_PRIORITIES) priority = NUM_PRIORITIES - 1;
    return do_task_create(name, entry, priority);
}

/* ── Dead task cleanup ───────────────────────────────────────────────────── */

static void
cleanup_dead_tasks(void)
{
    for (int i = 1; i < task_count; i++) {
        if (tasks[i].state != TASK_DEAD)
            continue;
        /* Never free the stack of the currently running task —
         * we're still executing on it until context_switch completes. */
        if (i == current_task)
            continue;

        /* Check if any task is waiting for this one. */
        bool someone_waiting = false;
        for (int j = 0; j < task_count; j++) {
            if (tasks[j].state == TASK_BLOCKED &&
                tasks[j].wait_for_id == tasks[i].id) {
                /* Wake the waiter. */
                tasks[j].state = TASK_READY;
                tasks[j].exit_code = tasks[i].exit_code;
                tasks[j].wait_for_id = 0;
                someone_waiting = true;
            }
        }

        /* Free the stack. */
        if (tasks[i].stack_base) {
            kfree(tasks[i].stack_base);
            tasks[i].stack_base = NULL;
        }

        /* Mark slot as reusable by keeping it dead but with no resources. */
        (void)someone_waiting;
    }
}

/* ── Scheduling ──────────────────────────────────────────────────────────── */

static int
pick_next(void)
{
    /* Wake sleeping tasks whose deadline has passed. */
    uint64_t now = timer_ticks();
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_tick)
            tasks[i].state = TASK_READY;
    }

    /* Clean up dead tasks. */
    cleanup_dead_tasks();

    /* Priority-based round-robin: find highest priority (lowest number)
     * that has a READY task, then pick next in round-robin within that. */
    for (int prio = PRIORITY_HIGH; prio < NUM_PRIORITIES; prio++) {
        for (int i = 1; i <= task_count; i++) {
            int idx = (current_task + i) % task_count;
            if (tasks[idx].state == TASK_READY && tasks[idx].priority == prio)
                return idx;
        }
    }

    return 0;  /* fall back to idle */
}

void
sched_yield(void)
{
    /* Disable interrupts to prevent the timer ISR from triggering a
     * nested context switch while we're mid-yield. */
    cli();

    int next = pick_next();
    if (next == current_task) {
        sti();
        return;
    }

    int prev = current_task;

    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;

    current_task = next;
    tasks[next].state = TASK_RUNNING;

    context_switch(&tasks[prev].ctx, &tasks[next].ctx);

    /* Re-enable interrupts after we've been switched back in. */
    sti();
}

void
sched_schedule(void)
{
    sched_yield();
}

void
sched_sleep(uint64_t ticks)
{
    tasks[current_task].state     = TASK_SLEEPING;
    tasks[current_task].wake_tick = timer_ticks() + ticks;
    sched_yield();
}

Task *
sched_current(void)
{
    return &tasks[current_task];
}

int
sched_wait(uint64_t task_id)
{
    /* Find the task. */
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].id == task_id) {
            if (tasks[i].state == TASK_DEAD) {
                return tasks[i].exit_code;
            }
            /* Block until target exits. */
            tasks[current_task].state = TASK_BLOCKED;
            tasks[current_task].wait_for_id = task_id;
            sched_yield();
            return tasks[current_task].exit_code;
        }
    }
    return -1;  /* task not found */
}

void
sched_exit(int code)
{
    tasks[current_task].exit_code = code;
    tasks[current_task].state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

bool
sched_any_priority(int priority)
{
    for (int i = 1; i < task_count; i++) {
        if (tasks[i].priority == priority && tasks[i].state != TASK_DEAD)
            return true;
    }
    return false;
}

static const char *state_str[] = {
    "ready", "running", "sleeping", "blocked", "dead"
};

static const char *prio_str[] = {
    "high", "normal", "low", "idle"
};

void
sched_list_tasks(void)
{
    for (int i = 0; i < task_count; i++) {
        Task *t = &tasks[i];
        if (t->state == TASK_DEAD && !t->stack_base)
            continue;  /* cleaned up slot */
        const char *st = (t->state <= TASK_DEAD) ? state_str[t->state] : "?";
        const char *pr = (t->priority < NUM_PRIORITIES) ? prio_str[t->priority] : "?";
        kprintf("%-4lu %-12s %-10s %-8s\n", t->id, t->name, st, pr);
    }
}
