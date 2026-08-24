/* pathutil.h - path resolution shared by hop, reveal, locate and exec. */
#ifndef CSHELL_PATHUTIL_H
#define CSHELL_PATHUTIL_H

/* Resolve the special hop/reveal arguments.
 *   "~" or "~/x" -> shell home        "."  -> current directory
 *   ".."         -> parent            "-"  -> previous directory
 *   anything else is returned unchanged (relative paths stay relative).
 * Returns a malloc'd string, or NULL when "-" is used before any hop. */
char *path_resolve_special(const char *arg);

/* Current working directory as a malloc'd absolute path. */
char *path_getcwd(void);

/* Join dir and name into a malloc'd "dir/name" (no double slash). */
char *path_join(const char *dir, const char *name);

/* Make `path` absolute against the cwd if it is not already. */
char *path_absolute(const char *path);

/* 1 if `path` is a directory. */
int path_is_dir(const char *path);

/* 1 if `path` is a regular (or link to a) file executable by this user. */
int path_is_executable(const char *path);

#endif /* CSHELL_PATHUTIL_H */
