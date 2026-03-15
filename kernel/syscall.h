/*
 * syscall.h — System call interface.
 *
 * Syscall numbers and the init function.  User programs invoke these
 * via the SYSCALL instruction (or INT 0x80 as fallback).
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
#define SYS_MAX      10

void syscall_init(void);

#endif /* SYSCALL_H */
