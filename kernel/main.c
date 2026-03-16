/*
 * main.c — Kernel entry point.
 *
 * Called by the UEFI bootloader after ExitBootServices.  At this point
 * we own the machine: no UEFI runtime, no interrupts, just a flat
 * physical address space and a framebuffer.
 */

#include "kernel.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "interrupts/gdt.h"
#include "interrupts/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "drivers/timer.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "sched/task.h"
#include "sync.h"
#include "syscall.h"
#include "fs/vfs.h"

static const char *mmap_type_str[] = {
    [MMAP_USABLE]                  = "usable",
    [MMAP_RESERVED]                = "reserved",
    [MMAP_ACPI_RECLAIMABLE]        = "ACPI reclaimable",
    [MMAP_ACPI_NVS]                = "ACPI NVS",
    [MMAP_BAD_MEMORY]              = "bad",
    [MMAP_BOOTLOADER_RECLAIMABLE]  = "bootloader",
    [MMAP_KERNEL]                  = "kernel",
    [MMAP_FRAMEBUFFER]             = "framebuffer",
};

void
panic(const char *msg)
{
    kprintf("\n!!! KERNEL PANIC: %s !!!\n", msg);
    serial_write("\n!!! KERNEL PANIC: ");
    serial_write(msg);
    serial_write(" !!!\n");
    cli();
    for (;;)
        hlt();
}

/* ── Demo tasks ──────────────────────────────────────────────────────────── */

static void
task_a(void)
{
    for (int i = 0; i < 5; i++) {
        kprintf("[task A] iteration %d\n", i);
        sched_sleep(100);
    }
    kprintf("[task A] done\n");
}

static void
task_b(void)
{
    for (int i = 0; i < 5; i++) {
        kprintf("[task B] iteration %d\n", i);
        sched_sleep(150);
    }
    kprintf("[task B] done\n");
}

/* ── Shell ───────────────────────────────────────────────────────────────── */

static char cwd[256] = "/";

static const char *shell_commands[] = {
    "help", "mem", "uptime", "clear", "reboot", "ps",
    "ls", "cat", "write", "mkdir", "rm", "touch",
    "cd", "pwd", "echo", "cp", "mv", "stat",
    "head", "tail", "wc", "whoami", "uname", "rmdir", NULL
};

/* Safe strncat: appends at most n bytes from src. */
static void
strncat_safe(char *dst, const char *src, int n)
{
    char *d = dst;
    while (*d) d++;
    while (n-- > 0 && *src)
        *d++ = *src++;
    *d = '\0';
}

/*
 * Resolve a possibly-relative path against cwd into an absolute path.
 * Handles ".", "..", and trailing slashes.
 */
static void
resolve_abs(const char *input, char *out, int outlen)
{
    char tmp[256];

    if (input[0] == '/') {
        strncpy(tmp, input, 255);
        tmp[255] = '\0';
    } else {
        /* Prepend cwd. */
        strncpy(tmp, cwd, 255);
        tmp[255] = '\0';
        int cwdlen = (int)strlen(tmp);
        if (cwdlen > 0 && tmp[cwdlen - 1] != '/') {
            if (cwdlen < 254) { tmp[cwdlen] = '/'; tmp[cwdlen + 1] = '\0'; }
        }
        int space = 255 - (int)strlen(tmp);
        if (space > 0)
            strncat_safe(tmp, input, space);
    }

    /* Normalize: split on '/', process each component. */
    char *components[64];
    int depth = 0;

    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        char *start = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }

        if (strcmp(start, ".") == 0) {
            continue;
        } else if (strcmp(start, "..") == 0) {
            if (depth > 0) depth--;
        } else {
            if (depth < 64)
                components[depth++] = start;
        }
    }

    out[0] = '/';
    out[1] = '\0';
    for (int i = 0; i < depth; i++) {
        if (i > 0) strcat(out, "/");
        else out[1] = '\0';
        int cur = (int)strlen(out);
        strncpy(out + cur, components[i], outlen - cur - 1);
        out[outlen - 1] = '\0';
    }
}

