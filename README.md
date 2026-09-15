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
explicit `>` overrides the pipe. An intrinsic on its own runs inside the shell
itself, with its redirections installed on the shell's own descriptors and
restored afterwards, because otherwise `hop` would change the directory of a
child that is about to exit. Inside a pipeline an
intrinsic runs in the child like any other command.

### Part D: sequential and background execution

A line is a sequence of command groups separated by `;` or `&`, and they run
left to right. A foreground group is waited for before the next one starts. The
sequence only stops early when the shell cannot start a command at all, which
is reported as `cshell: command not found (name)`. A command that did start and
then exited with a non-zero status has not failed, so the sequence carries on.

To tell those two cases apart the shell has to know whether a command exists
before it forks, so names are now resolved in the parent rather than in the
child. A group whose name cannot be resolved is not started at all, not even
partially, which is also why a pipeline with one unknown stage runs none of its
stages: the whole group is one `shell_cmd` and either starts or does not.

A group followed by `&` is launched and left running, and the shell announces it
as `[job_number] pid`. Job numbers come from a counter that only goes up, so a
number is never reused in a session. They are handed out when the user is told
about a job, which means when it is backgrounded, or, once Part E is in, when a
foreground job is stopped. For a pipeline the announced pid is that of the first
stage.

That line has to appear before any output the command produces, and a child that
is forked first could easily reach `exec` before the shell got round to printing
it. So every background child stops just before `exec` and waits for the shell
to close the write end of a small pipe. The shell closes it once it has printed
the job lines, which happens at the end of the input line, or earlier if a
foreground command in the same line needs to run first. Children close every
such pipe they inherit, otherwise a job launched later would hold an earlier
job's barrier open.

Each background group is put in a process group of its own with `setpgid`,
called in both the parent and the child so neither can race the other. The
terminal stays with the shell's own group, so a background process that tries to
read from the terminal is sent `SIGTTIN` and stops instead of stealing the
user's input.

Completions are noticed by a `SIGCHLD` handler. It reaps with `WNOHANG`, so the
shell never blocks in it, and with `WUNTRACED`, so a job stopped by the terminal
is noticed as stopped rather than not at all. A job is reported once all of its
processes have gone, under the name and pid of its first stage:
`<name> with pid <pid> exited normally`, or `exited abnormally` if it was killed
by a signal. Any exit status counts as a normal exit.

The handler runs at arbitrary points in the shell's execution, so it only uses
async signal safe calls: the message is formatted by hand into a buffer and sent
with one `write`, never through `printf`. Anything that needs more than that,
such as copying a finished job's spooled output into its destination files and
freeing its slot, is left to a sweep the reader loop runs before each prompt.

The shell blocks `SIGCHLD` from just before it forks a foreground group until
that group has been waited for. That keeps the handler from reaping a child the
shell is about to wait for itself, and it is also what the specification asks
for: a background job that finishes while a foreground job is running is only
reported once the foreground job is done, because the signal is delivered when
the shell unblocks it. The mask is cleared again in every child, since it would
otherwise survive `exec`.

A job that finishes while the shell is waiting for input is reported straight
away. The handler is deliberately installed without `SA_RESTART`, so the read at
the prompt comes back with `EINTR`, the message appears on a line of its own,
and the prompt is drawn again underneath it.

### Part E1: activities

Every pipeline now runs in a process group of its own, background or not. The
group id is the pid of the first stage, and `setpgid` is called both in the
parent right after `fork` and in the child before it execs, so whichever runs
first the group exists before the command starts.

A foreground group only works that way if it is also given the terminal, or it
would be a background group as far as the terminal is concerned and would be
stopped with `SIGTTIN` the moment it read anything. So the shell hands the
terminal over with `tcsetpgrp` before waiting and takes it back afterwards.
That call comes from a process group that is not the terminal's own, which
would normally stop the caller with `SIGTTOU`, so the shell ignores that signal
and the children inherit that while they claim the terminal for themselves.
The shell also puts itself in a process group of its own at startup, and all of
this is skipped when standard input is not a terminal, since there is then no
foreground group to hand around.

`activities` prints the job table: one line per group, `[job_number] pgid
<pgid>`, with one indented line per process underneath giving its pid, the name
it was launched under and whether it is `Running` or `Stopped`. Groups come out
in the order they were launched, which is the order of their job numbers, since
those only ever go up. Slots in the table are reused, so the listing is sorted
by number rather than by position.

A process is dropped from the listing as soon as it has been reaped, and a
group disappears once all of its processes have; a pipeline that has lost only
its first stage still lists the rest, under the group id it was given at the
start. The table is shared with the `SIGCHLD` handler, so the listing is taken
and printed with the signal blocked.

