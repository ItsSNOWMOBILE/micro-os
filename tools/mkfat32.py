#!/usr/bin/env python3
"""
mkfat32.py -- Create a small FAT32 disk image with test files.

Usage: python3 tools/mkfat32.py <output.img>
"""

import struct
import sys
import os

SECTOR_SIZE = 512
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 32
NUM_FATS = 2
TOTAL_SECTORS = 2048  # 1 MiB image
FAT_SIZE_SECTORS = 16  # sectors per FAT

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output.img>", file=sys.stderr)
        sys.exit(1)

    out_path = sys.argv[1]
    img = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    data_start = RESERVED_SECTORS + NUM_FATS * FAT_SIZE_SECTORS

    # ── BPB (Boot Parameter Block) ──────────────────────────────────────
    bpb = bytearray(SECTOR_SIZE)
    bpb[0:3] = b'\xEB\x58\x90'         # jmp short + nop
    bpb[3:11] = b'MICROOS '            # OEM name
    struct.pack_into('<H', bpb, 11, SECTOR_SIZE)
    bpb[13] = SECTORS_PER_CLUSTER
    struct.pack_into('<H', bpb, 14, RESERVED_SECTORS)
    bpb[16] = NUM_FATS
    struct.pack_into('<H', bpb, 17, 0)  # root entry count (0 for FAT32)
    struct.pack_into('<H', bpb, 19, 0)  # total sectors 16 (0 for FAT32)
    bpb[21] = 0xF8                       # media type (hard disk)
    struct.pack_into('<H', bpb, 22, 0)  # fat size 16 (0 for FAT32)
    struct.pack_into('<H', bpb, 24, 63) # sectors per track
    struct.pack_into('<H', bpb, 26, 255) # heads
    struct.pack_into('<I', bpb, 28, 0)  # hidden sectors
    struct.pack_into('<I', bpb, 32, TOTAL_SECTORS)
    # FAT32 extended fields
    struct.pack_into('<I', bpb, 36, FAT_SIZE_SECTORS)
    struct.pack_into('<H', bpb, 40, 0)  # ext flags
    struct.pack_into('<H', bpb, 42, 0)  # FS version
    struct.pack_into('<I', bpb, 44, 2)  # root cluster
    struct.pack_into('<H', bpb, 48, 1)  # FSInfo sector
    struct.pack_into('<H', bpb, 50, 6)  # backup boot sector
    bpb[66] = 0x29                       # boot signature
    struct.pack_into('<I', bpb, 67, 0x12345678)  # volume serial
    bpb[71:82] = b'MICRO-OS   '        # volume label
    bpb[82:90] = b'FAT32   '           # FS type
    bpb[510] = 0x55
    bpb[511] = 0xAA

    img[0:SECTOR_SIZE] = bpb

    # ── FAT ─────────────────────────────────────────────────────────────
    fat_offset = RESERVED_SECTORS * SECTOR_SIZE

    def write_fat_entry(cluster, value):
        for f in range(NUM_FATS):
            off = fat_offset + f * FAT_SIZE_SECTORS * SECTOR_SIZE + cluster * 4
            struct.pack_into('<I', img, off, value)

    # FAT[0] = media type, FAT[1] = EOC marker
    write_fat_entry(0, 0x0FFFFFF8)
    write_fat_entry(1, 0x0FFFFFFF)

    next_free_cluster = 2
    data_offset = data_start * SECTOR_SIZE

    def alloc_cluster():
        nonlocal next_free_cluster
        c = next_free_cluster
        next_free_cluster += 1
        write_fat_entry(c, 0x0FFFFFFF)  # mark as EOC
        return c

    def cluster_offset(cluster):
        return data_offset + (cluster - 2) * SECTORS_PER_CLUSTER * SECTOR_SIZE

    def write_83_entry(parent_off, slot, name83, attr, cluster, size):
        off = parent_off + slot * 32
        # Pad name to 11 chars
        n = name83.encode('ascii')
        if len(n) < 11:
            n = n + b' ' * (11 - len(n))
        img[off:off+11] = n[:11]
        img[off+11] = attr
        struct.pack_into('<H', img, off + 20, cluster >> 16)
        struct.pack_into('<H', img, off + 26, cluster & 0xFFFF)
        struct.pack_into('<I', img, off + 28, size)

    def write_lfn_entry(parent_off, slot, order, name_part, checksum):
        off = parent_off + slot * 32
        img[off] = order
        img[off + 11] = 0x0F  # LFN attribute
        img[off + 12] = 0x00  # type
        img[off + 13] = checksum

        # Encode name_part as UCS-2, pad with 0xFFFF
        chars = []
        for ch in name_part:
            chars.append(ord(ch))
        while len(chars) < 13:
            chars.append(0xFFFF)

        # name1: 5 chars at offsets 1,3,5,7,9
        for i in range(5):
            struct.pack_into('<H', img, off + 1 + i * 2, chars[i])
        # name2: 6 chars at offset 14
        for i in range(6):
            struct.pack_into('<H', img, off + 14 + i * 2, chars[5 + i])
        # name3: 2 chars at offset 28
        for i in range(2):
            struct.pack_into('<H', img, off + 28 + i * 2, chars[11 + i])

    def lfn_checksum(name83):
        s = 0
        for ch in name83.encode('ascii').ljust(11)[:11]:
            s = ((s >> 1) + ((s & 1) << 7) + ch) & 0xFF
        return s

    def write_file_entry(parent_off, slot_ptr, long_name, short_name, attr, cluster, size):
        """Write LFN + 8.3 directory entries. Returns next slot."""
        slot = slot_ptr[0]
        name83 = short_name.ljust(11)[:11]
        chk = lfn_checksum(name83)

        # Split long name into 13-char chunks
        chunks = []
        for i in range(0, len(long_name), 13):
            chunks.append(long_name[i:i+13])

        # Write LFN entries in reverse order
        for i in range(len(chunks) - 1, -1, -1):
            order = i + 1
            if i == len(chunks) - 1:
                order |= 0x40  # last LFN entry flag
            write_lfn_entry(parent_off, slot, order, chunks[i], chk)
            slot += 1

        # Write the 8.3 entry
        write_83_entry(parent_off, slot, name83, attr, cluster, size)
        slot += 1
        slot_ptr[0] = slot

    # ── Root directory (cluster 2) ──────────────────────────────────────
    root_cluster = alloc_cluster()  # cluster 2
    root_off = cluster_offset(root_cluster)

    # Volume label entry
    slot = [0]
    write_83_entry(root_off, slot[0], "MICRO-OS   ", 0x08, 0, 0)
    slot[0] = 1

    # Create /hello.txt
    hello_data = b"Hello from the FAT32 filesystem!\n"
    hello_cluster = alloc_cluster()
    co = cluster_offset(hello_cluster)
    img[co:co+len(hello_data)] = hello_data
    write_file_entry(root_off, slot, "hello.txt", "HELLO   TXT", 0x20,
                     hello_cluster, len(hello_data))

    # Create /readme.txt
    readme_data = b"micro-os FAT32 test volume.\nThis file lives on a real FAT32 disk image.\n"
    readme_cluster = alloc_cluster()
    co = cluster_offset(readme_cluster)
    img[co:co+len(readme_data)] = readme_data
    write_file_entry(root_off, slot, "readme.txt", "README  TXT", 0x20,
                     readme_cluster, len(readme_data))

    # Create /docs directory
    docs_cluster = alloc_cluster()
    write_file_entry(root_off, slot, "docs", "DOCS       ", 0x10,
                     docs_cluster, 0)

    # Populate /docs with . and .. entries
    docs_off = cluster_offset(docs_cluster)
    dslot = [0]
    write_83_entry(docs_off, dslot[0], ".          ", 0x10, docs_cluster, 0)
    dslot[0] = 1
    write_83_entry(docs_off, dslot[0], "..         ", 0x10, root_cluster, 0)
    dslot[0] = 2

    # Create /docs/notes.txt
    notes_data = b"These are some notes stored in the docs folder.\n"
    notes_cluster = alloc_cluster()
    co = cluster_offset(notes_cluster)
    img[co:co+len(notes_data)] = notes_data
    write_file_entry(docs_off, dslot, "notes.txt", "NOTES   TXT", 0x20,
                     notes_cluster, len(notes_data))

    # ── Write image ─────────────────────────────────────────────────────
    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(img)

    print(f"Created {out_path} ({len(img)} bytes, FAT32)")
    print(f"  Files: /hello.txt, /readme.txt, /docs/notes.txt")

if __name__ == '__main__':
    main()
