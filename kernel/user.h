/*
 * user.h — User-mode task support.
 *
 * Provides functions to create and run user-mode (Ring 3) tasks.
 * User tasks get their own stack mapped at a fixed user-space address,
 * use INT 0x80 for syscalls, and share the kernel's identity-mapped
 * page tables (with PTE_USER set on user-accessible pages).
 */

#ifndef USER_H
#define USER_H

#include <stdint.h>
#include "sched/task.h"

/* User stack is mapped at this virtual address (top of lower-half). */
#define USER_STACK_BASE  0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 4
#define USER_STACK_SIZE  (USER_STACK_PAGES * 4096)

/* Create a task that runs entry() in Ring 3. */
Task *user_task_create(const char *name, void (*entry)(void));

/* Jump to user mode — called from kernel to enter a user task for the
 * first time. Does not return. */
void jump_to_user(uint64_t entry, uint64_t user_rsp,
                  uint16_t user_cs, uint16_t user_ds);

#endif /* USER_H */