Stopped is not only a Ctrl-Z thing: a background job that tries to read from
the terminal is sent `SIGTTIN`, which stops its whole group, and the handler
notices because it reaps with `WUNTRACED`. It also passes `WCONTINUED`, so a
group that is continued again is listed as running once more.

### Part E2: terminal control

Ctrl-C and Ctrl-Z reach whichever process group owns the terminal, which is the
foreground job, so the shell itself only sees them while it is the foreground
group, that is, while it waits at the prompt. It handles both rather than
ignoring them: a handler is what makes the read come back so the prompt can be
drawn again, and it is installed without `SA_RESTART` for exactly that reason.
The newline that puts the prompt under the echoed `^C` is written from the
handler with `write`, since that keeps it in step with the echo and, unlike
`printf`, is safe to call there. `SIGTTOU` is ignored, because handing the
terminal over and taking it back is done from a process group that is not the
terminal's own.

A foreground job is waited for with `WUNTRACED`, so Ctrl-Z comes back to the
shell instead of leaving it waiting for a process that is never going to
finish. The whole group is marked stopped, the terminal is reclaimed, and the
job is kept in the table so it can be listed and resumed later. It is given a
job number at that point, since that is when the user first hears about it, and
the shell prints `[n] + Stopped    <command>`. Everything the job still owns,
including output spooled for several destination files, travels with it, and is
only finished off once the job really ends.

Ctrl-D is end of input only on an empty line. On a terminal it makes the read
return whatever has been typed so far without a newline, so the shell keeps
that text and reads on, and to the user nothing happens. Input that is not a
terminal has no Ctrl-D at all, so there a last line without a closing newline is
simply run. End of input on a terminal is not permanent either, which is why
the shell clears the end of file flag every time: otherwise every later read
would report end of input again without so much as looking at the terminal.

Leaving kills whatever the shell started, so a Ctrl-D with a stopped job would
throw that job away. The first one therefore only warns with `cshell: there are
stopped jobs`; a second one with nothing typed in between goes through, and any
line that is actually run clears the warning again. On the way out the shell
sends `SIGHUP` to the process group of every job it still has on the books and
does not wait for any of them. A stopped process would not see that signal
until something got it running again, so a stopped group is sent `SIGCONT`
after it.

### Part E3: resume

`resume %n fg [--timeout s]` and `resume %n bg` put a job that is stopped, or
merely running in the background, back to work. The line is checked before the
job is looked up, so a malformed command says `resume: invalid syntax` whether
or not the job exists, and only a well formed one that names an unknown job
says `resume: no such job`.

Either way the job's process group is sent `SIGCONT` and its processes are
marked running again. `bg` then prints `[n] + Running    <command>` and comes
straight back to the prompt without touching the terminal; from that point the
job is a background job, so its end is announced like any other. `fg` prints
the command line, exactly as launching it in the foreground would, hands the
terminal to the job, and waits for it with `WUNTRACED`. If it stops again it is
reported and kept, and if it finishes it is dropped from the table. The
terminal comes back to the shell however it ended.

`--timeout` arms a `setitimer` before the wait, with a handler installed
without `SA_RESTART` so that the wait comes back when it fires. The timer is
set to repeat every second: a signal that arrives in the gap between the flag
being checked and the wait starting would otherwise be missed and the wait
would never come back. It is taken down again whichever way the wait ended, so
a job that finishes or stops first cancels it. When it does fire, the job's
group is sent `SIGTERM`, `resume: job timed out` is printed, and the job leaves
the table, since it has been killed rather than stopped. Its children are
reaped by the `SIGCHLD` handler once the signal is unblocked, which is also why
they leave no zombies behind.

### Part E4: ping

`ping <target> <signal_number>` sends a signal to one process or to a whole
process group. A target with a `%` is a job number and the signal goes to every
process in that job's group; a plain number is a pid.

The signal is checked before the target is looked up, so `ping 99999 abc` is a
syntax error rather than an unknown process. It has to be a whole non-negative
number, which makes a negative one a syntax error rather than something to be
reduced by the modulo. What is actually sent is that number modulo 64, but the
message always echoes what the user typed, so `ping 4030 79` says `Sent signal
79 to 4030` and sends signal 15.

Only what the shell started and still tracks can be a target. The pid is looked
up in the job table rather than on the system, so a pid that exists but came
from somewhere else is as unknown here as one that never existed at all, and
both say `ping: no such process found`.

Nothing extra is needed to keep the listing honest afterwards: a group that is
sent `SIGSTOP` or `SIGCONT` shows up as stopped or running again in
`activities`, because the reaper already watches for both, and one that is
killed is announced as having exited abnormally.

### Part F1: spy

`spy [pid]` lists the open files of a process, the shell's own when no pid is
given. Everything comes from `/proc`: the working directory and the executable
are symbolic links, the mapped files are the sixth field of each line of
`maps`, and the descriptors are one symbolic link each in `fd`. `readlink`
gives what each of them points at, and `stat` on the `/proc` link, which
follows through to the object itself, gives what kind of thing it is.

