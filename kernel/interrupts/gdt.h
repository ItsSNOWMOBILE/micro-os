/*
 * gdt.h — Global Descriptor Table.
 *
 * GDT layout (required for SYSCALL/SYSRET):
 *   [0] Null
 *   [1] Kernel code  (0x08)
 *   [2] Kernel data  (0x10)
 *   [3] User data    (0x18)  — must be before user code for SYSRET
 *   [4] User code    (0x20)
 *   [5-6] TSS        (0x28)
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_init(void);
void gdt_set_rsp0(uint64_t rsp0);

/* Segment selectors (byte offsets into the GDT). */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x18
#define GDT_USER_CODE    0x20
#define GDT_TSS          0x28

/* Selectors with RPL 3 for user mode. */
#define GDT_USER_CODE_RPL3  (GDT_USER_CODE | 3)
#define GDT_USER_DATA_RPL3  (GDT_USER_DATA | 3)

#endif /* GDT_H */
