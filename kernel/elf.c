/*
 * elf.c — ELF64 binary loader.
 *
 * Reads an ELF64 executable from the VFS, validates headers, maps
 * PT_LOAD segments into a new per-process address space, allocates
 * a user stack, and creates a user-mode task at the ELF entry point.
 */

#include "elf.h"
#include "kernel.h"
#include "console.h"
#include "string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "interrupts/gdt.h"
#include "sched/task.h"
#include "user.h"
#include "fs/vfs.h"

/* ── Trampoline ─────────────────────────────────────────────────────────── */

struct elf_launch_info {
    uint64_t entry;
    uint64_t user_rsp;
};

static struct elf_launch_info elf_pending[MAX_TASKS];

static void
elf_trampoline(void)
{
    Task *t = sched_current();

    /* Find our pending info. */
    uint64_t entry = 0, user_rsp = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (elf_pending[i].entry != 0) {
            entry = elf_pending[i].entry;
            user_rsp = elf_pending[i].user_rsp;
            elf_pending[i].entry = 0;
            elf_pending[i].user_rsp = 0;
            break;
        }
    }

    if (!entry) {
        kprintf("elf_trampoline: no pending entry for %s\n", t->name);
        sched_exit(1);
        return;
    }

    gdt_set_rsp0((uint64_t)t->stack_base + TASK_STACK_SIZE);
    jump_to_user(entry, user_rsp, GDT_USER_CODE_RPL3, GDT_USER_DATA_RPL3);
}

/* ── Loader ─────────────────────────────────────────────────────────────── */

Task *
elf_load(const char *path, const char *name)
{
    /* Read the entire file into a kernel buffer. */
    VfsStat st;
    if (vfs_stat(path, &st) != 0) {
        kprintf("elf: %s: not found\n", path);
        return NULL;
    }
    if (st.size < sizeof(Elf64_Ehdr)) {
        kprintf("elf: %s: too small\n", path);
        return NULL;
    }

    int fd = vfs_open(path, false);
    if (fd < 0) return NULL;

    uint8_t *buf = kmalloc(st.size);
    if (!buf) { vfs_close(fd); return NULL; }

    int nread = vfs_read(fd, buf, st.size);
    vfs_close(fd);
    if (nread != (int)st.size) { kfree(buf); return NULL; }

    /* Validate ELF header. */
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    if (ehdr->e_ident_magic != ELF_MAGIC) {
        kprintf("elf: %s: bad magic\n", path);
        kfree(buf);
        return NULL;
    }
    if (ehdr->e_ident_class != 2 || ehdr->e_ident_data != 1) {
        kprintf("elf: %s: not ELF64 little-endian\n", path);
        kfree(buf);
        return NULL;
    }
    if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_X86_64) {
        kprintf("elf: %s: not x86-64 executable\n", path);
        kfree(buf);
        return NULL;
    }

    /* Create per-process address space. */
    uint64_t *task_pml4 = vmm_create_address_space();
    if (!task_pml4) { kfree(buf); return NULL; }

    /* Map PT_LOAD segments. */
    Elf64_Phdr *phdr = (Elf64_Phdr *)(buf + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uint64_t vaddr = phdr[i].p_vaddr & ~0xFFFULL;
        uint64_t end   = (phdr[i].p_vaddr + phdr[i].p_memsz + 0xFFF) & ~0xFFFULL;

        uint64_t flags = PTE_USER;
        if (phdr[i].p_flags & PF_W) flags |= PTE_WRITABLE;

        /* Map pages and copy data. */
        for (uint64_t addr = vaddr; addr < end; addr += 4096) {
            void *page = pmm_alloc_page();
            if (!page) { kfree(buf); return NULL; }
            memset(page, 0, 4096);

            /* Copy file data into this page if it overlaps. */
            uint64_t seg_start = phdr[i].p_vaddr;
            uint64_t seg_file_end = seg_start + phdr[i].p_filesz;
            uint64_t page_start = addr;
            uint64_t page_end = addr + 4096;

            if (page_start < seg_file_end && page_end > seg_start) {
                uint64_t copy_start = page_start > seg_start ? page_start : seg_start;
                uint64_t copy_end = page_end < seg_file_end ? page_end : seg_file_end;
                uint64_t file_off = phdr[i].p_offset + (copy_start - seg_start);
                uint64_t page_off = copy_start - page_start;
                memcpy((uint8_t *)page + page_off, buf + file_off, copy_end - copy_start);
            }

            vmm_map_page_in(task_pml4, addr, (uint64_t)page, flags);
        }
    }

    kfree(buf);

    /* Map user stack. */
    uint64_t user_stack_top = USER_STACK_BASE;
    uint64_t user_stack_bottom = user_stack_top - USER_STACK_SIZE;

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *page = pmm_alloc_page();
        if (!page) return NULL;
        uint64_t virt = user_stack_bottom + (uint64_t)i * 4096;
        vmm_map_page_in(task_pml4, virt, (uint64_t)page, PTE_WRITABLE | PTE_USER);
    }

    /* Store pending info. */
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (elf_pending[i].entry == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return NULL;

    elf_pending[slot].entry = ehdr->e_entry;
    elf_pending[slot].user_rsp = user_stack_top;

    /* Create the kernel task. */
    Task *t = task_create(name, elf_trampoline);
    if (t) {
        t->is_user = true;
        t->pml4 = task_pml4;
    }
    return t;
}
