# OSN Mini Project 1 — C-Shell

**Name:** athmeeyakashyap
**Roll number:** 2024113015

A POSIX shell written from scratch in C. This repository currently implements
**Part A (shell input)**, **Part B (shell intrinsics)** and **Part C (file
redirection and pipes)** of the C-Shell specification.

---

## Building and running

```sh
cd c-shell
make all       # produces ./shell.out in the same directory
./shell.out
make clean     # removes build/ and shell.out
```

The build uses exactly the flags required by the specification:

```
-std=c23 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
-Wall -Wextra -Werror -Wno-unused-parameter -fno-asm
```

GCC only accepts the spelling `-std=c23` from version 14 onwards; on older
toolchains the identical `-std=c2x` is used instead. The Makefile detects this
automatically, so `make all` works on both. The code compiles with zero
warnings under `-Werror`.

Press `Ctrl-D` (or type `exit`) to leave the shell.

---

## Repository layout

```
mini-project1/
├── c-shell/
│   ├── include/         # one header per subsystem
│   ├── src/             # one .c per subsystem
│   └── Makefile
├── xv6/                 # (end-submission)
└── README.md
```

### Source files

| File | Responsibility |
| --- | --- |
| `src/main.c` | entry point |
| `src/shell.c` | global shell state, the read → lex → parse → execute loop |
| `src/prompt.c` | renders `<username@hostname:path>` |
| `src/lexer.c` | character-level DFA producing tokens |
| `src/parser.c` | validates the token stream against the grammar |
| `src/builtins.c` | dispatch table for the intrinsics |
| `src/b_hop.c` | `hop` |
| `src/b_reveal.c` | `reveal` |
| `src/b_peek.c` | `peek` |
| `src/b_locate.c` | `locate` |
| `src/frecency.c` | persistent frequency + recency store used by `hop` |
| `src/redirect.c` | opens the files named by `<`, `>` and `>>` |
| `src/exec.c` | command resolution, forking, pipes, waiting |
| `src/pathutil.c` | path resolution helpers shared by several commands |
| `src/utils.c` | allocation, growable buffers, small I/O helpers |

---

## Part A — Shell input

### A1/A2 Prompt and input

The directory the shell is started in becomes its home directory. When the
current directory is that home directory, or below it, the home prefix is shown
as `~`; otherwise the absolute path is printed as is. Input is read one line at
a time (up to 1024 characters, as the specification allows).

### A3 Lexing

`lexer.c` is a six-state DFA that walks the line exactly once:

```
S_START     between tokens; whitespace is skipped, operators are emitted
S_WORD      inside an unquoted fragment
S_WORD_ESC  a backslash was just consumed outside quotes
S_DQ        inside "..."        S_DQ_ESC   a backslash inside "..."
S_SQ        inside '...'        (verbatim, no escape processing)
```

Quote removal and escape processing happen while scanning, so a `WORD` token
already carries its final value. Because `WORD -> fragment+` has no separator,
fragments that touch are joined: `abc"123"'def'` is the single word
`abc123def`, and `""` is a word with an empty value.

Maximal munch is applied at each position, so `>>` is always one token and
never two `>`.

The lexer rejects a line (`cshell: invalid syntax`) when a quote is opened and
never closed, or when a backslash is the final character of the line. There is
no line continuation.

### A3 Parsing

The grammar in the specification is right linear, so the parser needs no stack:
the current non-terminal is a single state and each token either advances it or
is a syntax error.

```
P_START     LINE: a WORD, or end of line for a blank line
P_ARG       ARG:  more words, an operator, or end of line
P_NEED_TGT  TGT:  the file name after < > >>
P_NEED_CMD  CMD:  the command after | or ;
P_BG        BG:   end of line, or the command after &
```

The structure that comes out is a list of `Pipeline`s (command groups separated
by `;` or `&`), each holding a list of `Command`s (pipeline stages), each with
its own ordered list of redirections. Nothing is executed until the whole line
has been validated.

---

## Part B — Shell intrinsics

### B1 `hop`

Arguments are applied in order, so `hop a .. -` performs three moves. `~`, `.`,
`..`, `-` and ordinary relative/absolute paths are handled directly; paths
outside the home directory are allowed. When a name does not resolve on disk,
`hop` falls back to the frecency store and jumps to the best-ranked directory
whose path contains that name as a substring, skipping entries that no longer
exist. If neither succeeds it prints `hop: no such directory`.

`.` is documented as a no-op, so it does not touch the frecency store. `..` at
`/` likewise does nothing. Every other successful hop scores the destination,
even when the directory did not actually change.

