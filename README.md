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

```sh
cd c-shell
make all       # builds ./shell.out
./shell.out
make clean
```

Type `exit` or press `Ctrl-D` to quit.

The directory the shell is started in is treated as its home, and is what `~`
refers to in the prompt, in `hop` and in `reveal`.

**Implemented**

- Part A — prompt, input, lexing and parsing of the given grammar.
- Part B — the intrinsics `hop`, `reveal`, `peek` and `locate`.
- Part C — `<`, `>`, `>>`, pipes, and command execution (including the `%`
  prefix that forces a `PATH` lookup).

**Source layout**

| File | Does |
| --- | --- |
| `src/main.c` | entry point |
| `src/shell.c` | read, lex, parse, execute loop |
| `src/prompt.c` | the prompt |
| `src/lexer.c` | line to tokens |
| `src/parser.c` | tokens to pipelines and commands |
| `src/builtins.c` | intrinsic dispatch |
| `src/b_hop.c`, `src/b_reveal.c`, `src/b_peek.c`, `src/b_locate.c` | one intrinsic each |
| `src/frecency.c` | the frequency + recency store used by `hop` |
| `src/redirect.c` | opening redirection targets |
| `src/exec.c` | command lookup, fork, pipes, wait |
| `src/pathutil.c`, `src/utils.c` | shared helpers |

**Build flags**

```
-std=c23 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
-Wall -Wextra -Werror -Wno-unused-parameter -fno-asm
```

GCC only accepts `-std=c23` from version 14 onwards; older versions want
`-std=c2x`. The Makefile picks whichever the compiler understands. The code
builds clean under `-Werror`.

## xv6

See [xv6/report.md](xv6/report.md).

## Notes

- Errors go to stderr, output to stdout.
- `Ctrl-C` and `Ctrl-\` are ignored by the shell and restored in children.
- An empty line just reprints the prompt.
- `hop`'s store is a text file, `.cshell_frecency`, in the shell's home directory.
