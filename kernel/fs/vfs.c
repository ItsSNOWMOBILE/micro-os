/*
 * vfs.c — In-memory virtual filesystem (ramfs).
 *
 * A flat vnode table with tree structure via parent/children indices.
 * Paths are absolute, starting with '/'.
 */

#include "vfs.h"
#include "../string.h"
#include "../mm/heap.h"

static VNode    vnodes[VFS_MAX_FILES];
static FileDesc fds[VFS_MAX_FD];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int
alloc_vnode(void)
{
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!vnodes[i].used) {
            memset(&vnodes[i], 0, sizeof(VNode));
            vnodes[i].used = true;
            vnodes[i].parent = -1;
            return i;
        }
    }
    return -1;
}

static int
alloc_fd(void)
{
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!fds[i].used) {
            fds[i].used = true;
            fds[i].offset = 0;
            return i;
        }
    }
    return -1;
}

/*
 * Resolve a path to a vnode index.
 * Path must be absolute (start with '/').
 * Returns -1 if not found.
 */
static int
resolve_path(const char *path)
{
    if (!path || path[0] != '/')
        return -1;

    int current = 0;  /* root */

    /* Skip leading '/' */
    path++;

    if (*path == '\0')
        return 0;  /* root directory */

    while (*path) {
        /* Extract the next component. */
        char component[VFS_MAX_NAME];
        int len = 0;
        while (*path && *path != '/' && len < VFS_MAX_NAME - 1)
            component[len++] = *path++;
        component[len] = '\0';

        if (*path == '/')
            path++;

        if (len == 0)
            continue;

        /* Search children of current node. */
        VNode *dir = &vnodes[current];
        if (dir->type != VFS_DIRECTORY)
            return -1;

        int found = -1;
        for (int i = 0; i < dir->child_count; i++) {
            int child_idx = dir->children[i];
            if (strcmp(vnodes[child_idx].name, component) == 0) {
                found = child_idx;
                break;
            }
        }

        if (found == -1)
            return -1;

        current = found;
    }

    return current;
}

/*
 * Resolve the parent directory and extract the final component name.
 */
static int
resolve_parent(const char *path, char *name_out)
{
    if (!path || path[0] != '/')
        return -1;

    /* Find the last '/'. */
    const char *last_slash = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            last_slash = p;
    }

    /* Extract name after last slash. */
    const char *name = last_slash + 1;
    if (*name == '\0')
        return -1;

    int len = 0;
    while (name[len] && len < VFS_MAX_NAME - 1) {
        name_out[len] = name[len];
        len++;
    }
    name_out[len] = '\0';

    /* Resolve parent path. */
    if (last_slash == path) {
        /* Parent is root. */
        return 0;
    }

    /* Temporarily truncate to get parent path. */
    char parent_path[256];
    int plen = (int)(last_slash - path);
    if (plen >= 256) plen = 255;
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    return resolve_path(parent_path);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void
vfs_init(void)
{
    memset(vnodes, 0, sizeof(vnodes));
    memset(fds, 0, sizeof(fds));

    /* Create root directory at index 0. */
    vnodes[0].used = true;
    vnodes[0].type = VFS_DIRECTORY;
    vnodes[0].parent = -1;
    strcpy(vnodes[0].name, "/");
}

int
vfs_open(const char *path, bool create)
{
    int vnode_idx = resolve_path(path);

    if (vnode_idx == -1 && create) {
        /* Create the file. */
        char name[VFS_MAX_NAME];
        int parent = resolve_parent(path, name);
        if (parent == -1 || vnodes[parent].type != VFS_DIRECTORY)
            return -1;

        vnode_idx = alloc_vnode();
        if (vnode_idx == -1)
            return -1;

        strcpy(vnodes[vnode_idx].name, name);
        vnodes[vnode_idx].type = VFS_FILE;
        vnodes[vnode_idx].parent = parent;
        vnodes[vnode_idx].data = (uint8_t *)kmalloc(VFS_MAX_DATA);
        vnodes[vnode_idx].capacity = vnodes[vnode_idx].data ? VFS_MAX_DATA : 0;
        vnodes[vnode_idx].size = 0;

        /* Add to parent's children. */
        VNode *pdir = &vnodes[parent];
        if (pdir->child_count < 32)
            pdir->children[pdir->child_count++] = vnode_idx;
    }

    if (vnode_idx == -1)
        return -1;

    if (vnodes[vnode_idx].type == VFS_DIRECTORY)
        return -1;  /* can't open directories as files */

    int fd = alloc_fd();
    if (fd == -1)
        return -1;

    fds[fd].vnode = vnode_idx;
    fds[fd].offset = 0;
    return fd;
}

int
vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !fds[fd].used)
        return -1;
    fds[fd].used = false;
    return 0;
}

