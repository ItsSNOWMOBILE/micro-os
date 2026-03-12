/*
 * task.h — Task (thread) management.
 *
 * Each task has its own kernel stack and saved register context.  The
 * scheduler is cooperative for now (yield-based), with a timer-driven
 * preemptive path added once the PIT is running.
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define TASK_STACK_SIZE  (4096 * 4)  /* 16 KiB per task */
#define MAX_TASKS        64

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD,
} TaskState;

typedef struct {
    uint64_t rsp;   /* saved stack pointer — first field for asm access */
    uint64_t rip;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rflags;
} TaskContext;

typedef struct Task {
    TaskContext ctx;
    uint64_t   id;
    TaskState  state;
    uint64_t   wake_tick;     /* for sleep */
    uint8_t   *stack_base;    /* bottom of allocated stack */
    const char *name;
} Task;

void  sched_init(void);
Task *task_create(const char *name, void (*entry)(void));
void  sched_yield(void);
void  sched_schedule(void);          /* called from timer ISR */
void  sched_sleep(uint64_t ticks);
Task *sched_current(void);

/* Assembly context switch. */
extern void context_switch(TaskContext *old_ctx, TaskContext *new_ctx);

#endif /* TASK_H */
