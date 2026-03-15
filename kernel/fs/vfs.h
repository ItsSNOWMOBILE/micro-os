/*
 * vfs.h — Virtual File System layer.
 *
 * Provides a simple unified interface for file operations.  The initial
 * implementation backs everything with an in-memory ramfs.
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VFS_MAX_NAME   64
#define VFS_MAX_FILES  128
#define VFS_MAX_FD     64
#define VFS_MAX_DATA   (4096 * 4)  /* 16 KiB max file size */

typedef enum {
    VFS_FILE,
    VFS_DIRECTORY,
} VNodeType;

typedef struct VNode {
    char      name[VFS_MAX_NAME];
    VNodeType type;
    uint8_t  *data;        /* file contents (NULL for directories) */
    size_t    size;         /* current file size */
    size_t    capacity;     /* allocated buffer size */
    int       parent;       /* index of parent directory (-1 for root) */
    int       children[32]; /* indices of children (for directories) */
    int       child_count;
    bool      used;
} VNode;

/* File descriptor state. */
typedef struct {
    int    vnode;     /* index into vnode table */
    size_t offset;    /* read/write position */
    bool   used;
} FileDesc;

/* Stat result. */
typedef struct {
    VNodeType type;
    size_t    size;
    char      name[VFS_MAX_NAME];
} VfsStat;

/* Init the VFS (creates root directory). */
void vfs_init(void);

/* File operations — return fd or bytes, -1 on error. */
int     vfs_open(const char *path, bool create);
int     vfs_close(int fd);
int     vfs_read(int fd, void *buf, size_t count);
int     vfs_write(int fd, const void *buf, size_t count);
int     vfs_stat(const char *path, VfsStat *out);

/* Directory operations. */
int     vfs_mkdir(const char *path);
int     vfs_readdir(const char *path, VfsStat *entries, int max_entries);

/* Remove a file or empty directory. */
int     vfs_unlink(const char *path);

/* Rename/move a file or directory. */
int     vfs_rename(const char *old_path, const char *new_path);

#endif /* VFS_H */