static void
shell_cmd_help(void)
{
    kprintf("Commands:\n");
    kprintf("  help             show this message\n");
    kprintf("  cd [dir]         change directory\n");
    kprintf("  pwd              print working directory\n");
    kprintf("  ls [path]        list directory\n");
    kprintf("  cat <file>       show file contents\n");
    kprintf("  head [-n N] <f>  show first N lines (default 10)\n");
    kprintf("  tail [-n N] <f>  show last N lines (default 10)\n");
    kprintf("  wc <file>        count lines, words, bytes\n");
    kprintf("  echo [text]      print text\n");
    kprintf("  touch <file>     create empty file\n");
    kprintf("  write <f> <txt>  write text to file\n");
    kprintf("  cp <src> <dst>   copy file\n");
    kprintf("  mv <src> <dst>   move/rename file\n");
    kprintf("  mkdir <dir>      create directory\n");
    kprintf("  rmdir <dir>      remove empty directory\n");
    kprintf("  rm <path>        remove file\n");
    kprintf("  stat <path>      show file info\n");
    kprintf("  mem              show memory stats\n");
    kprintf("  uptime           show uptime\n");
    kprintf("  ps               list tasks\n");
    kprintf("  whoami           print user\n");
    kprintf("  uname [-a]       system info\n");
    kprintf("  clear            clear screen\n");
    kprintf("  reboot           reboot machine\n");
}

static void
shell_cmd_ps(void)
{
    kprintf("%-4s %-12s %-10s %-8s\n", "ID", "NAME", "STATE", "PRIO");
    sched_list_tasks();
}

static void
shell_cmd_ls(const char *arg)
{
    char path[256];
    if (!arg || arg[0] == '\0')
        resolve_abs(".", path, 256);
    else
        resolve_abs(arg, path, 256);

    VfsStat entries[32];
    int count = vfs_readdir(path, entries, 32);
    if (count < 0) {
        kprintf("ls: cannot access '%s'\n", path);
        return;
    }

    if (count == 0) {
        kprintf("(empty)\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        if (entries[i].type == VFS_DIRECTORY)
            kprintf("  [dir]  %s\n", entries[i].name);
        else
            kprintf("  %lu B  %s\n", (unsigned long)entries[i].size, entries[i].name);
    }
}

static void
shell_cmd_cat(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        kprintf("cat: missing file path\n");
        return;
    }

    char path[256];
    resolve_abs(arg, path, 256);

    int fd = vfs_open(path, false);
    if (fd < 0) {
        kprintf("cat: '%s': no such file\n", path);
        return;
    }

    char buf[256];
    int n;
    while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        kprintf("%s", buf);
    }
    kprintf("\n");
    vfs_close(fd);
}

static void
shell_cmd_head(const char *args)
{
    int nlines = 10;
    const char *file = args;

    if (args && strncmp(args, "-n ", 3) == 0) {
        args += 3;
        nlines = 0;
        while (*args >= '0' && *args <= '9')
            nlines = nlines * 10 + (*args++ - '0');
        while (*args == ' ') args++;
        file = args;
    }

    if (!file || file[0] == '\0') {
        kprintf("head: missing file\n");
        return;
    }

    char path[256];
    resolve_abs(file, path, 256);

    int fd = vfs_open(path, false);
    if (fd < 0) {
        kprintf("head: '%s': no such file\n", path);
        return;
    }

    char buf[256];
    int lines_printed = 0;
    int n;
    while (lines_printed < nlines && (n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        for (int i = 0; i < n && lines_printed < nlines; i++) {
            console_putchar(buf[i]);
            if (buf[i] == '\n')
                lines_printed++;
        }
    }
    if (lines_printed == 0)
        kprintf("\n");
    vfs_close(fd);
}

static void
shell_cmd_tail(const char *args)
{
    int nlines = 10;
    const char *file = args;

    if (args && strncmp(args, "-n ", 3) == 0) {
        args += 3;
        nlines = 0;
        while (*args >= '0' && *args <= '9')
            nlines = nlines * 10 + (*args++ - '0');
        while (*args == ' ') args++;
        file = args;
    }

    if (!file || file[0] == '\0') {
        kprintf("tail: missing file\n");
        return;
    }

    char path[256];
    resolve_abs(file, path, 256);

    int fd = vfs_open(path, false);
    if (fd < 0) {
        kprintf("tail: '%s': no such file\n", path);
        return;
    }

    /* Two-pass: first count lines, then print last N. */
    char buf2[256];
    int line_count = 0, n2;
    while ((n2 = vfs_read(fd, buf2, sizeof(buf2))) > 0)
        for (int i = 0; i < n2; i++)
            if (buf2[i] == '\n') line_count++;
    vfs_close(fd);

    int skip = line_count - nlines;
    if (skip < 0) skip = 0;

    /* Second pass: reopen and print. */
    fd = vfs_open(path, false);
    if (fd < 0) return;
    int cur_line = 0;
    while ((n2 = vfs_read(fd, buf2, sizeof(buf2))) > 0) {
        for (int i = 0; i < n2; i++) {
            if (cur_line >= skip)
                console_putchar(buf2[i]);
            if (buf2[i] == '\n')
                cur_line++;
        }
    }
    if (line_count == 0 || cur_line == 0)
        kprintf("\n");
}

static void
shell_cmd_wc(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        kprintf("wc: missing file\n");
        return;
    }

    char path[256];
    resolve_abs(arg, path, 256);

    int fd = vfs_open(path, false);
    if (fd < 0) {
        kprintf("wc: '%s': no such file\n", path);
        return;
    }

    char buf[256];
    int lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    int n;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                words++;
            }
        }
    }
    vfs_close(fd);
    kprintf("  %d  %d  %d %s\n", lines, words, bytes, arg);
}

