/* frecency.h - persistent frequency+recency ranking of hopped directories.
 *
 * Modelled on zoxide: every visit adds 1 to a directory's rank, and the rank is
 * scaled by how long ago it was last visited. The store lives next to the
 * shell's home directory so it survives across sessions launched there. */
#ifndef CSHELL_FRECENCY_H
#define CSHELL_FRECENCY_H

/* Load the store from disk (idempotent, called lazily). */
void frecency_init(void);

/* Record one successful hop into `abs_path` and persist the store. */
void frecency_record(const char *abs_path);

/* Highest ranked existing directory whose path contains `needle`.
 * Returns a malloc'd absolute path, or NULL when nothing matches. */
char *frecency_lookup(const char *needle);

void frecency_shutdown(void);

#endif /* CSHELL_FRECENCY_H */
