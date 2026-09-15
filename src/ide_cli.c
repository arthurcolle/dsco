#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include "ide_cli.h"
#include "native_buffer_editor.h"
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IDE_LIMIT (256U * 1024U)
#define IDE_ENTRIES 2048
/* No process-global project settings; signal state is installed only in UI. */
static volatile sig_atomic_t ide_stop;
typedef struct { char *name; bool dir; } ide_entry;
typedef struct {
    int root;
    char project[PATH_MAX], directory[PATH_MAX], path[PATH_MAX];
    char text[IDE_LIMIT + 1], base[IDE_LIMIT + 1], message[512];
    struct stat stamp;
    native_buffer_editor_t editor;
    ide_entry files[IDE_ENTRIES];
    size_t count, selected, top, left;
    bool dirty, browser, truncated;
    int rows, cols;
    struct termios saved;
} ide_state;

static void ide_error(ide_state *s, const char *what) {
    snprintf(s->message, sizeof(s->message), "%s: %s", what, strerror(errno));
}
/* Each relative component is opened independently, never following a symlink.
 * Directory descriptors pin the destination, not an attacker-controlled string. */
static int ide_directory(int root, const char *path) {
    if (path[0] == '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
    int fd = dup(root);
    if (fd < 0) return -1;
    char copy[PATH_MAX]; snprintf(copy, sizeof(copy), "%s", path);
    char *save = NULL;
    for (char *p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!strcmp(p, ".") || !strcmp(p, "..")) { close(fd); errno = EINVAL; return -1; }
        int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int err = errno; close(fd); fd = next; errno = err;
        if (fd < 0) return -1;
    }
    return fd;
}
static int ide_parent(ide_state *s, const char *path, char *name) {
    if (!*path || *path == '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
    char copy[PATH_MAX]; snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    const char *leaf = slash ? slash + 1 : copy;
    if (!*leaf || !strcmp(leaf, ".") || !strcmp(leaf, "..") || strlen(leaf) > NAME_MAX) {
        errno = EINVAL; return -1;
    }
    strcpy(name, leaf);
    if (slash) *slash = 0;
    return ide_directory(s->root, slash ? copy : "");
}
static bool ide_stamp_equal(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
#define IDE_MT st_mtimespec
#define IDE_CT st_ctimespec
#else
#define IDE_MT st_mtim
#define IDE_CT st_ctim
#endif
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size && a->st_mode == b->st_mode &&
        a->st_uid == b->st_uid && a->st_gid == b->st_gid && a->st_nlink == b->st_nlink &&
        a->IDE_MT.tv_sec == b->IDE_MT.tv_sec && a->IDE_MT.tv_nsec == b->IDE_MT.tv_nsec &&
        a->IDE_CT.tv_sec == b->IDE_CT.tv_sec && a->IDE_CT.tv_nsec == b->IDE_CT.tv_nsec;
}
static size_t ide_next(const char *t, size_t p) {
    if (!t[p]) return p;
    for (++p; ((unsigned char)t[p] & 0xc0) == 0x80; ++p) {}
    return p;
}
static bool ide_text(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        if (c < 128) {
            if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return false;
            ++i; continue;
        }
        size_t w = c >= 0xc2 && c <= 0xdf ? 2 : c <= 0xef && c >= 0xe0 ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!w || w > n - i) return false;
        for (size_t j = 1; j < w; ++j) if ((p[i+j] & 0xc0) != 0x80) return false;
        if ((c == 0xe0 && p[i+1] < 0xa0) || (c == 0xed && p[i+1] >= 0xa0) ||
            (c == 0xf0 && p[i+1] < 0x90) || (c == 0xf4 && p[i+1] >= 0x90)) return false;
        i += w;
    }
    return true;
}
static int ide_read_at(int parent, const char *name, char *text, struct stat *st) {
    /* O_NONBLOCK prevents a regular-file-to-FIFO swap from hanging open. */
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = -1;
    if (fstat(fd, st)) goto done;
    if (!S_ISREG(st->st_mode)) { errno = EINVAL; goto done; }
    if (st->st_size < 0 || st->st_size > IDE_LIMIT) { errno = EFBIG; goto done; }
    size_t n = 0;
    while (n <= IDE_LIMIT) {
        ssize_t got = read(fd, text + n, IDE_LIMIT + 1 - n);
        if (got < 0) { if (errno == EINTR) continue; goto done; }
        if (!got) break;
        n += (size_t)got;
        if (n > IDE_LIMIT) { errno = EFBIG; goto done; }
    }
    struct stat after;
    if (fstat(fd, &after)) goto done;
    if (!ide_stamp_equal(st, &after) || (off_t)n != st->st_size) { errno = EBUSY; goto done; }
    if (!ide_text((unsigned char *)text, n)) { errno = EILSEQ; goto done; }
    text[n] = 0; result = 0;
done: {
    int err = errno; close(fd); errno = err; return result;
    }
}
static int ide_open(ide_state *s, const char *path) {
    if (s->dirty) { snprintf(s->message, sizeof(s->message), "Unsaved edits: Ctrl-S save or Ctrl-X explicitly discard first"); return -1; }
    char name[NAME_MAX+1]; int dir = ide_parent(s, path, name);
    char *candidate = malloc(IDE_LIMIT+1); struct stat st;
    if (!candidate) { if (dir >= 0) close(dir); errno = ENOMEM; ide_error(s, "Open"); return -1; }
    int rc = dir < 0 ? -1 : ide_read_at(dir, name, candidate, &st);
    int err = errno; if (dir >= 0) close(dir); errno = err;
    if (rc) ide_error(s, "Open rejected");
    else {
        strcpy(s->text, candidate); strcpy(s->base, candidate); s->stamp = st;
        snprintf(s->path, sizeof(s->path), "%s", path);
        native_buffer_editor_reset(&s->editor, strlen(s->text));
        native_buffer_editor_set_caret(&s->editor, s->text, 0);
        s->top = s->left = 0; s->browser = false;
        snprintf(s->message, sizeof(s->message), "Opened; Ctrl-] help, Ctrl-S save");
    }
    free(candidate); return rc;
}
static int ide_write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { if (!k) errno = EIO; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int ide_temp(int dir, char *name, const char *prefix) {
    static unsigned sequence;
    for (int i = 0; i < 128; ++i) {
        snprintf(name, NAME_MAX+1, "%s%ld-%u", prefix, (long)getpid(), ++sequence);
        int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST; return -1;
}
static int ide_matches(ide_state *s, int dir, const char *name) {
    char *current = malloc(IDE_LIMIT+1); struct stat st;
    if (!current) { errno = ENOMEM; return -1; }
    int rc = ide_read_at(dir, name, current, &st);
    if (!rc && (!ide_stamp_equal(&s->stamp, &st) || strcmp(s->base, current))) { errno = EBUSY; rc = -1; }
    free(current); return rc;
}
static int ide_save(ide_state *s) {
    if (!*s->path) { snprintf(s->message, sizeof(s->message), "Open a file first"); return -1; }
    char name[NAME_MAX+1], tmp[NAME_MAX+1] = "";
    int dir = ide_parent(s, s->path, name), fd = -1, rc = -1;
    if (dir < 0) goto done;
    if (ide_matches(s, dir, name)) goto done;
    /* Atomic replacement would silently split a hard link. Refuse it. */
    if (s->stamp.st_nlink != 1) { errno = EMLINK; goto done; }
    fd = ide_temp(dir, tmp, ".dsco-ide-save-");
    if (fd < 0) goto done;
    struct stat created;
    if (fstat(fd, &created)) goto done;
    if ((created.st_uid != s->stamp.st_uid || created.st_gid != s->stamp.st_gid) &&
        fchown(fd, s->stamp.st_uid, s->stamp.st_gid)) goto done;
    if (ide_write_all(fd, s->text, strlen(s->text)) || fchmod(fd, s->stamp.st_mode & 07777) || fsync(fd)) goto done;
    /* Revalidate bytes AND inode/metadata immediately before replacement.
     * POSIX rename has no compare-and-swap: hostile concurrent writers still
     * require external coordination in the final check-to-rename window. */
    if (ide_matches(s, dir, name)) goto done;
    char again[NAME_MAX+1]; int check = ide_parent(s, s->path, again);
    struct stat a, b;
    bool same = check >= 0 && !fstat(dir, &a) && !fstat(check, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    if (check >= 0) close(check);
    if (!same) { errno = EBUSY; goto done; }
    if (renameat(dir, tmp, dir, name)) goto done;
    tmp[0] = 0;
    strcpy(s->base, s->text); s->dirty = false; rc = 0;
    if (fstat(fd, &s->stamp)) {
        s->stamp.st_ino = 0; /* Future saves must fail closed. */
        snprintf(s->message, sizeof(s->message), "Saved; metadata unavailable, reopen before next save");
    } else if (fsync(dir)) snprintf(s->message, sizeof(s->message), "Saved atomically; directory sync failed (durability uncertain)");
    else snprintf(s->message, sizeof(s->message), "Saved atomically; source checked and mode preserved");
done: {
    int err = errno;
    if (fd >= 0) close(fd);
    if (*tmp && dir >= 0) unlinkat(dir, tmp, 0);
    if (dir >= 0) close(dir);
    errno = err;
    if (rc) ide_error(s, "Save refused (edits retained)");
    return rc;
    }
}
static int ide_recover(ide_state *s) {
    if (!s->dirty) return 0;
    char name[NAME_MAX+1]; int fd = ide_temp(s->root, name, ".dsco-ide-recovery-");
    if (fd < 0) return -1;
    int rc = ide_write_all(fd, s->text, strlen(s->text));
    if (!rc) rc = fsync(fd);
    int err = errno; close(fd); errno = err;
    if (!rc) { (void)fsync(s->root); fprintf(stderr, "\nUnsaved edits recovered in project file %s\n", name); }
    else fprintf(stderr, "\nRecovery incomplete: project file %s\n", name);
    return rc;
}
static void ide_entries_free(ide_state *s) {
    for (size_t i = 0; i < s->count; ++i) free(s->files[i].name);
    s->count = 0;
}
static int ide_entry_cmp(const void *a, const void *b) {
    const ide_entry *x = a, *y = b;
    return x->dir != y->dir ? (x->dir ? -1 : 1) : strcmp(x->name, y->name);
}
static int ide_list(ide_state *s) {
    int fd = ide_directory(s->root, s->directory);
    if (fd < 0) { ide_error(s, "Directory"); return -1; }
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); ide_error(s, "Directory"); return -1; }
    ide_entries_free(s); s->truncated = false; s->selected = 0;
    struct dirent *ent; int rc = 0;
    /* dup(root) shares directory offsets; rewind before every enumeration. */
    rewinddir(dir);
    for (;;) {
        errno = 0; ent = readdir(dir);
        if (!ent) { if (errno) rc = -1; break; }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (s->count == IDE_ENTRIES) { s->truncated = true; break; }
        struct stat st;
        bool isdir = !fstatat(fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode);
        char *name = strdup(ent->d_name);
        if (!name) { rc = -1; break; }
        s->files[s->count++] = (ide_entry){name, isdir};
    }
    int err = errno; closedir(dir); errno = err;
    qsort(s->files, s->count, sizeof(s->files[0]), ide_entry_cmp);
    if (rc) ide_error(s, "Listing incomplete");
    else snprintf(s->message, sizeof(s->message), "Enter opens; arrows select; Backspace parent; Esc editor%s", s->truncated ? " (listing capped at 2048)" : "");
    return rc;
}
static void ide_json(const char *p) {
    putchar('"');
    for (; *p; ++p) {
        unsigned c = (unsigned char)*p;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 32 || c >= 127) printf("\\u%04x", c);
        else putchar((int)c);
    }
    putchar('"');
}
static int ide_check(ide_state *s) {
    if (ide_list(s)) { printf("{\"ok\":false,\"error\":"); ide_json(s->message); puts("}"); return 1; }
    printf("{\"ok\":true,\"project\":"); ide_json(s->project);
    printf(",\"max_file_bytes\":%u,\"truncated\":%s,\"path_encoding\":\"escaped filesystem bytes\",\"files\":[", IDE_LIMIT, s->truncated ? "true" : "false");
    char *text = malloc(IDE_LIMIT+1);
    if (!text) { puts("],\"error\":\"out of memory\"}"); return 1; }
    for (size_t i = 0; i < s->count; ++i) {
        ide_entry *e = &s->files[i]; struct stat st;
        int rc = e->dir ? -1 : ide_read_at(s->root, e->name, text, &st);
        if (i) putchar(',');
        printf("{\"path\":"); ide_json(e->name);
        printf(",\"directory\":%s,\"editable\":%s", e->dir ? "true" : "false", !rc && st.st_nlink == 1 ? "true" : "false");
        if (rc && !e->dir) { printf(",\"reason\":"); ide_json(strerror(errno)); }
        else if (!rc && st.st_nlink != 1) printf(",\"reason\":\"hard links cannot be saved\"");
        putchar('}');
    }
    free(text); puts("]}"); return ferror(stdout) ? 1 : 0;
}
static void ide_signal(int sig) { ide_stop = sig; }
static int ide_raw(ide_state *s) {
    struct termios t = s->saved;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~OPOST; t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 1;
    return tcsetattr(STDIN_FILENO, TCSANOW, &t);
}
static void ide_terminal_leave(ide_state *s) {
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &s->saved);
    fputs("\033[?2004l\033[?25h\033[0m\033[?1049l", stdout); fflush(stdout);
}
static void ide_terminal_enter(void) { fputs("\033[?1049h\033[?2004h", stdout); }
static int ide_byte(void) {
    unsigned char c; ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return c;
    if (n == 0) {
        /* VTIME timeout is not EOF; POLLHUP is checked by the key reader. */
        return -1;
    }
    if (errno != EINTR && errno != EAGAIN) ide_stop = SIGHUP;
    return -1;
}
#include <poll.h>
static int ide_key(void) {
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    int ready = poll(&p, 1, 100);
    if (ready < 0 && errno != EINTR) ide_stop = SIGHUP;
    if (ready <= 0) return -1;
    if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) { ide_stop = SIGHUP; return -1; }
    int c = ide_byte();
    if (c != 27) return c;
    char seq[24]; size_t n = 0;
    int v = ide_byte();
    if (v != '[' && v != 'O') return 27;
    while (n < sizeof(seq)-1) {
        v = ide_byte(); if (v < 0) break;
        seq[n++] = (char)v;
        if (v >= '@' && v <= '~') break;
    }
    seq[n] = 0;
    if (!strcmp(seq, "A")) return 1001;
    if (!strcmp(seq, "B")) return 1002;
    if (!strcmp(seq, "C")) return 1003;
    if (!strcmp(seq, "D")) return 1004;
    if (!strcmp(seq, "H") || !strcmp(seq, "1~")) return 1005;
    if (!strcmp(seq, "F") || !strcmp(seq, "4~")) return 1006;
    if (!strcmp(seq, "3~")) return 1007;
    if (!strcmp(seq, "5~")) return 1008;
    if (!strcmp(seq, "6~")) return 1009;
    if (!strcmp(seq, "200~")) return 1010;
    if (!strcmp(seq, "201~")) return 1011;
    return -1;
}
/* Display untrusted file names/content without emitting terminal controls.
 * One cell per codepoint: tabs '>', CR '.', non-ASCII '?'. Bytes are retained. */