int
vfs_read(int fd, void *buf, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !fds[fd].used)
        return -1;

    VNode *vn = &vnodes[fds[fd].vnode];
    if (!vn->data)
        return 0;

    size_t avail = (fds[fd].offset < vn->size) ? vn->size - fds[fd].offset : 0;
    if (count > avail)
        count = avail;

    memcpy(buf, vn->data + fds[fd].offset, count);
    fds[fd].offset += count;
    return (int)count;
}

int
vfs_write(int fd, const void *buf, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !fds[fd].used)
        return -1;

    VNode *vn = &vnodes[fds[fd].vnode];
    if (!vn->data)
        return -1;

    size_t end = fds[fd].offset + count;
    if (end > vn->capacity)
        count = vn->capacity - fds[fd].offset;

    if (count == 0)
        return 0;

    memcpy(vn->data + fds[fd].offset, buf, count);
    fds[fd].offset += count;

    if (fds[fd].offset > vn->size)
        vn->size = fds[fd].offset;

    return (int)count;
}

int
vfs_stat(const char *path, VfsStat *out)
{
    int idx = resolve_path(path);
    if (idx == -1)
        return -1;

    out->type = vnodes[idx].type;
    out->size = vnodes[idx].size;
    strcpy(out->name, vnodes[idx].name);
    return 0;
}

int
vfs_mkdir(const char *path)
{
    /* Check it doesn't already exist. */
    if (resolve_path(path) != -1)
        return -1;

    char name[VFS_MAX_NAME];
    int parent = resolve_parent(path, name);
    if (parent == -1 || vnodes[parent].type != VFS_DIRECTORY)
        return -1;

    int idx = alloc_vnode();
    if (idx == -1)
        return -1;

    strcpy(vnodes[idx].name, name);
    vnodes[idx].type = VFS_DIRECTORY;
    vnodes[idx].parent = parent;

    VNode *pdir = &vnodes[parent];
    if (pdir->child_count < 32)
        pdir->children[pdir->child_count++] = idx;

    return 0;
}

int
vfs_readdir(const char *path, VfsStat *entries, int max_entries)
{
    int idx = resolve_path(path);
    if (idx == -1)
        return -1;

    VNode *dir = &vnodes[idx];
    if (dir->type != VFS_DIRECTORY)
        return -1;

    int count = 0;
    for (int i = 0; i < dir->child_count && count < max_entries; i++) {
        int child = dir->children[i];
        entries[count].type = vnodes[child].type;
        entries[count].size = vnodes[child].size;
        strcpy(entries[count].name, vnodes[child].name);
        count++;
    }

    return count;
}

int
vfs_unlink(const char *path)
{
    int idx = resolve_path(path);
    if (idx <= 0)  /* can't remove root */
        return -1;

    VNode *vn = &vnodes[idx];

    /* Don't remove non-empty directories. */
    if (vn->type == VFS_DIRECTORY && vn->child_count > 0)
        return -1;

    /* Remove from parent's children list. */
    int parent = vn->parent;
    if (parent >= 0) {
        VNode *pdir = &vnodes[parent];
        for (int i = 0; i < pdir->child_count; i++) {
            if (pdir->children[i] == idx) {
                /* Shift remaining children. */
                for (int j = i; j < pdir->child_count - 1; j++)
                    pdir->children[j] = pdir->children[j + 1];
                pdir->child_count--;
                break;
            }
        }
    }

    /* Free file data. */
    if (vn->data) {
        kfree(vn->data);
        vn->data = NULL;
    }

    /* Close any open FDs pointing to this vnode. */
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (fds[i].used && fds[i].vnode == idx)
            fds[i].used = false;
    }

    vn->used = false;
    return 0;
}

int
vfs_rename(const char *old_path, const char *new_path)
{
    int idx = resolve_path(old_path);
    if (idx <= 0)
        return -1;

    /* New path must not already exist. */
    if (resolve_path(new_path) != -1)
        return -1;

    char new_name[VFS_MAX_NAME];
    int new_parent = resolve_parent(new_path, new_name);
    if (new_parent == -1 || vnodes[new_parent].type != VFS_DIRECTORY)
        return -1;

    /* Remove from old parent. */
    int old_parent = vnodes[idx].parent;
    if (old_parent >= 0) {
        VNode *pdir = &vnodes[old_parent];
        for (int i = 0; i < pdir->child_count; i++) {
            if (pdir->children[i] == idx) {
                for (int j = i; j < pdir->child_count - 1; j++)
                    pdir->children[j] = pdir->children[j + 1];
                pdir->child_count--;
                break;
            }
        }
    }

    /* Add to new parent. */
    VNode *npdir = &vnodes[new_parent];
    if (npdir->child_count >= 32)
        return -1;
    npdir->children[npdir->child_count++] = idx;

    /* Update the vnode. */
    strcpy(vnodes[idx].name, new_name);
    vnodes[idx].parent = new_parent;

    return 0;
}
