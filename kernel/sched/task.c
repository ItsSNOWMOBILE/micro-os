/*
 * task.c — Round-robin preemptive scheduler.
 *
 * Tasks are stored in a fixed array.  The scheduler picks the next
 * READY task in round-robin order.  Preemption happens on each timer
 * tick (IRQ 0 calls sched_schedule).  Tasks can also yield voluntarily.
 */

#include "task.h"
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

    /* Task 0: the current execution context (becomes the idle task).
     * Its context will be saved on the first context switch. */
    Task *idle = &tasks[0];
    idle->id    = 0;
    idle->state = TASK_RUNNING;
    idle->name  = "idle";
    idle->stack_base = NULL;  /* uses the boot stack */
    task_count = 1;
}

/* ── Task creation ───────────────────────────────────────────────────────── */

/*
 * When a task's entry function returns, it lands here.
 * Mark the task as dead and yield — the scheduler will never pick it again.
 */
static void
task_exit_trampoline(void)
{
    tasks[current_task].state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");  /* unreachable */
}

Task *
task_create(const char *name, void (*entry)(void))
{
    if (task_count >= MAX_TASKS)
        return NULL;

    int slot = task_count++;
    Task *t = &tasks[slot];

    t->id    = next_id++;
    t->state = TASK_READY;
    t->name  = name;

    /* Allocate a stack. */
    t->stack_base = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!t->stack_base)
        return NULL;

    uint64_t stack_top = (uint64_t)(t->stack_base + TASK_STACK_SIZE);
    /* Align to 16 bytes. */
    stack_top &= ~(uint64_t)0xF;

    /*
     * Set up the initial stack so context_switch will "return" into the
     * task entry point.  context_switch pops: r15, r14, r13, r12, rbp,
     * rbx, rflags, then ret.
     *
     * We push a return address (task_exit_trampoline) as the "caller"
     * of entry, so that when entry() returns, it hits the trampoline.
     * Then we push entry as the ret address for context_switch.
     */
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

    /* Pre-fill register slots on the stack so context_switch pops them. */
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

    /* Round-robin: start after current, wrap around. */
    for (int i = 1; i <= task_count; i++) {
        int idx = (current_task + i) % task_count;
        if (tasks[idx].state == TASK_READY)
            return idx;
    }

    return 0;  /* fall back to idle */
}

void
sched_yield(void)
{
    int next = pick_next();
    if (next == current_task)
        return;

    int prev = current_task;

    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;

    current_task = next;
    tasks[next].state = TASK_RUNNING;

    context_switch(&tasks[prev].ctx, &tasks[next].ctx);
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
