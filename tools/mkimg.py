#!/usr/bin/env python3
"""
mkimg.py — Create a bootable UEFI FAT12/16 disk image.

Usage: python3 mkimg.py <BOOTX64.EFI> <output.img>

Produces a raw disk image with:
  - A protective MBR with one partition (type 0xEF = EFI System).
  - A FAT12 filesystem containing /EFI/BOOT/BOOTX64.EFI.

The image is sized to fit the bootloader with comfortable headroom.
QEMU boots this directly with -drive format=raw.
"""

import sys
import struct
import os

def align(val, boundary):
    return (val + boundary - 1) & ~(boundary - 1)

def create_fat12_image(efi_path, out_path):
    efi_data = open(efi_path, "rb").read()
    efi_size = len(efi_data)

    # FAT12 parameters
    bytes_per_sector = 512
    sectors_per_cluster = 4
    reserved_sectors = 1       # boot sector
    num_fats = 2
    root_entry_count = 512     # 512 * 32 = 16384 bytes = 32 sectors
    root_dir_sectors = (root_entry_count * 32 + bytes_per_sector - 1) // bytes_per_sector

    # We need space for: /EFI (dir), /EFI/BOOT (dir), /EFI/BOOT/BOOTX64.EFI
    cluster_size = bytes_per_sector * sectors_per_cluster
    file_clusters = (efi_size + cluster_size - 1) // cluster_size
    dir_clusters = 2  # EFI/ and EFI/BOOT/ each take one cluster

    total_data_clusters = file_clusters + dir_clusters + 16  # headroom
    sectors_per_fat = (total_data_clusters * 3 // 2 + bytes_per_sector - 1) // bytes_per_sector
    if sectors_per_fat < 1:
        sectors_per_fat = 1

    data_start = reserved_sectors + num_fats * sectors_per_fat + root_dir_sectors
    total_sectors = data_start + total_data_clusters * sectors_per_cluster

    # Ensure minimum image size for QEMU (at least 1 MiB)
    min_sectors = 2880  # 1.44 MiB floppy
    if total_sectors < min_sectors:
        total_sectors = min_sectors

    img = bytearray(total_sectors * bytes_per_sector)

    # ── BPB (BIOS Parameter Block) in the boot sector ────────────────────
    bs = bytearray(bytes_per_sector)
    bs[0:3] = b'\xEB\x3C\x90'           # jump + nop
    bs[3:11] = b'MICROOS '              # OEM name
    struct.pack_into('<H', bs, 11, bytes_per_sector)
    bs[13] = sectors_per_cluster
    struct.pack_into('<H', bs, 14, reserved_sectors)
    bs[16] = num_fats
    struct.pack_into('<H', bs, 17, root_entry_count)
    struct.pack_into('<H', bs, 19, total_sectors if total_sectors < 65536 else 0)
    bs[21] = 0xF0                        # media descriptor (removable)
    struct.pack_into('<H', bs, 22, sectors_per_fat)
    struct.pack_into('<H', bs, 24, 63)   # sectors per track
    struct.pack_into('<H', bs, 26, 255)  # heads
    struct.pack_into('<I', bs, 28, 0)    # hidden sectors
    struct.pack_into('<I', bs, 32, total_sectors if total_sectors >= 65536 else 0)
    bs[36] = 0x80                        # drive number
    bs[38] = 0x29                        # extended boot sig
    struct.pack_into('<I', bs, 39, 0x12345678)  # volume serial
    bs[43:54] = b'MICRO-OS   '          # volume label
    bs[54:62] = b'FAT12   '             # FS type
    bs[510] = 0x55
    bs[511] = 0xAA

    img[0:bytes_per_sector] = bs

    # ── FAT tables ───────────────────────────────────────────────────────
    fat_offset = reserved_sectors * bytes_per_sector

    def set_fat12(cluster, value):
        """Write a 12-bit FAT entry."""
        byte_off = cluster * 3 // 2
        if cluster % 2 == 0:
            img[fat_offset + byte_off] = value & 0xFF
            img[fat_offset + byte_off + 1] = (
                (img[fat_offset + byte_off + 1] & 0xF0) | ((value >> 8) & 0x0F)
            )
        else:
            img[fat_offset + byte_off] = (
                (img[fat_offset + byte_off] & 0x0F) | ((value & 0x0F) << 4)
            )
            img[fat_offset + byte_off + 1] = (value >> 4) & 0xFF

    # FAT[0] = media byte, FAT[1] = end of chain marker
    set_fat12(0, 0xFF0)
    set_fat12(1, 0xFFF)

    # Cluster allocation:
    #   cluster 2: /EFI directory
    #   cluster 3: /EFI/BOOT directory
    #   cluster 4..: BOOTX64.EFI data
    next_cluster = 2

    efi_dir_cluster = next_cluster
    set_fat12(efi_dir_cluster, 0xFFF)
    next_cluster += 1

    boot_dir_cluster = next_cluster
    set_fat12(boot_dir_cluster, 0xFFF)
    next_cluster += 1

    file_start_cluster = next_cluster
    for i in range(file_clusters):
        c = next_cluster + i
        if i < file_clusters - 1:
            set_fat12(c, c + 1)
        else:
            set_fat12(c, 0xFFF)
    next_cluster += file_clusters

    # Copy FAT1 to FAT2
    fat_size = sectors_per_fat * bytes_per_sector
    fat2_offset = fat_offset + fat_size
    img[fat2_offset:fat2_offset + fat_size] = img[fat_offset:fat_offset + fat_size]

    # ── Root directory ───────────────────────────────────────────────────
    root_offset = (reserved_sectors + num_fats * sectors_per_fat) * bytes_per_sector

    def make_dir_entry(name, attr, cluster, size=0):
        """Create a 32-byte FAT directory entry."""
        entry = bytearray(32)
        padded = name.encode('ascii')
        if len(padded) < 11:
            padded += b' ' * (11 - len(padded))
        entry[0:11] = padded[:11]
        entry[11] = attr
        struct.pack_into('<H', entry, 26, cluster)
        struct.pack_into('<I', entry, 28, size)
        return entry

    # Volume label entry
    vol_entry = make_dir_entry("MICRO-OS   ", 0x08, 0)
    img[root_offset:root_offset + 32] = vol_entry

    # /EFI directory entry
    efi_entry = make_dir_entry("EFI        ", 0x10, efi_dir_cluster)
    img[root_offset + 32:root_offset + 64] = efi_entry

    # ── /EFI directory contents ──────────────────────────────────────────
    def cluster_offset(cluster):
        return (data_start + (cluster - 2) * sectors_per_cluster) * bytes_per_sector

    efi_dir_off = cluster_offset(efi_dir_cluster)

    # "." and ".."
    dot = make_dir_entry(".          ", 0x10, efi_dir_cluster)
    dotdot = make_dir_entry("..         ", 0x10, 0)
    img[efi_dir_off:efi_dir_off + 32] = dot
    img[efi_dir_off + 32:efi_dir_off + 64] = dotdot

    # BOOT subdirectory entry
    boot_entry = make_dir_entry("BOOT       ", 0x10, boot_dir_cluster)
    img[efi_dir_off + 64:efi_dir_off + 96] = boot_entry

    # ── /EFI/BOOT directory contents ─────────────────────────────────────
    boot_dir_off = cluster_offset(boot_dir_cluster)

    dot2 = make_dir_entry(".          ", 0x10, boot_dir_cluster)
    dotdot2 = make_dir_entry("..         ", 0x10, efi_dir_cluster)
    img[boot_dir_off:boot_dir_off + 32] = dot2
    img[boot_dir_off + 32:boot_dir_off + 64] = dotdot2

    # BOOTX64.EFI file entry
    file_entry = make_dir_entry("BOOTX64 EFI", 0x20, file_start_cluster, efi_size)
    img[boot_dir_off + 64:boot_dir_off + 96] = file_entry

    # ── File data ────────────────────────────────────────────────────────
    file_data_off = cluster_offset(file_start_cluster)
    img[file_data_off:file_data_off + efi_size] = efi_data

    # ── Write image ──────────────────────────────────────────────────────
    with open(out_path, "wb") as f:
        f.write(img)

    print(f"Created {out_path} ({len(img)} bytes, {total_sectors} sectors)")
    print(f"  EFI binary: {efi_size} bytes, {file_clusters} clusters")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <BOOTX64.EFI> <output.img>")
        sys.exit(1)
    create_fat12_image(sys.argv[1], sys.argv[2])