static void ide_safe(const char *p, size_t width) {
    for (size_t i = 0; p[i] && width; --width) {
        unsigned char c = (unsigned char)p[i];
        putchar(c >= 32 && c < 127 ? c : c == '\t' ? '>' : c == '\r' ? '.' : '?');
        /* File names may contain invalid UTF-8, so advance only one byte here. */
        ++i;
    }
}
static size_t ide_line_start(const char *t, size_t p) { while (p && t[p-1] != '\n') --p; return p; }
static size_t ide_line_end(const char *t, size_t p) { while (t[p] && t[p] != '\n') ++p; return p; }
static size_t ide_column(const char *t, size_t p) {
    size_t n = 0;
    for (size_t i = ide_line_start(t, p); i < p; i = ide_next(t, i)) ++n;
    return n;
}
static void ide_vertical(ide_state *s, int direction) {
    size_t col = ide_column(s->text, s->editor.cursor), p = ide_line_start(s->text, s->editor.cursor);
    if (direction < 0) { if (!p) return; p = ide_line_start(s->text, p-1); }
    else { p = ide_line_end(s->text, p); if (!s->text[p]) return; ++p; }
    while (col-- && s->text[p] && s->text[p] != '\n') p = ide_next(s->text, p);
    native_buffer_editor_set_caret(&s->editor, s->text, p);
}
static void ide_draw(ide_state *s) {
    struct winsize ws = {0};
    (void)ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    s->rows = ws.ws_row >= 6 ? ws.ws_row : 24; s->cols = ws.ws_col >= 20 ? ws.ws_col : 80;
    int height = s->rows - 4, width = s->cols - 8;
    fputs("\033[?25l\033[H\033[2KDSCO IDE | ", stdout);
    ide_safe(s->browser ? s->directory : s->path, (size_t)s->cols-18);
    printf("%s\r\n\033[2K^O files ^P open ^S save ^F find ^] help ^Q quit\r\n", s->dirty ? " *" : "");
    size_t line = 0, col = ide_column(s->text, s->editor.cursor);
    for (size_t i = 0; i < s->editor.cursor; ++i) if (s->text[i] == '\n') ++line;
    if (line < s->top) s->top = line;
    if (line >= s->top + (size_t)height) s->top = line - (size_t)height + 1;
    if (col < s->left) s->left = col;
    if (col >= s->left + (size_t)width) s->left = col - (size_t)width + 1;
    size_t p = 0;
    for (size_t i = 0; i < s->top && s->text[p]; ++i) { p = ide_line_end(s->text, p); if (s->text[p]) ++p; }
    size_t first = s->selected / (size_t)height * (size_t)height;
    for (int r = 0; r < height; ++r) {
        fputs("\033[2K", stdout);
        if (s->browser) {
            size_t i = first + (size_t)r;
            if (i < s->count) { printf("%c %c ", i == s->selected ? '>' : ' ', s->files[i].dir ? '/' : ' '); ide_safe(s->files[i].name, (size_t)s->cols-5); }
        } else {
            printf("%6zu ", s->top + (size_t)r + 1);
            size_t x = 0;
            for (; s->text[p] && s->text[p] != '\n'; p = ide_next(s->text, p), ++x) {
                if (x >= s->left && x < s->left + (size_t)width) ide_safe(s->text+p, 1);
            }
            if (s->text[p]) ++p;
        }
        fputs("\r\n", stdout);
    }
    fputs("\033[2K", stdout); ide_safe(s->message, (size_t)s->cols-1);
    printf("\r\n\033[2K%zu:%zu | %zu bytes | %s", line+1, col+1, strlen(s->text), s->browser ? "FILES" : "EDIT");
    if (!s->browser) printf("\033[%zu;%zuH\033[?25h", line-s->top+3, col-s->left+8);
    fflush(stdout);
}
static bool ide_prompt(ide_state *s, const char *label, char *out, size_t cap) {
    size_t n = 0; out[0] = 0; bool paste = false;
    while (!ide_stop) {
        printf("\033[%d;1H\033[2K", s->rows); ide_safe(label, (size_t)s->cols/2);
        ide_safe(out, (size_t)s->cols/2-1); fputs("\033[?25h", stdout); fflush(stdout);
        int c = ide_key();
        if (c == 1010) { paste = true; continue; }
        if (c == 1011) { paste = false; continue; }
        if (paste && c < 32) continue; /* Pasted newline never executes a command. */
        if (c == 27 || c == 3) return false;
        if (c == '\r' || c == '\n') return n != 0;
        if (c == 127 || c == 8) { if (n) out[--n] = 0; }
        else if (c >= 32 && c < 256 && n+1 < cap) { out[n++] = (char)c; out[n] = 0; }
    }
    return false;
}
static void ide_help(void) {
    puts("Usage: dsco ide [PROJECT] | dsco ide --check PROJECT | dsco ide --help\n"
         "Existing-terminal IDE, one file at a time; no project config is auto-run.\n"
         "Ctrl-O files (Enter open/directory, Backspace parent, Esc editor).\n"
         "Ctrl-P open project-relative path. Ctrl-S save. Ctrl-Q quit (dirty blocks).\n"
         "Ctrl-X type discard to revert edits. Ctrl-Z undo, Ctrl-Y redo.\n"
         "Arrows, Home/End, PgUp/PgDn, Delete, Backspace, Enter, Tab edit/navigate.\n"
         "Ctrl-F literal find, wraps; Ctrl-L go to line. Ctrl-] help.\n"
         "Ctrl-B build / Ctrl-T test: type a shell command; Enter explicitly runs it\n"
         "in the project on SAVED disk files. Blank/Esc cancels. No default command.\n"
         "Ctrl-G: type yes for git diff --no-ext-diff --no-textconv (unstaged).\n"
         "UTF-8 bytes retained; non-ASCII displayed as ?, tab as >, CR as . .\n"
         "256 KiB/file; listing 2048 entries/directory; symlink/binary files rejected.\n"
         "Atomic save checks original bytes/inode/timestamps and preserves POSIX mode;\n"
         "hard-link saves refused. ACLs/xattrs are NOT preserved; no atomic CAS against\n"
         "hostile concurrent writers. No new-file/save-as, LSP, debugger, split panes.\n"
         "HUP/TERM/INT/QUIT/TSTP or terminal loss exits with 0600 recovery in project\n"
         "for dirty text; disk failure/SIGKILL cannot guarantee recovery.\n"
         "--check is read-only, nonrecursive JSON; it neither edits nor runs commands.");
}
static void ide_run(ide_state *s, const char *command, bool git) {
    ide_terminal_leave(s);
    fflush(NULL);
    pid_t child = fork(); int status = 0;
    if (!child) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        if (fchdir(s->root)) _exit(126);
        if (git) execl("/usr/bin/git", "git", "--no-pager", "-c", "core.fsmonitor=false", "diff", "--no-ext-diff", "--no-textconv", "--", (char *)NULL);
        else execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    if (child < 0) ide_error(s, "Command fork");
    else {
        pid_t got;
        do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
        if (got < 0) ide_error(s, "Command wait");
        else snprintf(s->message, sizeof(s->message), "Command %s %d; buffer not auto-saved", WIFEXITED(status) ? "exit" : "signal", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
    }
    if (!ide_stop) {
        fputs("\nPress Enter to return to IDE...", stdout); fflush(stdout);
        char c;
        while (!ide_stop && read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {}
    }
    if (ide_raw(s)) ide_stop = SIGHUP;
    ide_terminal_enter();
}
static void ide_browse_enter(ide_state *s) {
    if (!s->count) return;
    ide_entry *e = &s->files[s->selected]; char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s%s%s", s->directory, *s->directory ? "/" : "", e->name);
    if (n < 0 || (size_t)n >= sizeof(path)) { errno = ENAMETOOLONG; ide_error(s, "Path"); return; }
    if (!e->dir) { (void)ide_open(s, path); return; }
    int fd = ide_directory(s->root, path);
    if (fd < 0) { ide_error(s, "Directory rejected"); return; }
    close(fd); strcpy(s->directory, path); (void)ide_list(s);
}
static int ide_ui(ide_state *s) {
    if (tcgetattr(STDIN_FILENO, &s->saved)) { perror("ide termios"); return 1; }
    const int signals[] = {SIGHUP, SIGTERM, SIGINT, SIGQUIT, SIGTSTP};
    struct sigaction old[5], action; memset(&action, 0, sizeof(action));
    action.sa_handler = ide_signal; sigemptyset(&action.sa_mask); ide_stop = 0;
    size_t installed = 0;
    for (; installed < 5; ++installed) if (sigaction(signals[installed], &action, &old[installed])) break;
    if (installed < 5 || ide_raw(s)) {
        while (installed) { --installed; sigaction(signals[installed], &old[installed], NULL); }
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &s->saved); return 1;
    }
    ide_terminal_enter(); s->browser = true; (void)ide_list(s);
    bool quit = false, paste = false;
    while (!quit && !ide_stop) {
        ide_draw(s); int key = ide_key();
        if (key == 1010) { paste = true; continue; }
        if (key == 1011) { paste = false; continue; }
        if (paste) {
            if (!s->browser && *s->path && key >= 0 && key < 256 && (key >= 32 || key == '\t' || key == '\n' || key == '\r')) {
                native_buffer_editor_feed(&s->editor, s->text, sizeof(s->text), (unsigned char)(key == '\r' ? '\n' : key));
                s->dirty = strcmp(s->base, s->text) != 0;
            }
            continue;
        }
        char input[PATH_MAX];
        if (key == 17) {
            if (s->dirty) snprintf(s->message, sizeof(s->message), "Unsaved edits: Ctrl-S save, Ctrl-X explicitly discard; quit canceled");
            else quit = true;
        } else if (key == 19) (void)ide_save(s);
        else if (key == 15) { s->browser = !s->browser; if (s->browser) (void)ide_list(s); }
        else if (key == 16) { if (ide_prompt(s, "Open relative path: ", input, sizeof(input))) (void)ide_open(s, input); }
        else if (key == 24) {
            if (s->dirty && ide_prompt(s, "Type discard: ", input, sizeof(input)) && !strcmp(input, "discard")) {
                strcpy(s->text, s->base); s->dirty = false;
                native_buffer_editor_reset(&s->editor, strlen(s->text));
                s->top = s->left = 0;
            }
        } else if (key == 29) {
            ide_terminal_leave(s); ide_help(); fputs("Press Enter to return...", stdout); fflush(stdout);
            char c; while (!ide_stop && read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {}
            if (ide_raw(s)) ide_stop = SIGHUP;
            ide_terminal_enter();
        } else if (key == 2 || key == 20) {
            if (ide_prompt(s, key == 2 ? "Build shell command: " : "Test shell command: ", input, sizeof(input))) ide_run(s, input, false);
        } else if (key == 7) {
            if (ide_prompt(s, "Run git diff? type yes: ", input, sizeof(input)) && !strcmp(input, "yes")) ide_run(s, NULL, true);
        } else if (key == 6 && *s->path) {
            if (ide_prompt(s, "Find literal: ", input, sizeof(input)) && ide_text((unsigned char *)input, strlen(input))) {
                char *p = strstr(s->text + ide_next(s->text, s->editor.cursor), input);
                if (!p) p = strstr(s->text, input);
                if (p) { native_buffer_editor_set_caret(&s->editor, s->text, (size_t)(p-s->text)); s->browser = false; }
                else snprintf(s->message, sizeof(s->message), "Not found");
            }
        } else if (key == 12 && *s->path) {
            if (ide_prompt(s, "Line number: ", input, sizeof(input))) {
                char *end; errno = 0; unsigned long n = strtoul(input, &end, 10);
                if (!errno && !*end && n && input[0] != '-') {
                    size_t p = 0; while (--n && s->text[p]) { p = ide_line_end(s->text, p); if (s->text[p]) ++p; }
                    native_buffer_editor_set_caret(&s->editor, s->text, p); s->browser = false;
                } else snprintf(s->message, sizeof(s->message), "Invalid line number");
            }
        } else if (s->browser) {
            if (key == 1001 && s->selected) --s->selected;
            else if (key == 1002 && s->selected + 1 < s->count) ++s->selected;
            else if (key == 1008) s->selected = s->selected > 10 ? s->selected-10 : 0;
            else if (key == 1009 && s->count) s->selected = s->selected+10 < s->count ? s->selected+10 : s->count-1;
            else if (key == '\r' || key == '\n') ide_browse_enter(s);
            else if (key == 27) s->browser = false;
            else if (key == 127 || key == 8) { char *p = strrchr(s->directory, '/'); if (p) *p = 0; else s->directory[0] = 0; (void)ide_list(s); }
        } else if (*s->path) {
            if (key == 1001 || key == 1002) ide_vertical(s, key == 1001 ? -1 : 1);
            else if (key == 1003) native_buffer_editor_right(&s->editor, s->text);
            else if (key == 1004) native_buffer_editor_left(&s->editor, s->text);
            else if (key == 1005 || key == 1) native_buffer_editor_set_caret(&s->editor, s->text, ide_line_start(s->text, s->editor.cursor));
            else if (key == 1006 || key == 5) native_buffer_editor_set_caret(&s->editor, s->text, ide_line_end(s->text, s->editor.cursor));
            else if (key == 1008 || key == 1009) { for (int i = 0; i < s->rows-4; ++i) ide_vertical(s, key == 1008 ? -1 : 1); }
            else if (key == 26) native_buffer_editor_undo(&s->editor, s->text, sizeof(s->text));
            else if (key == 25) native_buffer_editor_redo(&s->editor, s->text, sizeof(s->text));
            else if (key == 127 || key == 8) native_buffer_editor_backspace(&s->editor, s->text);
            else if (key == 1007) { if (native_buffer_editor_right(&s->editor, s->text)) native_buffer_editor_backspace(&s->editor, s->text); }
            else if (key == '\r' || key == '\n') native_buffer_editor_insert(&s->editor, s->text, sizeof(s->text), "\n", 1);
            else if (key == '\t' || (key >= 32 && key < 256)) {
                if (!native_buffer_editor_feed(&s->editor, s->text, sizeof(s->text), (unsigned char)key) && strlen(s->text) >= IDE_LIMIT-4)
                    snprintf(s->message, sizeof(s->message), "File size limit; insertion refused");
            }
            s->dirty = strcmp(s->base, s->text) != 0;
        }
    }
    ide_terminal_leave(s);
    int result = ide_stop ? 128 + ide_stop : 0;
    if (s->dirty && ide_recover(s)) { perror("ide: unable to fully recover unsaved edits"); result = 1; }
    while (installed) { --installed; sigaction(signals[installed], &old[installed], NULL); }
    return result;
}
int ide_cli(int argc, char **argv) {
    if (!argv || argc < 2 || !argv[1] || strcmp(argv[1], "ide")) { ide_help(); return 2; }
    if (argc == 3 && (!strcmp(argv[2], "--help") || !strcmp(argv[2], "-h"))) { ide_help(); return 0; }
    bool check = argc >= 3 && !strcmp(argv[2], "--check");
    if ((check && argc != 4) || (!check && (argc > 3 || (argc == 3 && argv[2][0] == '-')))) {
        fputs("ide: use dsco ide [PROJECT] or dsco ide --check PROJECT\n", stderr); return 2;
    }
    const char *project = argc == 2 ? "." : argv[check ? 3 : 2];
    ide_state *s = calloc(1, sizeof(*s));
    if (!s) return 1;
    s->root = -1; struct stat st;
    /* Caller selects the project explicitly. Resolve its parent spelling once;
     * final project symlinks and all descendant symlinks are rejected. */
    if (lstat(project, &st) || !S_ISDIR(st.st_mode) || !realpath(project, s->project) ||
        (s->root = open(s->project, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
        if (check) puts("{\"ok\":false,\"error\":\"project must be an accessible non-symlink directory\"}");
        else fputs("ide: project must be an accessible non-symlink directory\n", stderr);
        free(s); return 1;
    }
    int result;
    if (check) result = ide_check(s);
    else if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("ide: interactive mode requires an existing input/output terminal; use --check PROJECT\n", stderr); result = 2;
    } else result = ide_ui(s);
    native_buffer_editor_dispose(&s->editor); ide_entries_free(s); close(s->root); free(s);
    return result;
}