The executable is both `txt` and one of the mapped files, so it is skipped in
the mapped list; the rest are printed once each, and lines with no file behind
them, such as `[heap]` or an anonymous mapping, are left out. A file that has
been deleted or replaced since it was mapped is marked as such in `maps`; the
mark is not part of the path, so it is cut off before the path is used, and a
mapping whose file can no longer be found is still a regular file, since only a
file can be mapped.

Some descriptors point at something with no path at all - an eventfd, an epoll
set, a timer - and `stat` cannot reach those from outside the process. What
they are is then read off the link itself, which is also where a socket or a
pipe gives itself away. Descriptors are listed in numeric order, so 2 comes
before 10.

A pid that has no directory under `/proc` is `spy: no such process`, and more
than one pid is `spy: invalid syntax`. Checked against `lsof -p` on the same
process, the two agree entry for entry and type for type, apart from `rtd`,
which the assignment does not ask for, and the network detail it puts out of
scope.

### Part F2: snoop

`snoop command [args...]` traces a command from its very first instruction: the
child asks to be traced with `PTRACE_TRACEME` and then execs, so the exec
itself is the first stop the shell sees and nothing the command does is missed.
`snoop -p pid` attaches to something already running instead.

From there the tracee is let go one system call at a time with
`PTRACE_SYSCALL`, which stops it twice per call, once going in and once coming
back. `PTRACE_O_TRACESYSGOOD` marks those stops as `SIGTRAP|0x80`, which is
what tells them apart from a `SIGTRAP` the program raised itself; any other
signal is simply passed on. The call number is read out of `orig_rax` on the
way in, along with a `CLOCK_MONOTONIC` timestamp, and the matching timestamp on
the way back gives the time that call took. Counting happens on the way in, so
a call that never returns - `exit_group` above all - is still counted, with no
time against it.

The summary is printed busiest first, ties going to whichever call was seen
first. Names come from a table generated from the kernel's own
`asm/unistd_64.h`, and a number that is not in it prints as `syscall_N`, so a
newer kernel still traces correctly. On anything other than x86_64 the
numbering would be someone else's, so every call is reported by number.

The shell's own reaper is held off for the whole trace, or it would take the
stops the tracer is waiting for; and when the traced process is one of the
shell's own jobs, its entry is closed off by hand afterwards, since the reaper
was not the one to wait for it. A stop signal is not passed on: it would put
the tracee in a group stop that only its tracer could lift, so Ctrl-Z does
nothing to something being traced, while Ctrl-C ends it and the summary is
printed for what it did before it died.

Counts were checked against `strace -c` on the same command and agree call for
call; `strace` additionally counts the `execve` it traced through, which is the
one stop this consumes to get started.

### Source layout

| File | Does |
| --- | --- |
| `src/main.c` | entry point |
| `src/shell.c` | read, lex, parse, execute loop |
| `src/prompt.c` | the prompt |
| `src/lexer.c` | line to tokens |
| `src/parser.c` | tokens to pipelines and commands |
| `src/builtins.c` | intrinsic dispatch |
| `src/b_hop.c`, `src/b_reveal.c`, `src/b_peek.c`, `src/b_locate.c`, `src/b_activities.c`, `src/b_resume.c`, `src/b_ping.c`, `src/b_spy.c`, `src/b_snoop.c` | one intrinsic each |
| `src/frecency.c` | the frequency and recency store used by `hop` |
| `src/redirect.c` | opening redirection targets |
| `src/exec.c` | command lookup, fork, pipes, process groups, wait |
| `src/jobs.c` | the job table and the `SIGCHLD` reaper |
| `include/syscalls.h` | the x86_64 syscall names, generated from `asm/unistd_64.h` |
| `src/pathutil.c`, `src/utils.c` | shared helpers |

## xv6

A multi level feedback queue scheduler, chosen when the kernel is built:

```
cd xv6
make clean; make qemu                  # round robin, the original scheduler
make clean; make qemu SCHEDULER=MLFQ   # multi level feedback queue
make clean; make qemu SCHEDULER=FIFO   # first come first served
```

`make clean` is not optional between policies, since make cannot see that the
flags changed and would otherwise link the old objects.

`user/schedulertest.c` is the workload the scheduler is measured with, and
`plots/` holds the runs it produced, the two python scripts that draw the
figures, and the figures themselves. The write up, including the timeline of
which queue each process was in and the comparison of the three policies, is
in [xv6/report.md](xv6/report.md).

## Notes

* Errors go to standard error and ordinary output goes to standard output.
* `Ctrl-\` is ignored by the shell, and every job control signal is put back to
  its default in children, so nothing a command inherits stops it from being
  interrupted or stopped itself.
* An empty or whitespace only line is valid and just reprints the prompt.
