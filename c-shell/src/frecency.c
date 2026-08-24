/* frecency.c - persistent frequency + recency store for `hop`.
 *
 * Each directory keeps a rank (incremented on every visit) and the time of its
 * last visit.  The score shown to the lookup is the rank scaled by an age
 * bucket, exactly the shape zoxide uses:
 *
 *   visited < 1 hour ago  -> rank * 4      < 1 day  -> rank * 2
 *   < 1 week             -> rank / 2       older    -> rank / 4
 *
 * Ties are broken lexicographically so the result is fully deterministic.
 * The store lives in the directory the shell was started from, so it survives
 * across sessions launched there.
 */
#include "frecency.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pathutil.h"
#include "shell.h"
#include "utils.h"

#define FRECENCY_FILE ".cshell_frecency"

typedef struct {
    char  *path;
    double rank;
    long   last;
} Entry;

static Entry *entries;
static size_t n_entries;
static size_t cap_entries;
static int    loaded;

static char *store_path(void)
{
    return path_join(g_shell.home, FRECENCY_FILE);
}

static void entries_push(char *path, double rank, long last)
{
    if (n_entries == cap_entries) {
        cap_entries = cap_entries ? cap_entries * 2 : 16;
        entries = xrealloc(entries, cap_entries * sizeof *entries);
    }
    entries[n_entries].path = path;
    entries[n_entries].rank = rank;
    entries[n_entries].last = last;
    n_entries++;
}

void frecency_init(void)
{
    char *file;
    FILE *f;
    char  line[PATH_MAX + 64];

    if (loaded)
        return;
    loaded = 1;

    file = store_path();
    f = fopen(file, "r");
    free(file);
    if (f == NULL)
        return;

    while (fgets(line, sizeof line, f) != NULL) {
        char  *tab1, *tab2;
        double rank;
        long   last;

        line[strcspn(line, "\n")] = '\0';
        tab1 = strchr(line, '\t');
        if (tab1 == NULL)
            continue;
        *tab1 = '\0';
        tab2 = strchr(tab1 + 1, '\t');
        if (tab2 == NULL)
            continue;
        *tab2 = '\0';

        rank = strtod(line, NULL);
        last = strtol(tab1 + 1, NULL, 10);
        if (tab2[1] == '\0')
            continue;
        entries_push(xstrdup(tab2 + 1), rank, last);
    }
    fclose(f);
}

static void frecency_save(void)
{
    char *file = store_path();
    FILE *f    = fopen(file, "w");

    free(file);
    if (f == NULL)
        return; /* a read-only home simply means no persistence */
    for (size_t i = 0; i < n_entries; i++)
        fprintf(f, "%.4f\t%ld\t%s\n", entries[i].rank, entries[i].last,
                entries[i].path);
    fclose(f);
}

void frecency_record(const char *abs_path)
{
    long now = (long)time(NULL);

    frecency_init();
    for (size_t i = 0; i < n_entries; i++) {
        if (strcmp(entries[i].path, abs_path) == 0) {
            entries[i].rank += 1.0;
            entries[i].last = now;
            frecency_save();
            return;
        }
    }
    entries_push(xstrdup(abs_path), 1.0, now);
    frecency_save();
}

static double score_of(const Entry *e, long now)
{
    long dt = now - e->last;

    if (dt < 0)
        dt = 0;
    if (dt < 3600)
        return e->rank * 4.0;
    if (dt < 86400)
        return e->rank * 2.0;
    if (dt < 604800)
        return e->rank / 2.0;
    return e->rank / 4.0;
}

typedef struct {
    const char *path;
    double      score;
} Ranked;

static int cmp_ranked(const void *a, const void *b)
{
    const Ranked *x = a, *y = b;

    if (x->score > y->score)
        return -1;
    if (x->score < y->score)
        return 1;
    return strcmp(x->path, y->path); /* deterministic tie break */
}

char *frecency_lookup(const char *needle)
{
    long    now = (long)time(NULL);
    Ranked *cand;
    size_t  n = 0;
    char   *result = NULL;

    frecency_init();
    if (n_entries == 0)
        return NULL;

    cand = xmalloc(n_entries * sizeof *cand);
    for (size_t i = 0; i < n_entries; i++) {
        if (strstr(entries[i].path, needle) != NULL) {
            cand[n].path  = entries[i].path;
            cand[n].score = score_of(&entries[i], now);
            n++;
        }
    }
    if (n > 1)
        qsort(cand, n, sizeof *cand, cmp_ranked);

    /* Skip directories that have since been removed from disk. */
    for (size_t i = 0; i < n; i++) {
        if (path_is_dir(cand[i].path)) {
            result = xstrdup(cand[i].path);
            break;
        }
    }
    free(cand);
    return result;
}

void frecency_shutdown(void)
{
    for (size_t i = 0; i < n_entries; i++)
        free(entries[i].path);
    free(entries);
    entries     = NULL;
    n_entries   = 0;
    cap_entries = 0;
}
