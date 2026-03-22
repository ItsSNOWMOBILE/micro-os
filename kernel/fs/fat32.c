/*
 * fat32.c -- FAT32 filesystem reader.
 *
 * Read-only FAT32 with long filename support.  Reads sectors via
 * the ATA PIO driver.  Supports traversing directories, reading
 * files, and stat.
 */

#include "fat32.h"
#include "../drivers/ata.h"
#include "../kernel.h"
#include "../console.h"
#include "../string.h"
#include "../mm/heap.h"

/* ── On-disk structures ────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;     /* 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;          /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32-specific fields. */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} Fat32BPB;

typedef struct __attribute__((packed)) {
    char     name[11];             /* 8.3 name */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} Fat32DirEnt;

typedef struct __attribute__((packed)) {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;                 /* always 0x0F */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} Fat32LfnEntry;

/* Directory entry attributes. */
#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F

#define FAT32_EOC       0x0FFFFFF8

/* ── Mount state ───────────────────────────────────────────────────────── */

static bool     mounted;
static int      ata_drive;
static uint32_t bytes_per_sector;
static uint32_t sectors_per_cluster;
static uint32_t reserved_sectors;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;
static uint32_t root_cluster;
static uint32_t num_fats;
static uint32_t fat_size;

/* ── Sector I/O ────────────────────────────────────────────────────────── */

static uint8_t sector_buf[512];

static int
read_sector(uint32_t lba, void *buf)
{
    return ata_read_sectors(ata_drive, lba, 1, buf);
}

/* ── FAT lookup ────────────────────────────────────────────────────────── */

static uint32_t
next_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_lba + (fat_offset / bytes_per_sector);
    uint32_t ent_offset = fat_offset % bytes_per_sector;

    if (read_sector(fat_sector, sector_buf) < 0)
        return FAT32_EOC;

    uint32_t val;
    memcpy(&val, &sector_buf[ent_offset], 4);
    return val & 0x0FFFFFFF;
}

static uint32_t
cluster_to_lba(uint32_t cluster)
{
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

/* ── LFN assembly ──────────────────────────────────────────────────────── */

static char lfn_buf[FAT32_MAX_NAME];
static int  lfn_len;
static bool lfn_valid;

static void
lfn_reset(void)
{
    lfn_len = 0;
    lfn_valid = false;
    memset(lfn_buf, 0, sizeof(lfn_buf));
}

static void
lfn_append(const Fat32LfnEntry *lfn)
{
    /* LFN entries come in reverse order. Each holds 13 UCS-2 chars. */
    int seq = (lfn->order & 0x3F) - 1;
    int base = seq * 13;

    uint16_t chars[13];
    for (int i = 0; i < 5; i++) chars[i]     = lfn->name1[i];
    for (int i = 0; i < 6; i++) chars[5 + i]  = lfn->name2[i];
    for (int i = 0; i < 2; i++) chars[11 + i] = lfn->name3[i];

    for (int i = 0; i < 13; i++) {
        int pos = base + i;
        if (pos >= FAT32_MAX_NAME - 1) break;
        if (chars[i] == 0x0000 || chars[i] == 0xFFFF) {
            if (pos > lfn_len) lfn_len = pos;
            continue;
        }
        /* Simple UCS-2 to ASCII (non-ASCII becomes '?'). */
        lfn_buf[pos] = (chars[i] < 128) ? (char)chars[i] : '?';
        if (pos + 1 > lfn_len) lfn_len = pos + 1;
    }

    lfn_valid = true;
}

/* ── 8.3 name conversion ──────────────────────────────────────────────── */

static void
short_name_to_str(const char *name83, char *out)
{
    int i = 0, o = 0;

    /* Base name (first 8 chars, trim trailing spaces). */
    for (i = 0; i < 8 && name83[i] != ' '; i++)
        out[o++] = (name83[i] >= 'A' && name83[i] <= 'Z')
                   ? name83[i] + 32 : name83[i];

    /* Extension (next 3 chars). */
    if (name83[8] != ' ') {
        out[o++] = '.';
        for (i = 8; i < 11 && name83[i] != ' '; i++)
            out[o++] = (name83[i] >= 'A' && name83[i] <= 'Z')
                       ? name83[i] + 32 : name83[i];
    }

    out[o] = '\0';
}

/* ── Directory iteration ───────────────────────────────────────────────── */

typedef void (*dir_cb)(const Fat32DirEnt *ent, const char *name, void *ctx);

static int
iterate_dir(uint32_t cluster, dir_cb cb, void *ctx)
{
    uint8_t *cluster_buf = kmalloc(sectors_per_cluster * bytes_per_sector);
    if (!cluster_buf) return -1;

    lfn_reset();

    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (read_sector(lba + s, cluster_buf + s * bytes_per_sector) < 0) {
                kfree(cluster_buf);
                return -1;
            }
        }

        uint32_t entries_per_cluster =
            (sectors_per_cluster * bytes_per_sector) / 32;

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            Fat32DirEnt *ent = (Fat32DirEnt *)(cluster_buf + i * 32);

            if (ent->name[0] == 0x00) {
                kfree(cluster_buf);
                return 0;  /* end of directory */
            }
            if ((uint8_t)ent->name[0] == 0xE5)
                continue;  /* deleted entry */

            if (ent->attr == ATTR_LFN) {
                lfn_append((Fat32LfnEntry *)ent);
                continue;
            }

            if (ent->attr & ATTR_VOLUME_ID) {
                lfn_reset();
                continue;
            }

            char name[FAT32_MAX_NAME];
            if (lfn_valid) {
                lfn_buf[lfn_len] = '\0';
                memcpy(name, lfn_buf, lfn_len + 1);
            } else {
                short_name_to_str(ent->name, name);
            }

            cb(ent, name, ctx);
            lfn_reset();
        }

        cluster = next_cluster(cluster);
    }

    kfree(cluster_buf);
    return 0;
}

