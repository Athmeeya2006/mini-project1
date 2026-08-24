# OSN Mini Project 1: C-Shell

**Name:** athmeeyakashyap
**Roll number:** 2024113015

A POSIX shell written from scratch in C. This submission covers Part A (shell
input), Part B (shell intrinsics) and Part C (file redirection and pipes). The
xv6 half of the project will be added for the end submission.

---

## Building and running

```sh
cd c-shell
make all       # produces ./shell.out in the same directory
./shell.out
make clean     # removes build/ and shell.out
```

The build uses the flags required by the specification:

```
-std=c23 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
-Wall -Wextra -Werror -Wno-unused-parameter -fno-asm
```

One portability note. GCC only accepts the spelling `-std=c23` from version 14
onwards; older versions want `-std=c2x` for the same standard. The Makefile
checks which one the compiler understands and uses it, so `make all` works
either way. The code builds with no warnings under `-Werror`.

Type `exit` or press `Ctrl-D` to leave the shell.

---

## Layout

```
c-shell/
    include/     one header per subsystem
    src/         one .c per subsystem
    Makefile
```

| File | Responsibility |
| --- | --- |
| `src/main.c` | entry point |
| `src/shell.c` | shell state and the read, lex, parse, execute loop |
| `src/prompt.c` | renders `<username@hostname:path>` |
| `src/lexer.c` | character-level DFA that produces tokens |
| `src/parser.c` | validates the token stream against the grammar |
| `src/builtins.c` | dispatch table for the intrinsics |
| `src/b_hop.c` | `hop` |
| `src/b_reveal.c` | `reveal` |
| `src/b_peek.c` | `peek` |
| `src/b_locate.c` | `locate` |
| `src/frecency.c` | persistent frequency and recency store used by `hop` |
| `src/redirect.c` | opens the files named by `<`, `>` and `>>` |
| `src/exec.c` | command resolution, forking, pipes, waiting |
| `src/pathutil.c` | path helpers shared by several commands |
| `src/utils.c` | allocation, growable buffers, small I/O helpers |

The split follows the stages a line goes through. Text becomes tokens, tokens
become a validated structure, and that structure gets executed. Anything used
by more than one stage lives in `utils.c` or `pathutil.c` instead of being
duplicated. The result is that `peek` never has to know what a pipe is, and `exec.c`
never has to know what a quote is.

---

## Part A: shell input

### A1, A2: prompt and input

The directory the shell starts in becomes its home directory. It is not
`$HOME` and not the login directory, just wherever the shell was launched.
Everything else that refers to "home" (the `~` argument to `hop` and `reveal`,
and the location of the frecency store) uses that same value.

When the working directory is the home directory, the prompt shows `~`. When it
sits below the home directory, the home prefix is replaced by `~`. Anywhere
else the absolute path is printed unchanged. The check requires the next
character after the prefix to be a `/`, so a home of `/home/user` does not
abbreviate `/home/user2`.

Input is read a line at a time, up to the 1024 characters the specification
allows.

### A3: lexing

The token grammar is regular, so `lexer.c` is a DFA that reads the line once,
left to right, with no backtracking. Six states cover it:

```
S_START     between tokens; whitespace is skipped, operators are emitted
S_WORD      inside an unquoted fragment
S_WORD_ESC  a backslash was just consumed outside quotes
S_DQ        inside "..."        S_DQ_ESC   a backslash inside "..."
S_SQ        inside '...'        (verbatim, no escape processing)
```

Quotes and escapes are resolved during the scan instead of in a second
pass, which is what gives `WORD -> fragment+` for nothing. The state changes
but the output buffer keeps filling, so fragments that touch are joined
automatically: `abc"123"'def'` is the single word `abc123def`. A bare `""` is a
word whose value is empty, so the lexer tracks "a word has started" separately
from "the buffer is non-empty".

The three quoting contexts differ only in how a backslash behaves. Outside
quotes it always contributes the next character literally. Inside double quotes
only `\"` and `\\` are escapes, and any other backslash keeps both characters,
which is why `echo "\n"` prints a literal backslash-n. Inside single quotes
there is no escape processing at all.

Maximal munch needs a two-character lookahead in one place, where `>>` is taken
as a single token. One consequence is that `&&` becomes two `OP_AMP`
tokens. The grammar has no such operator, so the parser rejects it, which is
the required behaviour.

Both lexical errors amount to the same thing: the line ended while a state was
still expecting more input. There is no line continuation, so ending inside a
quote or just after a backslash prints `cshell: invalid syntax`.

### A3: parsing

The grammar given in the specification is right linear, which means it is
equivalent to a finite automaton and the parser needs no stack. The current
non-terminal is just a variable:

```
P_START     LINE: a WORD, or end of line for a blank line
P_ARG       ARG:  more words, an operator, or end of line
P_NEED_TGT  TGT:  the file name after < > >>
P_NEED_CMD  CMD:  the command after | or ;
P_BG        BG:   end of line, or the command after &
```