static void
shell_cmd_write(const char *args)
{
    if (!args || args[0] == '\0') {
        kprintf("write: usage: write <file> <text>\n");
        return;
    }

    char filename[128];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 127)
        filename[i] = args[i], i++;
    filename[i] = '\0';

    while (args[i] == ' ') i++;
    const char *text = &args[i];

    char path[256];
    resolve_abs(filename, path, 256);

    int fd = vfs_open(path, true);
    if (fd < 0) {
        kprintf("write: cannot open '%s'\n", path);
        return;
    }

    int len = (int)strlen(text);
    vfs_write(fd, text, len);
    vfs_close(fd);
    kprintf("Wrote %d bytes to %s\n", len, path);
}

static void
shell_cmd_cp(const char *args)
{
    if (!args || args[0] == '\0') {
        kprintf("cp: usage: cp <src> <dst>\n");
        return;
    }

    char src_name[128], dst_name[128];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 127)
        src_name[i] = args[i], i++;
    src_name[i] = '\0';
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 127)
        dst_name[j++] = args[i++];
    dst_name[j] = '\0';

    if (dst_name[0] == '\0') {
        kprintf("cp: missing destination\n");
        return;
    }

    char src[256], dst[256];
    resolve_abs(src_name, src, 256);
    resolve_abs(dst_name, dst, 256);

    int sfd = vfs_open(src, false);
    if (sfd < 0) {
        kprintf("cp: '%s': no such file\n", src);
        return;
    }

    int dfd = vfs_open(dst, true);
    if (dfd < 0) {
        vfs_close(sfd);
        kprintf("cp: cannot create '%s'\n", dst);
        return;
    }

    char buf[256];
    int n;
    while ((n = vfs_read(sfd, buf, sizeof(buf))) > 0)
        vfs_write(dfd, buf, n);

    vfs_close(sfd);
    vfs_close(dfd);
}

static void
shell_cmd_mv(const char *args)
{
    if (!args || args[0] == '\0') {
        kprintf("mv: usage: mv <src> <dst>\n");
        return;
    }

    char src_name[128], dst_name[128];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 127)
        src_name[i] = args[i], i++;
    src_name[i] = '\0';
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 127)
        dst_name[j++] = args[i++];
    dst_name[j] = '\0';

    if (dst_name[0] == '\0') {
        kprintf("mv: missing destination\n");
        return;
    }

    char src[256], dst[256];
    resolve_abs(src_name, src, 256);
    resolve_abs(dst_name, dst, 256);

    if (vfs_rename(src, dst) != 0)
        kprintf("mv: cannot move '%s' to '%s'\n", src, dst);
}

