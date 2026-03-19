/*
 * elf.h — ELF64 binary format definitions and loader.
 */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include "sched/task.h"

/* ── ELF64 header structures ────────────────────────────────────────────── */

#define ELF_MAGIC  0x464C457F  /* "\x7FELF" as a 32-bit LE integer */

#define ET_EXEC    2
#define EM_X86_64  62
#define PT_LOAD    1

#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4

typedef struct {
    uint32_t e_ident_magic;     /* 0x7F 'E' 'L' 'F' */
    uint8_t  e_ident_class;     /* 2 = 64-bit */
    uint8_t  e_ident_data;      /* 1 = little-endian */
    uint8_t  e_ident_version;
    uint8_t  e_ident_osabi;
    uint8_t  e_ident_pad[8];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;           /* program header table offset */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;           /* number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;          /* offset in file */
    uint64_t p_vaddr;           /* virtual address */
    uint64_t p_paddr;
    uint64_t p_filesz;          /* bytes in file */
    uint64_t p_memsz;           /* bytes in memory (>= filesz; excess zeroed) */
    uint64_t p_align;
} Elf64_Phdr;

/* Load an ELF64 binary from the VFS path into a new user-mode task.
 * Returns the Task pointer, or NULL on failure. */
Task *elf_load(const char *path, const char *name);

#endif /* ELF_H */