Each state accepts exactly the tokens its production allows and everything else
is a syntax error, so the error cases can be read straight off the automaton.
`echo hi ;` ends in `P_NEED_CMD`, which has no empty production, so it fails.
`cat <` ends in `P_NEED_TGT` for the same reason. `| sort` fails immediately
because `P_START` accepts only a word or end of line. But `echo a &` is fine,
because `BG -> e` lets the line stop there.

What comes out is a list of `Pipeline`s (the groups separated by `;` or `&`),
each holding a list of `Command`s (the pipeline stages), each with its own
ordered list of redirections. Nothing runs until the whole line has been
validated, so an invalid line has no side effects at all.

---

## Part B: shell intrinsics

### B1: hop

Arguments are applied one at a time, so `hop a .. -` is three separate moves,
and a failure part way through leaves the shell wherever the last successful
move put it. Each successful move records where it came from, for `hop -`, and
scores where it arrived.

`~`, `.`, `..`, `-` and ordinary relative or absolute paths are handled
directly, and paths outside the home directory are allowed. If a name does not
resolve on disk, `hop` falls back to the frecency store and jumps to the
highest ranked directory whose path contains that name as a substring, skipping
any that no longer exist. If neither works it prints `hop: no such directory`.

Two rules are easy to get wrong. `.` is defined as doing nothing, so it must not
score anything, and `..` at `/` likewise does nothing. But `~` or an absolute
path does score, even when the directory does not actually change.

**The frecency algorithm.** Frequency on its own would keep sending you to a
directory you used heavily last month, and recency on its own would forget a
directory you use every day. The standard answer, which zoxide uses, is to keep
a visit count and scale it by how long ago the directory was last seen:

| Last visited | Score |
| --- | --- |
| within the hour | rank * 4 |
| within the day | rank * 2 |
| within the week | rank / 2 |
| older | rank / 4 |

A visit adds 1 to the rank. Ties are broken lexicographically so the result
never depends on the order entries happen to sit in the file, since the
specification asks for determinism. The store is a plain text file,
`.cshell_frecency`, written in the directory the shell was started from, so it
survives across sessions launched there. The previous directory used by
`hop -` is deliberately not persisted.

### B2: reveal

```
reveal (-(a|t)*)* (~ | . | .. | - | name)?
```

`-a` behaves like `ls -A` rather than `ls -a`: dotfiles are shown, but `.` and
`..` never are. If they were, `-t` would recurse forever.

`-t` is a pre-order depth-first walk. Every directory sorts its own entries and
is printed immediately before its contents, with directories marked by a
trailing slash. Sorting happens on the bare name, before the display slash is
added. That matters for `shell.out` against `src`, which must come out in that
order because `h` sorts before `r`. Entries are printed one per line for every
flag combination, and names containing blanks are quoted the way `ls` does.

The walk uses `lstat` and not `stat`, so a symbolic link to a directory is
listed as an ordinary entry and is not descended into. A link pointing at its
own ancestor therefore cannot hang the listing.

Flags may be repeated and split across several arguments, but they have to come
before the path. A second path argument or an unknown flag letter gives
`reveal: invalid syntax`. A path that is not an existing directory, including
`-` before any hop has happened, gives `reveal: no such directory`.

### B3: peek

```
peek (-(n|r)*)* filename*
```

`-n` numbers non-empty lines. The number is the count of non-empty lines seen
so far, not the physical line number, empty lines are still printed but
not numbered, and the count runs continuously across all the files given.

`-r` prints a file's lines in reverse. Numbering stays tied to a line's
original position, so reversing only changes the display order and two files of
two lines each print `2 1 4 3`.

This is the only command with a constraint on how it reads. Nothing is ever
loaded whole for a regular file:

* forward reading walks 4 KiB chunks;
* line counting walks 4 KiB chunks;
* `-r` walks the file backwards with `lseek`, one 4 KiB chunk at a time.

Reversing is the awkward case. A line is found by looking for the newline in
front of it, and a chunk boundary usually lands in the middle of a line. One extra buffer, `pending`, solves it, holding the part of a line that
has been seen already but whose beginning is still further left in the file.
When a newline turns up at index `k` the line starts at `k+1`; if no newline
has been seen yet in this chunk then that line runs off the right edge and is
completed by `pending`, and otherwise it ends at the previous newline found.
Whatever is left to the left of the last newline is prepended to `pending` for
the next chunk, and whatever survives the loop is the first line of the file.
Memory use is one chunk plus one line, whatever the file size.

Numbering interacts with this, because the last line's number is not known
until the file has been counted. So `-nr` makes one counting pass first and
then counts downwards during the backward pass.

Only non-seekable input, a pipe or the terminal, is buffered whole, which the
specification permits. A `-` means standard input and can appear among the file
names, so `peek file -` prints the file and then reads standard input.

A missing file prints `peek: no such file or directory` and a directory prints
`peek: is a directory`, and either way the remaining arguments are still
processed. File names starting with `-` are not supported and give
`peek: invalid syntax`.

### B4: locate

```
locate filename+
```