**Frecency algorithm.** Each directory keeps a rank and the time of its last
visit. A visit adds 1 to the rank. The score is the rank scaled by an age
bucket, which is the shape [zoxide](https://github.com/ajeetdsouza/zoxide/wiki/Algorithm)
uses:

| Last visited | Score |
| --- | --- |
| within the hour | rank × 4 |
| within the day | rank × 2 |
| within the week | rank ÷ 2 |
| older | rank ÷ 4 |

Ties are broken lexicographically, so the result is fully deterministic. The
store is a plain text file, `.cshell_frecency`, written in the directory the
shell was started from, so it persists across sessions launched there. The
previous directory used by `hop -` is deliberately *not* persisted.

### B2 `reveal`

`reveal (-(a|t)*)* (~ | . | .. | - | name)?`

`-a` includes entries beginning with `.` but never `.` or `..` themselves (so it
behaves like `ls -A`, which also keeps `-t` from recursing forever). `-t` walks
subdirectories depth-first, printing a directory and then its contents
immediately afterwards, with directories marked by a trailing `/`. Entries are
sorted by raw ASCII value on the bare name, without the trailing slash, so
`shell.out` sorts before `src`. One entry per line for every flag combination.
Names containing blanks are quoted, the way `ls` does.

Flags may be repeated and split across several arguments, but must precede the
path. A second path argument, or an unknown flag letter, gives
`reveal: invalid syntax`; a path that is not an existing directory (including
`-` before any hop has happened) gives `reveal: no such directory`.

### B3 `peek`

`peek (-(n|r)*)* filename*`

`-n` numbers non-empty lines. The number is the count of non-empty lines seen so
far rather than the physical line number, empty lines are still printed but not
numbered, and the count runs continuously across all files. `-r` prints a file's
lines in reverse; numbering stays tied to a line's original position, so
`peek -nr a b` prints `2 1 4 3`.

Nothing is ever slurped into memory for a regular file:

* forward reading walks 4 KiB chunks;
* line counting walks 4 KiB chunks;
* `-r` walks the file **backwards** with `lseek`, one 4 KiB chunk at a time,
  holding only the fragment of the line that spans a chunk boundary.

Only non-seekable input (a pipe or the terminal) is buffered whole, which the
specification permits. `-` means standard input and may appear among file
names, so `peek file -` prints the file and then reads standard input.

A missing file prints `peek: no such file or directory` and a directory prints
`peek: is a directory`; either way the remaining arguments are still processed.
File names starting with `-` are not supported and give `peek: invalid syntax`.

### B4 `locate`

`locate filename+`

The current working directory is searched first, then every `PATH` entry in
order, and **every** match is printed, so a name present in the cwd and two
`PATH` directories produces three lines. Executability is tested for the current
user with `access(..., X_OK)`. Symlinks are not resolved: the path is printed as
it was found. Directories in `PATH` are not searched recursively. No arguments
gives `locate: invalid syntax`, and a name that matches nothing gives
`locate: command not found (name)` before moving on to the next argument.

---

## Part C — Redirection and pipes

When a line contains `;` or `&`, only the first command group is executed; the
rest is parsed and validated but ignored, as the specification requires for this
part.

### C1 Command execution

* a name containing `/` is treated as a literal path;
* a bare name is looked for in the current working directory first, then in
  `PATH`;
* a `%` prefix forces the `PATH` lookup and skips the current directory, so
  `%build.sh` fails even when `./build.sh` exists;
* anything else gives `cshell: command not found (name)`.

Resolution is done by the shell itself and the program is started with `execv`,
so nothing is delegated to a library search. Scripts with a shebang are handled
by the kernel. The intrinsics are ordinary commands as far as the grammar is
concerned, so they work with redirection and inside pipelines.

### C2/C3 Redirection

Input files are opened `O_RDONLY`; a missing one prints
`cshell: no such file or directory` and the command does not run. Output files
are opened `O_WRONLY | O_CREAT` with `O_TRUNC` for `>` and `O_APPEND` for `>>`,
mode `0644`; a file that cannot be opened prints
`cshell: unable to create file for writing` and the command does not run. Input
files are opened before output files, so a missing input never creates output
files as a side effect.

Both directions support several files:

* **several `<`** — the files are concatenated, in the order given, into one
  spool file, and the command sees a single continuous stream;
* **several `>` / `>>`** — the command writes into a spool, and once it has
  finished the spool is copied into every destination, each keeping its own
  truncate/append mode. So `echo again >> a.txt > b.txt` appends to `a.txt` and
  truncates `b.txt`, and both end up with the output.

A single redirection in either direction skips the spool and is wired straight
through with `dup2`. Spool files are created with `mkstemp` and unlinked
immediately, so they never appear on disk. Every descriptor the shell opens is
closed once it is no longer needed.

### C4 Pipes

One `pipe()` per `|`, one `fork()` per stage. Stage *i*'s standard output goes
to the write end of pipe *i* and stage *i+1*'s standard input comes from the
read end of pipe *i*. Explicit redirections are applied **after** the pipe
wiring, so they take precedence, which is what makes
`cat < in.txt | sort > out.txt` behave as expected. Each child closes every pipe
descriptor it does not use, the parent closes all of them after forking (without
this the readers would never see end of file), and the parent waits for every
stage before printing the next prompt. A stage that cannot be resolved prints
`cshell: command not found (name)` and exits; the other stages still run.

A single intrinsic with no pipe is run inside the shell itself, with its
redirections temporarily installed on the shell's own descriptors, so `hop` can
change the shell's directory. Inside a pipeline an intrinsic runs in the child,
like any other command.

---

## Notes and assumptions

* Error messages go to standard error; ordinary output goes to standard output.
* `Ctrl-C` and `Ctrl-\` are ignored by the shell itself and restored to their
  default behaviour in children, so an interrupt kills the foreground command
  rather than the shell.
* `exit` is provided as an intrinsic in addition to `Ctrl-D`.
* An empty or whitespace-only line is valid and simply reprints the prompt.
* The hostname is shown up to the first `.`, the way `bash`'s `\h` does.

---

## Testing

The shell has been exercised against a test harness covering all three parts —
lexing, quoting and escapes, grammar validation, every intrinsic and its error
paths, command resolution, redirection (including the multi-file cases) and
pipes, plus cross-checks against `cat`, `tac` and `which -a` on multi-chunk
files. It has also been run under AddressSanitizer, UndefinedBehaviorSanitizer
and LeakSanitizer with no findings, and checked for descriptor leaks over
several hundred consecutive redirection and pipeline commands.
