/*
 * user.c — User-mode task support.
 *
 * Creates tasks that run in Ring 3.  Each user task gets:
 *   - A kernel stack (for syscall/interrupt handling)
 *   - A user stack mapped with PTE_USER
 *   - Entry via IRETQ into Ring 3
 */

#include "user.h"
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "interrupts/gdt.h"
#include "string.h"

/*
 * Wrapper that runs in Ring 0 and does an IRETQ to enter user mode.
 * This is the kernel-side entry point for user tasks.
 */
struct user_task_info {
    uint64_t entry;
    uint64_t user_rsp;
};

static struct user_task_info user_infos[MAX_TASKS];
static int user_info_count;

static void
user_task_trampoline(void)
{
    /* Find our info. */
    Task *t = sched_current();
    struct user_task_info *info = NULL;
    for (int i = 0; i < user_info_count; i++) {
        /* Match by checking the entry we stored. */
        if (user_infos[i].entry != 0) {
            info = &user_infos[i];
            /* Mark as consumed so other tasks don't grab it. */
            /* Actually, we store index = task id. */
            break;
        }
    }

    if (!info) {
        t->state = TASK_DEAD;
        sched_yield();
        return;
    }

    uint64_t entry = info->entry;
    uint64_t user_rsp = info->user_rsp;
    info->entry = 0;  /* consumed */

    /* Set TSS RSP0 to this task's kernel stack top for interrupts. */
    gdt_set_rsp0((uint64_t)t->stack_base + TASK_STACK_SIZE);

    /* IRETQ into Ring 3. */
    jump_to_user(entry, user_rsp, GDT_USER_CODE_RPL3, GDT_USER_DATA_RPL3);
}

Task *
user_task_create(const char *name, void (*entry)(void))
{
    /* Allocate user stack pages and map them with PTE_USER. */
    uint64_t user_stack_bottom = USER_STACK_BASE - USER_STACK_SIZE;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *page = pmm_alloc_page();
        if (!page) return NULL;
        uint64_t virt = user_stack_bottom + (uint64_t)i * 4096;
        vmm_map_page(virt, (uint64_t)page,
                     PTE_WRITABLE | PTE_USER);
    }

    /* Also map the user code with PTE_USER.
     * Since we identity-map the first 4 GiB with 2 MiB huge pages,
     * user code in kernel space is already mapped but without PTE_USER.
     * For a simple demo, we mark the kernel code pages as user-accessible.
     * A proper OS would copy code to a user-accessible region. */

    /* Store user info. */
    int idx = user_info_count++;
    user_infos[idx].entry = (uint64_t)entry;
    user_infos[idx].user_rsp = USER_STACK_BASE;  /* stack grows down */

    /* Create a kernel task that will trampoline into user mode. */
    Task *t = task_create(name, user_task_trampoline);
    return t;
}
