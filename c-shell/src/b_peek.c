/* b_peek.c - `peek`, a cat with line numbering and reverse output.
 *
 * Syntax: peek (-(n|r)*)* filename*
 *   -n  prefix every non-empty line with its number (numbering is continuous
 *       across all files and stays tied to a line's original position)
 *   -r  print each file's lines in reverse order
 * Nothing is ever slurped into memory for a regular file: forward reads walk
 * fixed size chunks, and -r walks the file backwards with lseek.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "builtins.h"
#include "utils.h"

#define PEEK_CHUNK 4096

/* Running count of non-empty lines already printed, shared across the files
 * of one peek invocation. */
static int g_seen;

static void emit_line(const char *s, size_t n, int number, int lineno)
{
    if (number && n > 0)
        printf("%d ", lineno);
    if (n > 0)
        fwrite(s, 1, n, stdout);
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* forward reading                                                     */
/* ------------------------------------------------------------------ */

static void forward_stream(int fd, int number)
{
    char    buf[PEEK_CHUNK];
    ssize_t r;
    int     at_start = 1;

    while ((r = read(fd, buf, sizeof buf)) > 0) {
        if (!number) {
            fwrite(buf, 1, (size_t)r, stdout);
            continue;
        }
        for (ssize_t i = 0; i < r; i++) {
            if (at_start) {
                if (buf[i] != '\n')
                    printf("%d ", ++g_seen);
                at_start = 0;
            }
            putchar(buf[i]);
            if (buf[i] == '\n')
                at_start = 1;
        }
    }
}

/* Count non-empty lines from the current offset, in fixed size chunks. */
static int count_nonempty(int fd)
{
    char    buf[PEEK_CHUNK];
    ssize_t r;
    int     at_start = 1, count = 0;

    while ((r = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            if (at_start) {
                if (buf[i] != '\n')
                    count++;
                at_start = 0;
            }
            if (buf[i] == '\n')
                at_start = 1;
        }
    }
    return count;
}

static int count_nonempty_buf(const char *data, size_t len)
{
    int at_start = 1, count = 0;

    for (size_t i = 0; i < len; i++) {
        if (at_start) {
            if (data[i] != '\n')
                count++;
            at_start = 0;
        }
        if (data[i] == '\n')
            at_start = 1;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* reverse reading                                                     */
/* ------------------------------------------------------------------ */

static int read_full(int fd, char *buf, size_t n)
{
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Walk a seekable file from the end, one PEEK_CHUNK at a time, emitting whole
 * lines as their leading newline comes into view.  `pending` holds the part of
 * a line that has already been seen but whose start is still further left. */
static void reverse_file(int fd, int number)
{
    off_t size = lseek(fd, 0, SEEK_END);
    off_t scan, off;
    Buf   pending;
    char  chunk[PEEK_CHUNK];
    int   total = 0, num = 0;
    char  last;

    if (size <= 0)
        return;

    if (number) {
        if (lseek(fd, 0, SEEK_SET) < 0)
            return;
        total = count_nonempty(fd);
    }
    num = g_seen + total; /* the last non-empty line's number */
    g_seen += total;

    /* A trailing newline terminates the final line; it is not a line itself. */
    scan = size;
    if (lseek(fd, size - 1, SEEK_SET) >= 0 && read_full(fd, &last, 1) == 0 &&
        last == '\n')
        scan = size - 1;

    buf_init(&pending);
    off = scan;
    while (off > 0) {
        size_t n = (off > (off_t)sizeof chunk) ? sizeof chunk : (size_t)off;
        size_t j;

        off -= (off_t)n;
        if (lseek(fd, off, SEEK_SET) < 0)
            break;
        if (read_full(fd, chunk, n) != 0)
            break;

        j = n; /* exclusive right edge of the line currently being assembled */
        for (size_t k = n; k-- > 0;) {
            if (chunk[k] != '\n')
                continue;
            if (j == n) {
                /* This line runs off the right edge of the chunk, so it is
                 * completed by whatever is already pending. */
                Buf line;
                buf_init(&line);
                buf_append(&line, chunk + k + 1, n - (k + 1));
                buf_append(&line, pending.data, pending.len);
                emit_line(line.data, line.len, number, num);
                if (line.len > 0)
                    num--;
                buf_free(&line);
                pending.len     = 0;
                pending.data[0] = '\0';
            } else {
                emit_line(chunk + k + 1, j - (k + 1), number, num);
                if (j - (k + 1) > 0)
                    num--;
            }
            j = k;
        }

        if (j > 0) {
            /* chunk[0, j) is the tail of a line that starts further left. */
            Buf np;
            buf_init(&np);
            buf_append(&np, chunk, j);
            buf_append(&np, pending.data, pending.len);
            buf_free(&pending);
            pending = np;
        }
    }

    /* Whatever survives is the first line of the file. */
    emit_line(pending.data, pending.len, number, num);
    buf_free(&pending);
}

/* Non-seekable input (a pipe or the terminal) may be buffered whole. */
static void reverse_stream(int fd, int number)
{
    Buf     all;
    char    chunk[PEEK_CHUNK];
    ssize_t r;
    size_t  scan, end;
    int     total, num;

    buf_init(&all);
    while ((r = read(fd, chunk, sizeof chunk)) > 0)
        buf_append(&all, chunk, (size_t)r);

    if (all.len == 0) {
        buf_free(&all);
        return;
    }

    scan = all.len;
    if (all.data[scan - 1] == '\n')
        scan--;

    total = number ? count_nonempty_buf(all.data, all.len) : 0;
    num   = g_seen + total;
    g_seen += total;

    end = scan;
    for (size_t k = scan; k-- > 0;) {
        if (all.data[k] != '\n')
            continue;
        emit_line(all.data + k + 1, end - (k + 1), number, num);
        if (end - (k + 1) > 0)
            num--;
        end = k;
    }
    emit_line(all.data, end, number, num);
    buf_free(&all);
}

/* ------------------------------------------------------------------ */

static int is_seekable(int fd)
{
    struct stat st;

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return lseek(fd, 0, SEEK_CUR) >= 0;
}

static void peek_fd(int fd, int number, int reverse)
{
    if (!reverse)
        forward_stream(fd, number);
    else if (is_seekable(fd))
        reverse_file(fd, number);
    else
        reverse_stream(fd, number);
}

static int peek_file(const char *name, int number, int reverse)
{
    struct stat st;
    int         fd;

    if (strcmp(name, "-") == 0) { /* '-' means standard input */
        peek_fd(STDIN_FILENO, number, reverse);
        return 0;
    }

    if (stat(name, &st) != 0) {
        err_printf("peek: no such file or directory");
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        err_printf("peek: is a directory");
        return 1;
    }

    fd = open(name, O_RDONLY);
    if (fd < 0) {
        err_printf("peek: no such file or directory");
        return 1;
    }
    peek_fd(fd, number, reverse);
    close(fd);
    return 0;
}

int builtin_peek(int argc, char **argv)
{
    int    number = 0, reverse = 0, status = 0;
    StrVec files;

    g_seen = 0;
    strvec_init(&files);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        /* A lone "-" is standard input, not a flag. */
        if (a[0] == '-' && a[1] != '\0') {
            if (files.len > 0) { /* flags must precede the file names */
                err_printf("peek: invalid syntax");
                strvec_free(&files);
                return 1;
            }
            for (const char *p = a + 1; *p != '\0'; p++) {
                if (*p == 'n')
                    number = 1;
                else if (*p == 'r')
                    reverse = 1;
                else {
                    err_printf("peek: invalid syntax");
                    strvec_free(&files);
                    return 1;
                }
            }
        } else {
            strvec_push(&files, xstrdup(a));
        }
    }

    if (files.len == 0) {
        peek_fd(STDIN_FILENO, number, reverse);
    } else {
        for (size_t i = 0; i < files.len; i++) {
            if (peek_file(files.items[i], number, reverse) != 0)
                status = 1;
        }
    }

    fflush(stdout);
    strvec_free(&files);
    return status;
}
