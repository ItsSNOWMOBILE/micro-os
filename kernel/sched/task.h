/*
 * task.h — Task (thread) management.
 *
 * Each task has its own kernel stack and saved register context.
 * Supports priority-based scheduling and task lifecycle management.
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>

#define TASK_STACK_SIZE  (4096 * 4)  /* 16 KiB per task */
#define MAX_TASKS        64

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_DEAD,
} TaskState;

/* Priority levels: lower number = higher priority. */
#define PRIORITY_HIGH    0
#define PRIORITY_NORMAL  1
#define PRIORITY_LOW     2
#define PRIORITY_IDLE    3
#define NUM_PRIORITIES   4

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

#define SIG_MAX_TASK 20

typedef struct Task {
    TaskContext ctx;
    uint64_t   id;
    TaskState  state;
    uint64_t   wake_tick;      /* for sleep */
    uint8_t   *stack_base;     /* bottom of allocated stack */
    const char *name;
    int        priority;       /* 0=high, 3=idle */
    int        exit_code;
    uint64_t   parent_id;      /* ID of parent task */
    uint64_t   wait_for_id;    /* task ID we're waiting on (0 = none) */
    bool       is_user;        /* true if this is a Ring 3 task */

    /* Signal state. */
    uint32_t   sig_pending;    /* bitmask of pending signals */
    uint64_t   sig_handlers[SIG_MAX_TASK]; /* 0=SIG_DFL, 1=SIG_IGN, else handler addr */

    /* Per-process address space. NULL = use kernel page tables. */
    uint64_t  *pml4;           /* task's own PML4 (physical address) */

    int        pending_slot;   /* index into pending_user/elf_pending (-1 = none) */
} Task;

void  sched_init(void);
Task *task_create(const char *name, void (*entry)(void));
Task *task_create_prio(const char *name, void (*entry)(void), int priority);
void  sched_yield(void);
void  sched_schedule(void);          /* called from timer ISR */
void  sched_sleep(uint64_t ticks);
Task *sched_current(void);

/* Wait for a specific task to exit. Returns its exit code. */
int   sched_wait(uint64_t task_id);

/* Exit the current task with a code. */
void  sched_exit(int code);

/* Check if any non-dead tasks exist at the given priority. */
bool  sched_any_priority(int priority);

/* Print task list (for ps command). */
void  sched_list_tasks(void);

/* Find a task by ID. Returns NULL if not found. */
Task *sched_find_task(uint64_t id);

/* Send a signal to a task. Returns 0 on success, -1 if not found. */
int   sched_send_signal(uint64_t task_id, int sig);

/* Assembly context switch. */
extern void context_switch(TaskContext *old_ctx, TaskContext *new_ctx);

#endif /* TASK_H */