static void
shell_cmd_stat(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        kprintf("stat: missing path\n");
        return;
    }

    char path[256];
    resolve_abs(arg, path, 256);

    VfsStat st;
    if (vfs_stat(path, &st) != 0) {
        kprintf("stat: '%s': no such file or directory\n", path);
        return;
    }

    kprintf("  File: %s\n", path);
    kprintf("  Type: %s\n", st.type == VFS_DIRECTORY ? "directory" : "regular file");
    if (st.type == VFS_FILE)
        kprintf("  Size: %lu bytes\n", (unsigned long)st.size);
}

/* ── Line editing helpers ────────────────────────────────────────────────── */

static void
shell_redraw_from(char *line, int len, int cursor)
{
    console_begin_batch();
    /* Redraw from cursor to end of line, then erase any trailing char. */
    for (int i = cursor; i < len; i++)
        console_putchar(line[i]);
    console_putchar(' ');  /* erase old last char */
    /* Move cursor back to the right position. */
    for (int i = 0; i < len - cursor + 1; i++)
        console_putchar('\b');
    console_end_batch();
}

/* ── Tab completion (bash-style) ────────────────────────────────────────── */

/* Track whether the last keypress was also Tab (for double-Tab listing). */
static bool last_was_tab;

/*
 * Compute the longest common prefix length among all strings in `matches`
 * (array of pointers, `count` entries), starting comparison from position
 * `from` (i.e. characters before `from` are assumed to already match).
 */
static int
common_prefix_len(const char *matches[], int count, int from)
{
    if (count <= 0)
        return from;

    int cp = (int)strlen(matches[0]);
    for (int i = 1; i < count; i++) {
        int mlen = (int)strlen(matches[i]);
        if (mlen < cp) cp = mlen;
        for (int j = from; j < cp; j++) {
            if (matches[i][j] != matches[0][j]) {
                cp = j;
                break;
            }
        }
    }
    return cp;
}

static void
shell_list_matches(const char *matches[], int types[], int count)
{
    /* Find longest match name for column formatting. */
    int maxw = 0;
    for (int i = 0; i < count; i++) {
        int w = (int)strlen(matches[i]) + (types[i] == VFS_DIRECTORY ? 1 : 0);
        if (w > maxw) maxw = w;
    }
    maxw += 2;  /* padding */

    int cols = 80 / maxw;
    if (cols < 1) cols = 1;

    kprintf("\n");
    for (int i = 0; i < count; i++) {
        if (types[i] == VFS_DIRECTORY)
            kprintf("%s/", matches[i]);
        else
            kprintf("%s", matches[i]);

        if ((i + 1) % cols == 0 || i == count - 1)
            kprintf("\n");
        else {
            /* Pad to column width. */
            int w = (int)strlen(matches[i]) + (types[i] == VFS_DIRECTORY ? 1 : 0);
            for (int p = w; p < maxw; p++)
                console_putchar(' ');
        }
    }
}

static void
shell_print_prompt(void)
{
    kprintf("root@micro-os:%s$ ", cwd);
}

static void
shell_redraw_prompt(char *line, int pos)
{
    shell_print_prompt();
    for (int i = 0; i < pos; i++)
        console_putchar(line[i]);
}

