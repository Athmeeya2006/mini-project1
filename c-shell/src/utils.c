#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL) {
        fputs("cshell: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (q == NULL) {
        fputs("cshell: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

long parse_nonneg(const char *s)
{
    long v = 0;

    if (s == NULL || s[0] == '\0')
        return -1;
    /* Digits and nothing else: strtol would also take a sign or leading
     * blanks, which are not part of a plain number. */
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        if (v > (LONG_MAX - (*p - '0')) / 10)
            return -1; /* too big to represent */
        v = v * 10 + (*p - '0');
    }
    return v;
}

int is_digits(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return 0;
    for (const char *p = s; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

void buf_init(Buf *b)
{
    b->cap  = 32;
    b->len  = 0;
    b->data = xmalloc(b->cap);
    b->data[0] = '\0';
}

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return;
    while (b->len + extra + 1 > b->cap)
        b->cap *= 2;
    b->data = xrealloc(b->data, b->cap);
}

void buf_push(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
}

void buf_append(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

char *buf_release(Buf *b)
{
    char *s = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void strvec_init(StrVec *v)
{
    v->cap   = 8;
    v->len   = 0;
    v->items = xmalloc(v->cap * sizeof *v->items);
}

void strvec_push(StrVec *v, char *owned)
{
    if (v->len == v->cap) {
        v->cap *= 2;
        v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    }
    v->items[v->len++] = owned;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void strvec_sort(StrVec *v)
{
    if (v->len > 1)
        qsort(v->items, v->len, sizeof *v->items, cmp_str);
}

void strvec_free(StrVec *v)
{
    for (size_t i = 0; i < v->len; i++)
        free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->len = v->cap = 0;
}

void err_printf(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;

    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int copy_fd(int src, int dst)
{
    char buf[4096];

    for (;;) {
        ssize_t r = read(src, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return 0;
        if (write_all(dst, buf, (size_t)r) < 0)
            return -1;
    }
}

int make_temp_fd(void)
{
    char template[] = "/tmp/cshell-spool-XXXXXX";
    int  fd = mkstemp(template);

    if (fd < 0)
        return -1;
    /* Unlink straight away: the fd keeps the file alive until we close it. */
    unlink(template);
    return fd;
}
