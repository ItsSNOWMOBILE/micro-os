/*
 * fat32.h -- FAT32 filesystem reader.
 */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum path component length. */
#define FAT32_MAX_NAME  256

/* Directory entry types. */
#define FAT32_TYPE_FILE  0
#define FAT32_TYPE_DIR   1

typedef struct {
    char     name[FAT32_MAX_NAME];
    uint8_t  type;       /* FAT32_TYPE_FILE or FAT32_TYPE_DIR */
    uint32_t size;       /* file size in bytes */
    uint32_t cluster;    /* first cluster */
} Fat32DirEntry;

/* Initialise FAT32 from the specified ATA drive.
 * Returns 0 on success, -1 on failure. */
int fat32_init(int drive);

/* Returns true if FAT32 is mounted. */
bool fat32_mounted(void);

/* List directory contents at the given path.
 * Calls cb(entry, ctx) for each entry. Returns 0 on success. */
int fat32_readdir(const char *path,
                  void (*cb)(const Fat32DirEntry *entry, void *ctx),
                  void *ctx);

/* Read a file into buf (up to buf_size bytes).
 * Returns bytes read, or -1 on error. */
int fat32_read_file(const char *path, void *buf, uint32_t buf_size);

/* Get file/dir info. Returns 0 on success. */
int fat32_stat(const char *path, Fat32DirEntry *out);

#endif /* FAT32_H */