static void
shell_tab_complete(char *line, int *pos_out, int *len_out, int maxlen)
{
    int pos = *pos_out;
    int len = *len_out;
    line[len] = '\0';

    /* Find start of the current token. */
    int tok_start = pos;
    while (tok_start > 0 && line[tok_start - 1] != ' ')
        tok_start--;

    const char *prefix = &line[tok_start];
    int prefix_len = pos - tok_start;

    /* Empty token: list all possibilities on double-Tab. */
    bool is_command = (tok_start == 0);

    if (prefix_len == 0 && !last_was_tab) {
        last_was_tab = true;
        return;
    }

    /* Collect matches. */
    const char *matches[64];
    int types[64];  /* VFS_FILE or VFS_DIRECTORY, 0 for commands */
    int match_count = 0;

    if (is_command) {
        for (int i = 0; shell_commands[i] && match_count < 64; i++) {
            if (prefix_len == 0 ||
                strncmp(shell_commands[i], prefix, prefix_len) == 0) {
                matches[match_count] = shell_commands[i];
                types[match_count] = 0;
                match_count++;
            }
        }
    } else {
        /* Parse directory and name prefix from the token. */
        char dir[256];
        char name_prefix[128] = "";
        int last_slash = -1;
        for (int i = 0; i < prefix_len; i++) {
            if (prefix[i] == '/')
                last_slash = i;
        }

        if (last_slash >= 0) {
            /* Extract the directory portion and resolve it. */
            char raw_dir[256];
            int dlen = last_slash + 1;
            strncpy(raw_dir, prefix, dlen < 255 ? dlen : 255);
            raw_dir[dlen] = '\0';
            resolve_abs(raw_dir, dir, 256);
            strncpy(name_prefix, prefix + last_slash + 1, 127);
            name_prefix[prefix_len - last_slash - 1] = '\0';
        } else {
            /* No slash — complete in cwd. */
            resolve_abs(".", dir, 256);
            strncpy(name_prefix, prefix, 127);
            name_prefix[prefix_len] = '\0';
        }

        int np_len = (int)strlen(name_prefix);
        VfsStat entries[32];
        int count = vfs_readdir(dir, entries, 32);

        for (int i = 0; i < count && match_count < 64; i++) {
            if (np_len == 0 ||
                strncmp(entries[i].name, name_prefix, np_len) == 0) {
                matches[match_count] = entries[i].name;
                types[match_count] = entries[i].type;
                match_count++;
            }
        }
    }

    if (match_count == 0) {
        last_was_tab = true;
        return;
    }

    if (match_count == 1) {
        /* Unique match: complete fully. */
        const char *m = matches[0];
        int mlen = (int)strlen(m);

        /* How much of the match name is already typed. */
        int already;
        if (is_command) {
            already = prefix_len;
        } else {
            /* Name portion only. */
            int last_slash = -1;
            for (int i = 0; i < prefix_len; i++)
                if (prefix[i] == '/') last_slash = i;
            already = (last_slash >= 0) ? (prefix_len - last_slash - 1) : prefix_len;
        }

        for (int i = already; i < mlen && pos < maxlen - 2; i++) {
            /* Insert at cursor. */
            if (pos < len)
                memmove(&line[pos + 1], &line[pos], len - pos);
            line[pos] = m[i];
            pos++;
            len++;
        }

        /* Append trailing char: space for commands/files, / for directories. */
        if (pos < maxlen - 2) {
            char trail;
            if (is_command)
                trail = ' ';
            else
                trail = (types[0] == VFS_DIRECTORY) ? '/' : ' ';

            if (pos < len)
                memmove(&line[pos + 1], &line[pos], len - pos);
            line[pos] = trail;
            pos++;
            len++;
        }

        /* Redraw from the original cursor position. */
        line[len] = '\0';
        console_begin_batch();
        for (int i = *pos_out; i < len; i++)
            console_putchar(line[i]);
        for (int i = len; i > pos; i--)
            console_putchar('\b');
        console_end_batch();

        *pos_out = pos;
        *len_out = len;
        last_was_tab = false;
        return;
    }

    /* Multiple matches: complete the common prefix first. */
    int already;
    if (is_command) {
        already = prefix_len;
    } else {
        int last_slash = -1;
        for (int i = 0; i < prefix_len; i++)
            if (prefix[i] == '/') last_slash = i;
        already = (last_slash >= 0) ? (prefix_len - last_slash - 1) : prefix_len;
    }

    int cp = common_prefix_len(matches, match_count, already);
    bool advanced = false;

    if (cp > already) {
        /* We can extend the completion. */
        for (int i = already; i < cp && pos < maxlen - 2; i++) {
            if (pos < len)
                memmove(&line[pos + 1], &line[pos], len - pos);
            line[pos] = matches[0][i];
            pos++;
            len++;
        }

        line[len] = '\0';
        console_begin_batch();
        for (int i = *pos_out; i < len; i++)
            console_putchar(line[i]);
        for (int i = len; i > pos; i--)
            console_putchar('\b');
        console_end_batch();

        *pos_out = pos;
        *len_out = len;
        advanced = true;
    }

    if (!advanced && last_was_tab) {
        /* Double-Tab: show all matches. */
        shell_list_matches(matches, types, match_count);
        shell_redraw_prompt(line, pos);
        *pos_out = pos;
        *len_out = len;
        last_was_tab = false;
        return;
    }

    last_was_tab = true;
}

/* ── Shell command dispatcher ───────────────────────────────────────────── */

