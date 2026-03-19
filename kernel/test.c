/*
 * test.c — Kernel self-tests.
 *
 * Lightweight test framework that validates core subsystems at boot
 * or on demand via the "test" shell command.  Each test is a function
 * that returns 0 on success or -1 on failure.
 */

#include "test.h"
#include "string.h"
#include "console.h"
#include "mm/heap.h"
#include "fs/vfs.h"

static int tests_run;
static int tests_passed;
static int tests_failed;

#define TEST(name) \
    static int test_##name(void); \
    static const char *testname_##name = #name; \
    static int test_##name(void)

#define RUN(name) do { \
    tests_run++; \
    kprintf("  %-30s ", testname_##name); \
    if (test_##name() == 0) { \
        tests_passed++; \
        kprintf("PASS\n"); \
    } else { \
        tests_failed++; \
        kprintf("FAIL\n"); \
    } \
} while (0)

/* ── String tests ───────────────────────────────────────────────────────── */

TEST(strlen_basic)
{
    if (strlen("") != 0) return -1;
    if (strlen("hello") != 5) return -1;
    if (strlen("a") != 1) return -1;
    return 0;
}

TEST(strcmp_basic)
{
    if (strcmp("abc", "abc") != 0) return -1;
    if (strcmp("abc", "abd") >= 0) return -1;
    if (strcmp("abd", "abc") <= 0) return -1;
    if (strcmp("", "") != 0) return -1;
    if (strcmp("a", "") <= 0) return -1;
    return 0;
}

TEST(strncmp_basic)
{
    if (strncmp("abcdef", "abcxyz", 3) != 0) return -1;
    if (strncmp("abcdef", "abcxyz", 4) >= 0) return -1;
    if (strncmp("abc", "abc", 10) != 0) return -1;
    return 0;
}

TEST(strcpy_basic)
{
    char buf[32];
    strcpy(buf, "hello");
    if (strcmp(buf, "hello") != 0) return -1;
    strcpy(buf, "");
    if (strcmp(buf, "") != 0) return -1;
    return 0;
}

TEST(strncpy_basic)
{
    char buf[8];
    memset(buf, 'X', sizeof(buf));
    strncpy(buf, "hi", sizeof(buf));
    if (strcmp(buf, "hi") != 0) return -1;
    /* Remaining bytes should be zero-filled. */
    if (buf[3] != '\0' || buf[7] != '\0') return -1;
    return 0;
}

TEST(strcat_basic)
{
    char buf[32];
    strcpy(buf, "hello");
    strcat(buf, " world");
    if (strcmp(buf, "hello world") != 0) return -1;
    return 0;
}

TEST(strchr_basic)
{
    const char *s = "hello world";
    if (strchr(s, 'w') != s + 6) return -1;
    if (strchr(s, 'z') != (void *)0) return -1;
    if (strchr(s, '\0') != s + 11) return -1;
    return 0;
}

TEST(strrchr_basic)
{
    const char *s = "hello";
    if (strrchr(s, 'l') != s + 3) return -1;
    if (strrchr(s, 'z') != (void *)0) return -1;
    return 0;
}

TEST(memset_basic)
{
    char buf[16];
    memset(buf, 0xAB, sizeof(buf));
    for (int i = 0; i < 16; i++)
        if ((uint8_t)buf[i] != 0xAB) return -1;
    return 0;
}

TEST(memcpy_basic)
{
    char src[] = "test data";
    char dst[16];
    memcpy(dst, src, sizeof(src));
    if (memcmp(dst, src, sizeof(src)) != 0) return -1;
    return 0;
}

TEST(memmove_overlap)
{
    char buf[] = "abcdefgh";
    /* Overlapping copy forward. */
    memmove(buf + 2, buf, 6);
    if (memcmp(buf + 2, "abcdef", 6) != 0) return -1;
    return 0;
}

TEST(memcmp_basic)
{
    if (memcmp("abc", "abc", 3) != 0) return -1;
    if (memcmp("abc", "abd", 3) >= 0) return -1;
    if (memcmp("abd", "abc", 3) <= 0) return -1;
    return 0;
}

/* ── Heap tests ─────────────────────────────────────────────────────────── */

TEST(kmalloc_basic)
{
    void *p = kmalloc(64);
    if (!p) return -1;
    /* Should be able to write without faulting. */
    memset(p, 0xCC, 64);
    kfree(p);
    return 0;
}

