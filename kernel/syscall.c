/*
 * syscall.c — System call dispatcher.
 *
 * Sets up the SYSCALL/SYSRET MSRs and handles the INT 0x80 fallback.
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

/* ── MSR definitions ─────────────────────────────────────────────────────── */

#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_SFMASK  0xC0000084

#define EFER_SCE    (1ULL << 0)

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

extern void syscall_entry(void);

/* ── Syscall implementations (uniform 4-arg signature) ───────────────────── */

static int64_t
sys_exit(int64_t code, int64_t a1, int64_t a2, int64_t a3)
{
    (void)a1; (void)a2; (void)a3;
    (void)code;
    sched_current()->state = TASK_DEAD;
    sched_yield();
    return 0;
}

static int64_t
sys_write(int64_t fd, int64_t buf_addr, int64_t len, int64_t a3)
{
    (void)fd; (void)a3;
    const char *buf = (const char *)buf_addr;
    if (len < 0) return -1;
    for (int64_t i = 0; i < len; i++)
        console_putchar(buf[i]);
    return len;
}

static int64_t
sys_read(int64_t fd, int64_t buf_addr, int64_t len, int64_t a3)
{
    (void)fd; (void)a3;
    char *buf = (char *)buf_addr;
    if (len <= 0) return -1;
    uint16_t key = keyboard_getchar();
    if (key < 128) {
        buf[0] = (char)key;
        return 1;
    }
    return 0;  /* special keys not returned to userspace */
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
    syscall_table[SYS_EXIT]   = sys_exit;
    syscall_table[SYS_WRITE]  = sys_write;
    syscall_table[SYS_READ]   = sys_read;
    syscall_table[SYS_YIELD]  = sys_yield;
    syscall_table[SYS_GETPID] = sys_getpid;
    syscall_table[SYS_SLEEP]  = sys_sleep;

    /* Register INT 0x80 handler. */
    idt_register_handler(0x80, int80_handler);

    /* SYSCALL/SYSRET MSRs — disabled for now (no user-mode tasks yet).
     * Enable when user_task_create is actually used. */
#if 0
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);
#endif
}