/* Strip trailing whitespace in-place. */
static void
strip_trailing(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    s[n] = '\0';
}

static void
shell_execute(char *line, int len)
{
    if (len == 0)
        return;

    strip_trailing(line);

    if (strcmp(line, "help") == 0) {
        shell_cmd_help();

    } else if (strcmp(line, "pwd") == 0) {
        kprintf("%s\n", cwd);

    } else if (strncmp(line, "cd", 2) == 0 &&
               (line[2] == '\0' || line[2] == ' ')) {
        const char *arg = (line[2] == ' ') ? &line[3] : NULL;
        char path[256];
        if (!arg || arg[0] == '\0') {
            strcpy(cwd, "/");
        } else {
            resolve_abs(arg, path, 256);
            VfsStat st;
            if (vfs_stat(path, &st) != 0)
                kprintf("cd: '%s': no such directory\n", path);
            else if (st.type != VFS_DIRECTORY)
                kprintf("cd: '%s': not a directory\n", path);
            else
                strcpy(cwd, path);
        }

    } else if (strncmp(line, "echo", 4) == 0 &&
               (line[4] == '\0' || line[4] == ' ')) {
        const char *text = (line[4] == ' ') ? &line[5] : "";
        kprintf("%s\n", text);

    } else if (strcmp(line, "mem") == 0) {
        kprintf("Free pages: %lu (%lu MiB)\n",
                pmm_free_pages(),
                pmm_free_pages() * PAGE_SIZE / (1024 * 1024));

    } else if (strcmp(line, "uptime") == 0) {
        uint64_t ticks = timer_ticks();
        uint64_t secs = ticks / 100;
        uint64_t mins = secs / 60;
        uint64_t hrs  = mins / 60;
        kprintf("up %lu:%02lu:%02lu (%lu ticks)\n",
                hrs, mins % 60, secs % 60, ticks);

    } else if (strcmp(line, "clear") == 0) {
        console_clear();

    } else if (strcmp(line, "ps") == 0) {
        shell_cmd_ps();

    } else if (strcmp(line, "whoami") == 0) {
        kprintf("root\n");

    } else if (strncmp(line, "uname", 5) == 0 &&
               (line[5] == '\0' || line[5] == ' ')) {
        const char *arg = (line[5] == ' ') ? &line[6] : "";
        if (strcmp(arg, "-a") == 0)
            kprintf("micro-os 0.2 x86_64 micro-os-kernel\n");
        else
            kprintf("micro-os\n");

    } else if (strcmp(line, "reboot") == 0) {
        kprintf("Rebooting...\n");
        uint8_t good = 0x02;
        while (good & 0x02)
            good = inb(0x64);
        outb(0x64, 0xFE);
        cli();
        struct { uint16_t limit; uint64_t base; } __attribute__((packed))
            null_idt = { 0, 0 };
        __asm__ volatile("lidt %0" : : "m"(null_idt));
        __asm__ volatile("int $0x03");

    } else if (strncmp(line, "ls", 2) == 0 &&
               (line[2] == '\0' || line[2] == ' ')) {
        const char *arg = (line[2] == ' ') ? &line[3] : NULL;
        shell_cmd_ls(arg);

    } else if (strncmp(line, "cat ", 4) == 0) {
        shell_cmd_cat(&line[4]);

    } else if (strncmp(line, "head ", 5) == 0) {
        shell_cmd_head(&line[5]);

    } else if (strncmp(line, "tail ", 5) == 0) {
        shell_cmd_tail(&line[5]);

    } else if (strncmp(line, "wc ", 3) == 0) {
        shell_cmd_wc(&line[3]);

    } else if (strncmp(line, "write ", 6) == 0) {
        shell_cmd_write(&line[6]);

    } else if (strncmp(line, "cp ", 3) == 0) {
        shell_cmd_cp(&line[3]);

    } else if (strncmp(line, "mv ", 3) == 0) {
        shell_cmd_mv(&line[3]);

    } else if (strncmp(line, "stat ", 5) == 0) {
        shell_cmd_stat(&line[5]);

    } else if (strncmp(line, "mkdir ", 6) == 0) {
        char path[256];
        resolve_abs(&line[6], path, 256);
        if (vfs_mkdir(path) == 0)
            kprintf("Created %s\n", path);
        else
            kprintf("mkdir: failed to create '%s'\n", path);

    } else if (strncmp(line, "rmdir ", 6) == 0) {
        char path[256];
        resolve_abs(&line[6], path, 256);
        VfsStat st;
        if (vfs_stat(path, &st) != 0)
            kprintf("rmdir: '%s': no such directory\n", path);
        else if (st.type != VFS_DIRECTORY)
            kprintf("rmdir: '%s': not a directory\n", path);
        else if (vfs_unlink(path) != 0)
            kprintf("rmdir: '%s': directory not empty\n", path);
        else
            kprintf("Removed %s\n", path);

    } else if (strncmp(line, "rm ", 3) == 0) {
        char path[256];
        resolve_abs(&line[3], path, 256);
        if (vfs_unlink(path) == 0)
            kprintf("Removed %s\n", path);
        else
            kprintf("rm: cannot remove '%s'\n", path);

    } else if (strncmp(line, "touch ", 6) == 0) {
        char path[256];
        resolve_abs(&line[6], path, 256);
        int fd = vfs_open(path, true);
        if (fd >= 0) {
            vfs_close(fd);
            kprintf("Created %s\n", path);
        } else {
            kprintf("touch: cannot create '%s'\n", path);
        }

    } else {
        kprintf("Unknown command: %s\n", line);
    }
}