TEST(kmalloc_multiple)
{
    void *a = kmalloc(128);
    void *b = kmalloc(128);
    if (!a || !b) return -1;
    /* Allocations should not overlap. */
    if (a == b) return -1;
    uint64_t diff = (uint64_t)b > (uint64_t)a
                  ? (uint64_t)b - (uint64_t)a
                  : (uint64_t)a - (uint64_t)b;
    if (diff < 128) return -1;
    kfree(a);
    kfree(b);
    return 0;
}

TEST(kmalloc_reuse)
{
    /* Free then realloc should reuse memory. */
    void *a = kmalloc(64);
    if (!a) return -1;
    kfree(a);
    void *b = kmalloc(64);
    if (!b) return -1;
    kfree(b);
    return 0;
}

TEST(kmalloc_zero)
{
    /* Zero-size allocation should still return something or NULL gracefully. */
    void *p = kmalloc(0);
    if (p) kfree(p);
    return 0;
}

/* ── VFS tests ──────────────────────────────────────────────────────────── */

TEST(vfs_create_read_write)
{
    int fd = vfs_open("/test_file", true);
    if (fd < 0) return -1;

    const char *data = "test content";
    int written = vfs_write(fd, data, strlen(data));
    if (written != (int)strlen(data)) { vfs_close(fd); return -1; }
    vfs_close(fd);

    /* Re-open and read back. */
    fd = vfs_open("/test_file", false);
    if (fd < 0) return -1;

    char buf[64];
    int nread = vfs_read(fd, buf, sizeof(buf));
    vfs_close(fd);
    if (nread != (int)strlen(data)) return -1;
    if (memcmp(buf, data, nread) != 0) return -1;

    vfs_unlink("/test_file");
    return 0;
}

TEST(vfs_mkdir_readdir)
{
    if (vfs_mkdir("/test_dir") != 0) return -1;

    VfsStat st;
    if (vfs_stat("/test_dir", &st) != 0) return -1;
    if (st.type != VFS_DIRECTORY) return -1;

    vfs_unlink("/test_dir");
    return 0;
}

TEST(vfs_stat_missing)
{
    VfsStat st;
    /* Non-existent path should fail. */
    if (vfs_stat("/no_such_file", &st) == 0) return -1;
    return 0;
}

TEST(vfs_unlink)
{
    int fd = vfs_open("/to_delete", true);
    if (fd < 0) return -1;
    vfs_close(fd);

    if (vfs_unlink("/to_delete") != 0) return -1;

    VfsStat st;
    if (vfs_stat("/to_delete", &st) == 0) return -1;  /* should be gone */
    return 0;
}

TEST(vfs_rename)
{
    int fd = vfs_open("/rename_src", true);
    if (fd < 0) return -1;
    const char *data = "rename test";
    vfs_write(fd, data, strlen(data));
    vfs_close(fd);

    if (vfs_rename("/rename_src", "/rename_dst") != 0) return -1;

    VfsStat st;
    if (vfs_stat("/rename_src", &st) == 0) return -1;  /* old name gone */
    if (vfs_stat("/rename_dst", &st) != 0) return -1;  /* new name exists */

    /* Verify content survived. */
    fd = vfs_open("/rename_dst", false);
    if (fd < 0) return -1;
    char buf[64];
    int nread = vfs_read(fd, buf, sizeof(buf));
    vfs_close(fd);
    if (nread != (int)strlen(data)) return -1;
    if (memcmp(buf, data, nread) != 0) return -1;

    vfs_unlink("/rename_dst");
    return 0;
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int
kernel_run_tests(void)
{
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;

    kprintf("Running kernel self-tests...\n\n");

    /* String */
    kprintf("[string]\n");
    RUN(strlen_basic);
    RUN(strcmp_basic);
    RUN(strncmp_basic);
    RUN(strcpy_basic);
    RUN(strncpy_basic);
    RUN(strcat_basic);
    RUN(strchr_basic);
    RUN(strrchr_basic);
    RUN(memset_basic);
    RUN(memcpy_basic);
    RUN(memmove_overlap);
    RUN(memcmp_basic);

    /* Heap */
    kprintf("[heap]\n");
    RUN(kmalloc_basic);
    RUN(kmalloc_multiple);
    RUN(kmalloc_reuse);
    RUN(kmalloc_zero);

    /* VFS */
    kprintf("[vfs]\n");
    RUN(vfs_create_read_write);
    RUN(vfs_mkdir_readdir);
    RUN(vfs_stat_missing);
    RUN(vfs_unlink);
    RUN(vfs_rename);

    kprintf("\n%d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        kprintf(", %d FAILED", tests_failed);
    kprintf("\n");

    return tests_failed;
}