/* ── Path resolution ───────────────────────────────────────────────────── */

struct find_ctx {
    const char *target;
    Fat32DirEnt result;
    bool found;
};

static int
strcasecmp_simple(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void
find_cb(const Fat32DirEnt *ent, const char *name, void *ctx)
{
    struct find_ctx *fc = ctx;
    if (strcasecmp_simple(name, fc->target) == 0) {
        fc->result = *ent;
        fc->found = true;
    }
}

/* Resolve a path to a directory entry.  Returns 0 on success. */
static int
resolve_path(const char *path, Fat32DirEnt *out)
{
    if (!path || !*path || (path[0] == '/' && path[1] == '\0')) {
        /* Root directory. */
        memset(out, 0, sizeof(*out));
        out->attr = ATTR_DIRECTORY;
        out->first_cluster_hi = (uint16_t)(root_cluster >> 16);
        out->first_cluster_lo = (uint16_t)(root_cluster);
        return 0;
    }

    /* Skip leading slash. */
    if (*path == '/') path++;

    uint32_t current_cluster = root_cluster;

    while (*path) {
        /* Extract next component. */
        char component[FAT32_MAX_NAME];
        int len = 0;
        while (*path && *path != '/' && len < FAT32_MAX_NAME - 1)
            component[len++] = *path++;
        component[len] = '\0';
        if (*path == '/') path++;

        /* Search current directory for this component. */
        struct find_ctx fc;
        fc.target = component;
        fc.found = false;

        if (iterate_dir(current_cluster, find_cb, &fc) < 0)
            return -1;

        if (!fc.found)
            return -1;

        /* If more path remains, this must be a directory. */
        if (*path && !(fc.result.attr & ATTR_DIRECTORY))
            return -1;

        *out = fc.result;
        current_cluster = ((uint32_t)fc.result.first_cluster_hi << 16) |
                          fc.result.first_cluster_lo;
    }

    return 0;
}

static uint32_t
entry_cluster(const Fat32DirEnt *ent)
{
    return ((uint32_t)ent->first_cluster_hi << 16) | ent->first_cluster_lo;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int
fat32_init(int drive)
{
    if (!ata_drive_present(drive)) {
        kprintf("FAT32: drive %d not present\n", drive);
        return -1;
    }

    /* Read the BPB (boot sector) into a full 512-byte buffer. */
    uint8_t boot_sector[512];
    if (ata_read_sectors(drive, 0, 1, boot_sector) < 0) {
        kprintf("FAT32: failed to read boot sector\n");
        return -1;
    }
    Fat32BPB *bpb = (Fat32BPB *)boot_sector;

    /* Validate. */
    if (bpb->bytes_per_sector != 512) {
        kprintf("FAT32: unsupported sector size %d\n", bpb->bytes_per_sector);
        return -1;
    }
    if (bpb->fat_size_32 == 0) {
        kprintf("FAT32: not a FAT32 volume\n");
        return -1;
    }

    ata_drive           = drive;
    bytes_per_sector    = bpb->bytes_per_sector;
    sectors_per_cluster = bpb->sectors_per_cluster;
    reserved_sectors    = bpb->reserved_sectors;
    num_fats            = bpb->num_fats;
    fat_size            = bpb->fat_size_32;
    root_cluster        = bpb->root_cluster;

    fat_start_lba  = reserved_sectors;
    data_start_lba = reserved_sectors + num_fats * fat_size;

    mounted = true;

    char label[12];
    memcpy(label, bpb->volume_label, 11);
    label[11] = '\0';
    /* Trim trailing spaces. */
    for (int i = 10; i >= 0 && label[i] == ' '; i--)
        label[i] = '\0';

    kprintf("FAT32: mounted '%s' (%lu sectors/cluster)\n",
            label, (uint64_t)sectors_per_cluster);

    return 0;
}

bool
fat32_mounted(void)
{
    return mounted;
}

struct readdir_ctx {
    void (*cb)(const Fat32DirEntry *entry, void *ctx);
    void *user_ctx;
};

static void
readdir_adapter(const Fat32DirEnt *ent, const char *name, void *ctx)
{
    struct readdir_ctx *rc = ctx;
    Fat32DirEntry entry;
    memset(&entry, 0, sizeof(entry));

    int len = 0;
    while (name[len] && len < FAT32_MAX_NAME - 1) {
        entry.name[len] = name[len];
        len++;
    }
    entry.name[len] = '\0';
    entry.type = (ent->attr & ATTR_DIRECTORY) ? FAT32_TYPE_DIR : FAT32_TYPE_FILE;
    entry.size = ent->file_size;
    entry.cluster = entry_cluster(ent);

    rc->cb(&entry, rc->user_ctx);
}

int
fat32_readdir(const char *path,
              void (*cb)(const Fat32DirEntry *entry, void *ctx),
              void *ctx)
{
    if (!mounted) return -1;

    Fat32DirEnt ent;
    if (resolve_path(path, &ent) < 0)
        return -1;
    if (!(ent.attr & ATTR_DIRECTORY))
        return -1;

    uint32_t cluster = entry_cluster(&ent);
    if (cluster == 0) cluster = root_cluster;

    struct readdir_ctx rc = { .cb = cb, .user_ctx = ctx };
    return iterate_dir(cluster, readdir_adapter, &rc);
}

int
fat32_read_file(const char *path, void *buf, uint32_t buf_size)
{
    if (!mounted) return -1;

    Fat32DirEnt ent;
    if (resolve_path(path, &ent) < 0)
        return -1;
    if (ent.attr & ATTR_DIRECTORY)
        return -1;

    uint32_t size = ent.file_size;
    if (size > buf_size) size = buf_size;

    uint32_t cluster = entry_cluster(&ent);
    uint32_t cluster_size = sectors_per_cluster * bytes_per_sector;
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = size;

    uint8_t *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    while (remaining > 0 && cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (read_sector(lba + s, cluster_buf + s * bytes_per_sector) < 0) {
                kfree(cluster_buf);
                return -1;
            }
        }

        uint32_t to_copy = remaining > cluster_size ? cluster_size : remaining;
        memcpy(p, cluster_buf, to_copy);
        p += to_copy;
        remaining -= to_copy;

        cluster = next_cluster(cluster);
    }

    kfree(cluster_buf);
    return (int)size;
}

int
fat32_stat(const char *path, Fat32DirEntry *out)
{
    if (!mounted) return -1;

    Fat32DirEnt ent;
    if (resolve_path(path, &ent) < 0)
        return -1;

    memset(out, 0, sizeof(*out));
    short_name_to_str(ent.name, out->name);
    /* If the path was root, fix the name. */
    if (!path || !*path || (path[0] == '/' && path[1] == '\0'))
        strcpy(out->name, "/");
    out->type = (ent.attr & ATTR_DIRECTORY) ? FAT32_TYPE_DIR : FAT32_TYPE_FILE;
    out->size = ent.file_size;
    out->cluster = entry_cluster(&ent);
    return 0;
}