This does not report the one command that would run. It prints every match, so
a name present in the working directory and in two `PATH` directories produces
three lines, in that order. The working directory is searched first, then each
`PATH` entry in order, and `PATH` directories are not searched recursively.

Executability is tested with `access(path, X_OK)` instead of reading the
permission bits by hand, since that answers the question for the current user,
including group membership. Directories are excluded, since a directory
is "executable" only in the sense of being searchable. Symbolic links are not
resolved, so the path is printed as it was found.

No arguments gives `locate: invalid syntax`. A name that matches nothing gives
`locate: command not found (name)` and processing moves on to the next
argument.

---

## Part C: redirection and pipes

When a line contains `;` or `&`, only the first command group runs. The rest is
parsed and validated but ignored, as the specification requires for this part.

### C1: command execution

* a name containing `/` is treated as a literal path;
* a bare name is looked for in the working directory first, then in `PATH`;
* a `%` prefix forces the `PATH` lookup and skips the working directory, so
  `%build.sh` fails even when `./build.sh` exists;
* anything else gives `cshell: command not found (name)`.

The shell does this lookup itself and then calls `execv`, rather than handing
the name to `execvp`, because the rules above differ from the C library's.
Scripts with a shebang need no special handling, since the kernel recognises
the magic number and starts the interpreter. The `%` marker is stripped from
`argv[0]` and from the error message, so `%build.sh` reports
`cshell: command not found (build.sh)`.

The intrinsics are ordinary commands as far as the grammar is concerned, so
they work with redirection and inside pipelines too.

### C2, C3: redirection

Input files are opened `O_RDONLY`. A missing one prints
`cshell: no such file or directory` and the command does not run. Output files
are opened `O_WRONLY | O_CREAT` with `O_TRUNC` for `>` or `O_APPEND` for `>>`,
mode `0644`. One that cannot be opened prints
`cshell: unable to create file for writing` and the command does not run.
Either failure aborts the command instead of running it with a partial set
of descriptors.

Input files are opened before output files, so a missing input never creates or
truncates an output file as a side effect.

A single redirection in either direction is a plain `dup2`. The multi-file
cases need more thought, because a process has exactly one standard input and
one standard output:

* **several `<`**: the files are concatenated, in the order given, into one
  spool file, and the command is handed that spool. This is exactly the "one
  continuous stream" the specification asks for.
* **several `>` or `>>`**: the command writes into a spool, and once it has
  finished the spool is copied into each destination. Each destination keeps
  its own mode, so `echo again >> a.txt > b.txt` appends to one file and
  truncates the other while both receive the output.

Spools are made with `mkstemp` and unlinked immediately, so they never show up
in a directory listing; the descriptor keeps the data alive until it is closed.
Every descriptor the shell opens is closed once it is no longer needed.

### C4: pipes

One `pipe()` per `|` and one `fork()` per stage. All the pipes are created
before any fork, so every child inherits every descriptor and can then close
what it does not need. Stage *i* writes to the write end of pipe *i*, and stage
*i+1* reads from the read end of pipe *i*.

Two ordering decisions matter here.

Redirections are applied after the pipe wiring, so an explicit `>` overrides
the pipe, which is what `cat < in.txt | sort > out.txt` needs.

The parent closes every pipe descriptor after forking. Forgetting this hangs
the shell, because a reader only sees end of file once all the write ends are
closed and the parent is still holding one. With the parent's copies closed, each stage finishes
as its predecessor does, and the shell waits for every child before printing
the next prompt.

A stage whose command cannot be resolved prints its error and exits with status
127, and the remaining stages still run. So `badcmd | sort` prints the
diagnostic and then an empty sort result.

A single intrinsic with no pipe is a special case: it runs inside the shell
itself, with its redirections temporarily installed on the shell's own
descriptors and restored afterwards. Otherwise `hop` would change the directory
of a child that is about to exit. Inside a pipeline an intrinsic runs in the
child like any other command.

---

## Notes and assumptions

* Error messages go to standard error and ordinary output goes to standard
  output.
* `Ctrl-C` and `Ctrl-\` are ignored by the shell and restored to their default
  behaviour in children, so an interrupt kills the foreground command instead
  of the shell.
* `exit` is provided as an intrinsic alongside `Ctrl-D`.
* An empty or whitespace-only line is valid and reprints the prompt.
* The hostname is shown up to the first `.`, the way `bash`'s `\h` does.

---

## Testing

The shell was checked with a script that feeds a set of commands to
`shell.out`, merges standard error into standard output, strips the prompts and
compares the result against an expected string. It covers lexing, quoting and
escapes, all the grammar error cases, every intrinsic and its error paths,
command resolution, redirection including the multi-file cases, and pipelines.
A few cases compare against `cat`, `tac` and `which -a` on a 4000-line file
instead, which is what exercises the multi-chunk path in `peek -r`.

It was also rebuilt under AddressSanitizer, UndefinedBehaviorSanitizer and
LeakSanitizer and put through the same workload with nothing reported, and
checked for descriptor leaks by running several hundred consecutive redirection
and pipeline commands in one session.