/* ── Shell main loop ────────────────────────────────────────────────────── */

static void
shell_task(void)
{
    /* Wait for demo tasks to finish, then present a clean shell. */
    sched_sleep(50);  /* brief yield to let tasks start */
    while (sched_any_priority(PRIORITY_LOW))
        sched_sleep(10);
    console_clear();
    keyboard_flush();  /* discard any stray scancodes from boot */
    kprintf("micro-os 0.2 — type 'help' for commands\n");
    shell_print_prompt();

    char line[128];
    int pos = 0;    /* cursor position (insertion point) */
    int len = 0;    /* total chars in line */

    for (;;) {
        uint16_t key = keyboard_getchar();

        if (key == '\n') {
            console_putchar('\n');
            line[len] = '\0';
            shell_execute(line, len);
            pos = 0;
            len = 0;
            last_was_tab = false;
            shell_print_prompt();

        } else if (key == '\b') {
            /* Backspace: delete char before cursor. */
            last_was_tab = false;
            if (pos > 0) {
                memmove(&line[pos - 1], &line[pos], len - pos);
                pos--;
                len--;
                console_putchar('\b');
                shell_redraw_from(line, len, pos);
            }

        } else if (key == '\t') {
            shell_tab_complete(line, &pos, &len, (int)sizeof(line));

        } else if (key == KEY_LEFT) {
            last_was_tab = false;
            if (pos > 0) {
                pos--;
                console_putchar('\b');
            }

        } else if (key == KEY_RIGHT) {
            last_was_tab = false;
            if (pos < len) {
                console_putchar(line[pos]);
                pos++;
            }

        } else if (key == KEY_HOME) {
            last_was_tab = false;
            console_begin_batch();
            while (pos > 0) {
                pos--;
                console_putchar('\b');
            }
            console_end_batch();

        } else if (key == KEY_END) {
            last_was_tab = false;
            console_begin_batch();
            while (pos < len) {
                console_putchar(line[pos]);
                pos++;
            }
            console_end_batch();

        } else if (key == KEY_DELETE) {
            last_was_tab = false;
            if (pos < len) {
                memmove(&line[pos], &line[pos + 1], len - pos - 1);
                len--;
                shell_redraw_from(line, len, pos);
            }

        } else if (key == 0x0C) {
            /* Ctrl+L: clear screen. */
            last_was_tab = false;
            console_clear();
            console_begin_batch();
            shell_print_prompt();
            for (int i = 0; i < len; i++)
                console_putchar(line[i]);
            for (int i = len; i > pos; i--)
                console_putchar('\b');
            console_end_batch();

        } else if (key >= 0x80) {
            /* Other special keys (arrows up/down, F-keys, etc.) — ignore. */

        } else if (key >= 0x01 && key <= 0x1F) {
            /* Other control codes — ignore. */

        } else if (len < (int)sizeof(line) - 1) {
            /* Printable character: insert at cursor. */
            last_was_tab = false;
            if (pos < len)
                memmove(&line[pos + 1], &line[pos], len - pos);
            line[pos] = (char)key;
            len++;
            console_begin_batch();
            for (int i = pos; i < len; i++)
                console_putchar(line[i]);
            pos++;
            for (int i = len; i > pos; i--)
                console_putchar('\b');
            console_end_batch();
        }
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

extern uint8_t __bss_start[];
extern uint8_t __bss_end[];

static void
zero_bss(void)
{
    uint8_t *p = __bss_start;
    while (p < __bss_end)
        *p++ = 0;
}

void
kernel_main(BootInfo *info)
{
    zero_bss();

    /* ── Serial ──────────────────────────────────────────────────────── */
    serial_init();
    serial_write("micro-os kernel booting...\n");

    /* ── Console ─────────────────────────────────────────────────────── */
    console_init(&info->fb);
    kprintf("micro-os v0.2\n");
    kprintf("Framebuffer: %ux%u @ 0x%lx\n",
            info->fb.width, info->fb.height, info->fb.base);

    /* ── Memory map ──────────────────────────────────────────────────── */
    uint64_t total_usable = 0;
    kprintf("\nMemory map (%u entries):\n", info->mmap_count);
    for (uint32_t i = 0; i < info->mmap_count; i++) {
        MmapEntry *e = &info->mmap[i];
        const char *type = (e->type <= MMAP_FRAMEBUFFER)
                            ? mmap_type_str[e->type] : "unknown";
        kprintf("  0x%lx - 0x%lx  %s\n",
                e->base, e->base + e->length, type);

        if (e->type == MMAP_USABLE)
            total_usable += e->length;
    }
    kprintf("Total usable: %lu MiB\n\n", total_usable / (1024 * 1024));

    /* ── GDT ─────────────────────────────────────────────────────────── */
    gdt_init();
    kprintf("[ok] GDT\n");

    /* ── IDT ─────────────────────────────────────────────────────────── */
    idt_init();
    sti();
    kprintf("[ok] IDT\n");

    /* ── PMM ─────────────────────────────────────────────────────────── */
    pmm_init(info->mmap, info->mmap_count,
             info->kernel_phys_base, info->kernel_size);
    kprintf("[ok] PMM (%lu MiB free)\n",
            pmm_free_pages() * PAGE_SIZE / (1024 * 1024));

    /* ── VMM ─────────────────────────────────────────────────────────── */
    vmm_init();
    kprintf("[ok] VMM\n");

    /* ── Heap ────────────────────────────────────────────────────────── */
    heap_init();
    kprintf("[ok] Heap\n");

    /* ── Timer ───────────────────────────────────────────────────────── */
    timer_init(100);
    kprintf("[ok] PIT (100 Hz)\n");

    /* ── Keyboard ────────────────────────────────────────────────────── */
    keyboard_init();
    kprintf("[ok] Keyboard\n");

    /* ── Mouse ───────────────────────────────────────────────────────── */
    mouse_init();
    kprintf("[ok] Mouse\n");

    /* ── VFS ─────────────────────────────────────────────────────────── */
    vfs_init();
    kprintf("[ok] VFS (ramfs)\n");

    /* Create some demo files. */
    {
        int fd = vfs_open("/hello.txt", true);
        if (fd >= 0) {
            const char *msg = "Hello from micro-os!\n";
            vfs_write(fd, msg, strlen(msg));
            vfs_close(fd);
        }
        vfs_mkdir("/bin");
        vfs_mkdir("/tmp");
    }

    /* ── Syscalls ────────────────────────────────────────────────────── */
    syscall_init();
    kprintf("[ok] Syscalls\n");

    /* ── Scheduler ───────────────────────────────────────────────────── */
    sched_init();
    task_create_prio("task_a", task_a, PRIORITY_LOW);
    task_create_prio("task_b", task_b, PRIORITY_LOW);
    task_create_prio("shell",  shell_task, PRIORITY_HIGH);
    kprintf("[ok] Scheduler (3 tasks, preemptive)\n");

    kprintf("\nBoot complete.\n");
    serial_write("Boot complete.\n");

    timer_enable_preemption();

    for (;;)
        sched_yield();
}
