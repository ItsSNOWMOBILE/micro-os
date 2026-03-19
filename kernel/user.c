/*
 * user.c — User-mode task support.
 *
 * Creates tasks that run in Ring 3.  Each user task gets:
 *   - Its own PML4 (address space) with kernel mappings cloned
 *   - A kernel stack (for syscall/interrupt handling)
 *   - A user stack mapped with PTE_USER in the task's address space
 *   - Entry via IRETQ into Ring 3
 *
 * User code lives in the identity-mapped kernel region.  The relevant
 * 2 MiB huge page is marked PTE_USER so Ring 3 can execute it.
 * A production OS would copy code to a separate user-accessible region.
 */

#include "user.h"
#include "kernel.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "interrupts/gdt.h"
#include "string.h"
#include "console.h"

/* All user tasks get their stack at the same virtual address in their
 * own address space.  This is safe because each task has its own PML4. */
#define USER_STACK_VIRT  USER_STACK_BASE

/*
 * Per-task info passed from user_task_create to the trampoline.
 */
static struct {
    uint64_t entry;
    uint64_t user_rsp;
} pending_user[MAX_TASKS];

static void
user_task_trampoline(void)
{
    Task *t = sched_current();

    /* Find our pending info by scanning for a matching entry. */
    uint64_t entry = 0, user_rsp = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (pending_user[i].entry != 0 && pending_user[i].user_rsp != 0) {
            entry = pending_user[i].entry;
            user_rsp = pending_user[i].user_rsp;
            pending_user[i].entry = 0;
            pending_user[i].user_rsp = 0;
            break;
        }
    }

    if (!entry) {
        kprintf("user_task_trampoline: no pending entry for %s\n", t->name);
        sched_exit(1);
        return;
    }

    /* Set TSS RSP0 to this task's kernel stack top. When Ring 3 takes
     * an interrupt, the CPU loads this as the new stack pointer. */
    gdt_set_rsp0((uint64_t)t->stack_base + TASK_STACK_SIZE);

    /* The scheduler already switched CR3 to our PML4 before we ran. */

    /* IRETQ into Ring 3. */
    jump_to_user(entry, user_rsp, GDT_USER_CODE_RPL3, GDT_USER_DATA_RPL3);
}

Task *
user_task_create(const char *name, void (*entry)(void))
{
    /* Create a per-process address space. */
    uint64_t *task_pml4 = vmm_create_address_space();
    if (!task_pml4) return NULL;

    /* Map user stack pages into the task's address space. */
    uint64_t user_stack_top = USER_STACK_VIRT;
    uint64_t user_stack_bottom = user_stack_top - USER_STACK_SIZE;

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *page = pmm_alloc_page();
        if (!page) return NULL;
        uint64_t virt = user_stack_bottom + (uint64_t)i * 4096;
        vmm_map_page_in(task_pml4, virt, (uint64_t)page, PTE_WRITABLE | PTE_USER);
    }

    /* The code page is in the identity-mapped region (cloned from kernel
     * PML4).  Mark the 2 MiB huge page as user-accessible in the kernel
     * page tables — the clone shares the same PDPT/PD entries. */
    vmm_mark_huge_user((uint64_t)entry);

    /* Store pending info. Use the first free slot. */
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (pending_user[i].entry == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return NULL;

    pending_user[slot].entry = (uint64_t)entry;
    pending_user[slot].user_rsp = user_stack_top;

    /* Create a kernel task that will trampoline into user mode. */
    Task *t = task_create(name, user_task_trampoline);
    if (t) {
        t->is_user = true;
        t->pml4 = task_pml4;
    }
    return t;
}
