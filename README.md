# OSN Mini Project 1

**Name:** athmeeyakashyap
**Roll number:** 2024113015

Two parts: a POSIX shell written from scratch in C, and the xv6 assignment.

## Structure

```
mini-project1/
├── c-shell/
│   ├── src/
│   ├── include/
│   └── Makefile
├── xv6/
│   ├── user/
│   ├── mkfs/
│   ├── kernel/
│   ├── Makefile
│   └── report.md
└── README.md
```

## c-shell

The Makefile in `c-shell` builds `shell.out` with the flags the specification
asks for, including `-Wall -Wextra -Werror`, and the code builds clean under
them. GCC only accepts the spelling `-std=c23` from version 14 onwards and older
versions want `-std=c2x` for the same standard, so the Makefile checks which one
the compiler understands and uses that.

The directory the shell is started in is treated as its home. That is what `~`
means in the prompt, in `hop` and in `reveal`. It is not `$HOME`.

### Part A: input

The prompt shows the user, the host and the working directory. The path is shown
as `~` when the working directory is the home directory, and the home prefix is
replaced by `~` when the working directory sits below it. Anywhere else the full
path is printed. The hostname is cut at the first dot. Input is read one line at
a time, up to 1024 characters.

Lexing is done in a single left to right pass with a small state machine, one
state for each context: between tokens, inside a word, inside single quotes,
inside double quotes, and after a backslash. Quotes and escapes are resolved
during that pass, so fragments that touch each other join into one word and
`abc"123"'def'` comes out as the single word `abc123def`. A backslash outside
quotes always takes the next character literally, inside double quotes only `\"`
and `\\` are escapes, and inside single quotes nothing is escaped. A line that
ends inside a quote or right after a backslash is a syntax error, since there is
no line continuation.

Parsing walks the token list once and checks it against the grammar. Because the
grammar is right linear, no stack is needed and the current position in the
grammar is just a variable. Each position accepts only the tokens its rule
allows, so a line ending in a separator, a redirection with no file after it, and
a line starting with a pipe are all rejected, while a line ending in `&` is
allowed. What comes out is a list of pipelines, each holding its commands, each
with its own redirections. Nothing runs until the whole line has been validated,
so an invalid line has no side effects.

### Part B: intrinsics

`hop` changes directory. Arguments are applied one at a time, so three arguments
mean three separate moves. It handles `~`, `.`, `..`, `-`, and relative or
absolute paths. If a name does not exist on disk, `hop` falls back to a store of
previously visited directories and jumps to the best ranked one whose path
contains that name. Ranking uses frequency and recency together: every visit adds
one to a directory's count, and that count is then scaled up or down by how long
ago the directory was last used, so a directory visited in the last hour beats
one last used a month ago. Ties break lexicographically so the result is
deterministic. The store is a text file, `.cshell_frecency`, kept in the shell's
home directory, so it survives between sessions.

`reveal` lists a directory. `-a` shows dotfiles but never `.` and `..`, which
also keeps the recursive listing from looping. `-t` walks the tree depth first,
printing each directory before its contents and marking directories with a
trailing slash. Sorting is done on the plain name, before the slash is added.
Entries are checked with `lstat` rather than `stat`, so a symbolic link to a
directory is listed as an ordinary entry and is not descended into, and a link
pointing at its own ancestor cannot hang the listing. Flags can be repeated and
split across arguments but have to come before the path.

`peek` prints files. `-n` numbers the non empty lines, and the count carries on
across all the files given rather than restarting. `-r` prints a file's lines in
reverse while keeping each line's original number, so reversing changes only the
order they appear in. Regular files are never read whole. Forward reading, line
counting and the backward pass all work in 4 KiB chunks. The backward pass seeks
through the file from the end, and since a chunk boundary usually lands in the
middle of a line it keeps one spare buffer holding the part of a line whose start
is still further left in the file. Memory use is one chunk plus one line whatever
the file size. Only pipes and the terminal are buffered whole, since they cannot
be seeked.

`locate` prints every executable of a given name rather than just the one that
would run. The working directory is searched first, then each `PATH` entry in
order, without recursing into subdirectories. Executability is tested with
`access(path, X_OK)` rather than by reading the permission bits, since that
answers the question for the current user, and directories are skipped.

### Part C: redirection and pipes

The shell resolves command names itself and then calls `execv`, rather than
handing the name to `execvp`, because the rules differ from the C library's. A
name containing a slash is used as a path, a bare name is looked for in the
working directory first and then in `PATH`, and a `%` prefix forces the `PATH`
lookup and skips the working directory. Scripts with a shebang need no special
handling, since the kernel recognises it and starts the interpreter.

Input files are opened read only, and output files are created if needed, then
truncated for `>` or appended to for `>>`. Inputs are opened before outputs, so a
missing input never truncates an output file on the way. If any file cannot be
opened the command does not run at all, instead of running with a partial set of
descriptors.

A single redirection in either direction is a plain `dup2`. Several inputs are
concatenated in the order given into one temporary file which the command then
reads, which is the one continuous stream the specification asks for. Several
outputs are handled the other way round: the command writes into a temporary
file, and once it has finished that file is copied into each destination, so each
destination keeps its own truncate or append behaviour. The temporary files are
made with `mkstemp` and unlinked straight away, so they never show up in a
listing and the data stays alive only through the open descriptor.

Pipes use one `pipe()` per `|` and one `fork()` per stage. All the pipes are
created before any fork, so every child inherits every descriptor and can then
close what it does not need. The parent closes all of them after forking; if it
kept a write end open, the reader on the other side would never see end of file
and the shell would hang. Redirections are applied after the pipe wiring, so an
explicit `>` overrides the pipe. A stage whose command cannot be found reports it
and exits with status 127, and the remaining stages still run. An intrinsic on
its own runs inside the shell itself, with its redirections installed on the
shell's own descriptors and restored afterwards, because otherwise `hop` would
change the directory of a child that is about to exit. Inside a pipeline an
intrinsic runs in the child like any other command.

When a line contains `;` or `&`, only the first command group runs. The rest is
parsed and validated but ignored, as the specification requires for this part.

### Source layout

| File | Does |
| --- | --- |
| `src/main.c` | entry point |
| `src/shell.c` | read, lex, parse, execute loop |
| `src/prompt.c` | the prompt |
| `src/lexer.c` | line to tokens |
| `src/parser.c` | tokens to pipelines and commands |
| `src/builtins.c` | intrinsic dispatch |
| `src/b_hop.c`, `src/b_reveal.c`, `src/b_peek.c`, `src/b_locate.c` | one intrinsic each |
| `src/frecency.c` | the frequency and recency store used by `hop` |
| `src/redirect.c` | opening redirection targets |
| `src/exec.c` | command lookup, fork, pipes, wait |
| `src/pathutil.c`, `src/utils.c` | shared helpers |

## xv6

See [xv6/report.md](xv6/report.md).

## Notes

* Errors go to standard error and ordinary output goes to standard output.
* `Ctrl-C` and `Ctrl-\` are ignored by the shell and restored in children, so an
  interrupt kills the running command instead of the shell.
* An empty or whitespace only line is valid and just reprints the prompt.
