/*
 * syscall.h — System call interface.
 *
 * Syscall numbers and the init function.  User programs invoke these
 * via INT 0x80.
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Syscall numbers. */
#define SYS_EXIT     0
#define SYS_WRITE    1
#define SYS_READ     2
#define SYS_YIELD    3
#define SYS_GETPID   4
#define SYS_SLEEP    5
#define SYS_OPEN     6
#define SYS_CLOSE    7
#define SYS_STAT     8
#define SYS_SBRK     9
#define SYS_GETPPID  10
#define SYS_WAITPID  11
#define SYS_KILL     12
#define SYS_SIGRETURN 13
#define SYS_MAX      14

/* Signal numbers (subset of POSIX). */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL   9
#define SIGSEGV  11
#define SIGTERM  15
#define SIGCHLD  17
#define SIGSTOP  19
#define SIG_MAX  20

/* Special signal handler values. */
#define SIG_DFL  ((uint64_t)0)
#define SIG_IGN  ((uint64_t)1)

void syscall_init(void);

#endif /* SYSCALL_H */
