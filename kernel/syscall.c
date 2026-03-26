/*
 * syscall.c — System call dispatcher.
 *
 * Handles INT 0x80 system calls.
 * Convention:
 *   RAX = syscall number
 *   RDI = arg0, RSI = arg1, RDX = arg2, R10 = arg3
 *   Return value in RAX.
 */

#include "syscall.h"
#include "kernel.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "sched/task.h"
#include "drivers/keyboard.h"
#include "interrupts/idt.h"
#include "interrupts/gdt.h"
#include "fs/vfs.h"

/* ── Userspace pointer validation ────────────────────────────────────────── */

#define USER_ADDR_LIMIT  0x0000800000000000ULL
#define SYSCALL_MAX_LEN  65536

static bool
uptr_valid(uint64_t addr, uint64_t len)
{
    return addr < USER_ADDR_LIMIT && len <= USER_ADDR_LIMIT - addr;
}

/* ── Syscall implementations (uniform 4-arg signature) ───────────────────── */

static int64_t
sys_exit(int64_t code, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a1; (void)a2; (void)a3;
    sched_exit((int)code);
    return 0;
}

static int64_t
sys_write(int64_t fd, int64_t buf_addr, int64_t len, int64_t a3)
{
    (void)a3;
    if (len < 0) return -1;
    if (len > SYSCALL_MAX_LEN) len = SYSCALL_MAX_LEN;
    if (!uptr_valid((uint64_t)buf_addr, (uint64_t)len)) return -1;
    const char *buf = (const char *)buf_addr;

    if (fd == 1 || fd == 2) {
        /* stdout/stderr → console */
        for (int64_t i = 0; i < len; i++)
            console_putchar(buf[i]);
        return len;
    }

    /* File descriptor write. */
    return (int64_t)vfs_write((int)fd, buf, (size_t)len);
}

static int64_t
sys_read(int64_t fd, int64_t buf_addr, int64_t len, int64_t a3)
{
    (void)a3;
    if (len <= 0) return -1;
    if (len > SYSCALL_MAX_LEN) len = SYSCALL_MAX_LEN;
    if (!uptr_valid((uint64_t)buf_addr, (uint64_t)len)) return -1;
    char *buf = (char *)buf_addr;

    if (fd == 0) {
        /* stdin → keyboard */
        uint16_t key = keyboard_getchar();
        if (key < 128) {
            buf[0] = (char)key;
            return 1;
        }
        return 0;
    }

    /* File descriptor read. */
    return (int64_t)vfs_read((int)fd, buf, (size_t)len);
}

static int64_t
sys_yield(int64_t a0, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    sched_yield();
    return 0;
}

static int64_t
sys_getpid(int64_t a0, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    return (int64_t)sched_current()->id;
}

static int64_t
sys_sleep(int64_t ticks, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a1; (void)a2; (void)a3;
    if (ticks > 0)
        sched_sleep((uint64_t)ticks);
    return 0;
}

static int64_t
sys_open(int64_t path_addr, int64_t create, int64_t a2, int64_t a3)
{
    (void)a2; (void)a3;
    if (!uptr_valid((uint64_t)path_addr, 1)) return -1;
    return (int64_t)vfs_open((const char *)path_addr, (bool)create);
}

static int64_t
sys_close(int64_t fd, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a1; (void)a2; (void)a3;
    return (int64_t)vfs_close((int)fd);
}

static int64_t
sys_stat(int64_t path_addr, int64_t buf_addr, int64_t a2, int64_t a3)
{
    (void)a2; (void)a3;
    if (!uptr_valid((uint64_t)path_addr, 1)) return -1;
    if (!uptr_valid((uint64_t)buf_addr, sizeof(VfsStat))) return -1;
    return (int64_t)vfs_stat((const char *)path_addr, (VfsStat *)buf_addr);
}

static int64_t
sys_getppid(int64_t a0, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    return (int64_t)sched_current()->parent_id;
}

static int64_t
sys_waitpid(int64_t pid, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a1; (void)a2; (void)a3;
    if (pid <= 0) return -1;
    return (int64_t)sched_wait((uint64_t)pid);
}

static int64_t
sys_kill(int64_t pid, int64_t sig, int64_t a2, int64_t a3)
{
    (void)a2; (void)a3;
    if (pid <= 0) return -1;
    return (int64_t)sched_send_signal((uint64_t)pid, (int)sig);
}

/* ── Dispatch table ──────────────────────────────────────────────────────── */

typedef int64_t (*syscall_fn_t)(int64_t, int64_t, int64_t, int64_t);

static syscall_fn_t syscall_table[SYS_MAX];

int64_t
syscall_dispatch(uint64_t num, int64_t a0, int64_t a1, int64_t a2, int64_t a3)
{
    if (num >= SYS_MAX || !syscall_table[num])
        return -1;
    return syscall_table[num](a0, a1, a2, a3);
}

/* ── INT 0x80 handler ────────────────────────────────────────────────────── */

static void
int80_handler(InterruptFrame *frame)
{
    frame->rax = (uint64_t)syscall_dispatch(
        frame->rax,
        (int64_t)frame->rdi,
        (int64_t)frame->rsi,
        (int64_t)frame->rdx,
        (int64_t)frame->r10
    );
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void
syscall_init(void)
{
    syscall_table[SYS_EXIT]      = sys_exit;
    syscall_table[SYS_WRITE]     = sys_write;
    syscall_table[SYS_READ]      = sys_read;
    syscall_table[SYS_YIELD]     = sys_yield;
    syscall_table[SYS_GETPID]    = sys_getpid;
    syscall_table[SYS_SLEEP]     = sys_sleep;
    syscall_table[SYS_OPEN]      = sys_open;
    syscall_table[SYS_CLOSE]     = sys_close;
    syscall_table[SYS_STAT]      = sys_stat;
    syscall_table[SYS_GETPPID]   = sys_getppid;
    syscall_table[SYS_WAITPID]   = sys_waitpid;
    syscall_table[SYS_KILL]      = sys_kill;

    /* Register INT 0x80 handler (DPL 3 gate set in idt_init). */
    idt_register_handler(0x80, int80_handler);
}
