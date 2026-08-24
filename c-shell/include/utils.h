/* utils.h - small allocation, string and I/O helpers used everywhere. */
#ifndef CSHELL_UTILS_H
#define CSHELL_UTILS_H

#include <stddef.h>

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Growable NUL-terminated character buffer. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_push(Buf *b, char c);
void  buf_append(Buf *b, const char *s, size_t n);
char *buf_release(Buf *b); /* hand ownership of the string to the caller */
void  buf_free(Buf *b);

/* Growable vector of owned strings. */
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrVec;

void  strvec_init(StrVec *v);
void  strvec_push(StrVec *v, char *owned);
void  strvec_sort(StrVec *v); /* lexicographic, plain ASCII (strcmp) */
void  strvec_free(StrVec *v);

/* Print "cshell: ..." style diagnostics on stderr. */
void err_printf(const char *fmt, ...);

/* Write exactly n bytes, retrying on short writes. Returns 0 or -1. */
int write_all(int fd, const void *buf, size_t n);

/* Copy everything readable from `src` to `dst`. Returns 0 or -1. */
int copy_fd(int src, int dst);

/* Create an unlinked temporary file and return its fd (-1 on failure). */
int make_temp_fd(void);

#endif /* CSHELL_UTILS_H */
