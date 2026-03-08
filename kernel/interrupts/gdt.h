/*
 * gdt.h — Global Descriptor Table.
 *
 * In 64-bit long mode the GDT is mostly vestigial (segmentation is
 * disabled), but the CPU still requires valid code and data segment
 * descriptors, and the TSS descriptor is needed for IST (Interrupt
 * Stack Table) entries used by the IDT.
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_init(void);

/* Segment selectors (byte offsets into the GDT). */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x18
#define GDT_USER_DATA    0x20
#define GDT_TSS          0x28

#endif /* GDT_H */
