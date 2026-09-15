

# 1. C, POSIX, and Build Environment

## Q1. Why use `-std=c23`?

`-std=c23` selects the C23 language mode instead of a GNU dialect such as `gnu17`. The practical goal in this assignment is to keep language usage within the specified C standard and avoid depending on compiler-only language extensions.

The shell is still fundamentally a POSIX program. POSIX functions are provided by the operating system's C library and are not themselves part of ISO C.

A useful mental separation is:

```text
ISO C
  -> syntax, types, memory allocation, strings, stdio, qsort, etc.

POSIX
  -> fork, execve, pipe, dup2, open, waitpid, signals, process groups, etc.
```

The assignment's shell should stay within the permitted POSIX API.

---

## Q2. What does `_POSIX_C_SOURCE=200809L` do?

It is a feature-test macro. Before system headers are processed, the macro tells the C library which POSIX interface level the program expects.

For this project, this is relevant to interfaces such as:

```text
fork()
execve()
pipe()
dup2()
open()
waitpid()
sigaction()
getcwd()
chdir()
read()
write()
lseek()
```

The exact symbols exposed can vary slightly by implementation, but the purpose is to request the POSIX.1-2008 interface set.

---

## Q3. What does `_XOPEN_SOURCE=700` do?

It requests the X/Open/SUSv4 interface level associated with POSIX.1-2008 plus XSI extensions.

The important implementation principle is still the same: write against the standard interfaces permitted by the assignment rather than depending on Linux/GNU convenience APIs.

Because `_GNU_SOURCE` is not being requested, code should not quietly rely on GNU-only functions such as `execvpe()` or `asprintf()`.

---

## Q4. What belongs in a header and what belongs in a source file?

Headers should normally contain:

```text
typedefs
struct definitions
enum definitions
constants/macros
function prototypes
extern declarations
```

Source files contain:

```text
function implementations
global variable definitions
private helper functions
```

For shared state:

```c
/* jobs.h */
extern int next_job_number;
```

and exactly one definition:

```c
/* jobs.c */
int next_job_number = 1;
```

This avoids multiple-definition linker errors.

---

## Q5. Why use `static` for private helpers?

A `static` function at file scope has internal linkage.

```c
static int is_blank(char c) {
    return c == ' ' || c == '\t';
}
```

It cannot collide with a helper having the same name in another source file. This is useful for parser, lexer, job, and builtin-specific utility functions.

---

## Q6. How does `make` decide what to rebuild?

`make` treats the build as a dependency graph.

For example:

```make
parser.o: parser.c parser.h shell.h
	$(CC) $(CFLAGS) -c parser.c -o parser.o
```

If `parser.h` changes, the object becomes stale and is rebuilt.

The important concept is that source files depend not only on `.c` files but also on the headers that affect their compilation.

The assignment expects `make all` in the shell directory to produce `shell.out`.

---

## Q7. What common memory leaks occur in a long-running shell?

The shell loops indefinitely, so per-command leaks accumulate.

Common examples:

```text
malloc'ed token strings never freed
realloc'ed token arrays never freed
partial parser structures abandoned on syntax errors
strdup'ed arguments never freed
file descriptors never closed
frecency entries repeatedly allocated without cleanup
temporary buffers lost on early returns
```

The most useful rule is ownership:

> Every allocation has one owner, and every owner has a cleanup path.

There should be a cleanup path for success, syntax error, execution error, and allocation failure.

---

# 2. Shell Architecture

## Q8. What are the major layers of the shell?

A clean architecture is:

```text
stdin
  |
  v
input reader
  |
  v
lexer
  |
  v
parser
  |
  v
execution representation
  |
  +------------+----------------+
  |                             |
builtin                      external command
  |                             |
  |                     fork/process group
  |                             |
  |                     redirection/pipes
  |                             |
  |                           execve
  |
  +-----------------------------+
                |
                v
        jobs/signals/reaping
                |
                v
              prompt
```

The key design principle is to avoid making the executor reinterpret syntax that the parser already understood.

---

## Q9. Why separate lexing from parsing?

The lexer answers:

```text
What token is this sequence of characters?
```

The parser answers:

```text
Is this sequence of tokens grammatically valid?
```

For example:

```text
echo "a > b" >> file
```

The lexer must know that `>` inside quotes is ordinary text and that `>>` is one token. The parser then checks whether that token sequence fits the shell grammar.

---

# 3. Shell Input Reader

## Q10. Why can raw `read()` be preferable here?

The project has unusual requirements around Ctrl-D, prompt redraw, and piped input.

`read()` gives direct control over bytes arriving from stdin. The reader can maintain an internal buffer and consume exactly one logical line per shell iteration.

Unlike `fgets()`, `read()` can return:

```text
1..N bytes
0 bytes for EOF
-1 for error
```

It does not automatically append `\0`, and one call does not necessarily correspond to one line.

---

## Q11. Why must the reader consume exactly one line from piped stdin?

Suppose stdin contains:

```text
peek -
 echo next
```

If `peek -` calls a buffered read that consumes both lines, the shell may lose `echo next` before the main shell loop gets to process it.

Therefore the input subsystem should have an unread buffer conceptually like:

```text
persistent byte buffer
          |
          +--> extract first complete line
          |
          +--> retain remainder for next iteration
```

This is essential for non-interactive shell input.

---

## Q12. What happens if Ctrl-D is pressed after typing text?

Ctrl-D represents EOF only when the current input line is empty.

For this assignment:

```text
empty line + Ctrl-D -> shell may exit
non-empty partial line + Ctrl-D -> preserve typed text
```

Therefore the reader needs to distinguish line state rather than treating every EOF event as an immediate exit.

---

## Q13. Why can prompt redraw be difficult?

A background completion can happen while the user has already typed part of the next command. If the shell prints a status message directly into the middle of the current line, the terminal display becomes confusing.

A robust approach separates:

```text
stored input line
shell status messages
prompt rendering
```

When a redraw is needed, the stored input is not lost. The prompt can be printed again and the preserved text can be restored.

---

# 4. Lexer

## Q14. What characters are special to the lexer?

The special operators are:

```text
|
&
>
<
;
```

The two-character token is:

```text
>>
```

Quotes are:

```text
'
"
```

Backslash is the escape character outside single quotes and has special behavior inside double quotes according to the assignment.

Whitespace includes spaces, tabs, newlines, and carriage returns for token separation.

---

## Q15. What is maximal munch?

Maximal munch means taking the longest valid token at the current position.

For example:

```text
>>
```

must be emitted as:

```text
OP_GTGT
```

rather than:

```text
OP_GT
OP_GT
```

A one-character lookahead is enough because only `>` has the multi-character variant `>>` here.

---

## Q16. How should unquoted backslash work?

Outside quotes:

```text
\c
```

means the backslash loses its special meaning and contributes only `c` to the word.

Therefore:

```text
my\ file.txt
```

becomes one word containing the space.

Similarly:

```text
\|
```

makes `|` part of a word rather than a pipeline operator.

A backslash at end of line is invalid because there is no character to escape.

---

## Q17. How should single quotes work?

Within:

```text
'...'
```

all characters are literal until the next `'`.

That includes:

```text
spaces
backslashes
pipes
redirect symbols
double quotes
```

No escape processing occurs inside single quotes.

---

## Q18. How should double quotes work?

Within:

```text
"..."
```

spaces and shell operators are ordinary word characters.

For this assignment, `\"` becomes `"` and `\\` becomes `\`. Other backslash-character combinations retain both characters.

This is different from the more general behavior of a full Bash shell; follow the assignment's exact lexical rules rather than inventing additional shell features.

---

## Q19. What should happen on an unclosed quote?

The assignment has no multiline continuation. If the lexer reaches the end of the input line while still inside a quote state, it reports:

```text
cshell: invalid syntax
```

The parser must not attempt to execute a partially tokenized line.

---

## Q20. How can lexer state be modeled?

A simple finite-state model is:

```text
NORMAL
SINGLE_QUOTE
DOUBLE_QUOTE
```

In `NORMAL`:

```text
whitespace -> token boundary
operator   -> operator token
quote      -> enter quote state
backslash  -> escape next char
ordinary   -> append to current word
```

In `SINGLE_QUOTE`:

```text
' -> return to NORMAL
anything else -> append literally
```

In `DOUBLE_QUOTE`:

```text
" -> return to NORMAL
backslash + " -> append "
backslash + \ -> append \
anything else -> append literally according to assignment rule
```

---

# 5. Parser

## Q21. What does the supplied grammar imply?

The grammar is right-linear and is essentially a finite-state grammar.

The important categories are:

```text
LINE
ARG
CMD
TGT
BG
```

The parser can therefore be implemented with a small number of states instead of constructing a complex recursive AST.

---

## Q22. What parser states are useful?

A practical state machine can use:

```text
EXPECT_COMMAND
EXPECT_ARGUMENT_OR_OPERATOR
EXPECT_TARGET
EXPECT_BACKGROUND_BODY
```

For example:

```text
START
  |
 WORD
  v
COMMAND_BODY
  |
  +--> WORD -> COMMAND_BODY
  +--> <    -> EXPECT_TARGET
  +--> >    -> EXPECT_TARGET
  +--> >>   -> EXPECT_TARGET
  +--> |    -> EXPECT_COMMAND
  +--> ;    -> EXPECT_COMMAND
  +--> &    -> optional background body / line end
  +--> EOF  -> accept
```

---

## Q23. Why validate the entire line before executing?

The assignment explicitly requires the input line to be scanned and validated before execution.

Suppose:

```text
echo hi ;
```

is invalid. The shell should not execute `echo hi` and only then discover that the final semicolon was invalid.

Validation first gives the all-or-nothing syntax behavior expected by the assignment.

---

## Q24. What is the difference between syntax and execution failure?

Syntax failure:

```text
cat |
```

means the input is not a legal command line at all.

Execution failure:

```text
missing-command
```

means the command line was syntactically valid but an executable could not be started.

The executor must not be responsible for parser-level syntax validation.

---

# 6. Intermediate Representation

## Q25. Why build an execution representation?

Once the token stream is validated, execution becomes simpler if the shell has explicit objects representing:

```text
command arguments
input redirections
output redirections
append/truncate mode
pipeline links
background status
```

For example:

```c
typedef struct Command {
    char **argv;
    int argc;

    char **input_files;
    int input_count;

    char **output_files;
    int *append;
    int output_count;
} Command;
```

A pipeline can then be represented as an array or linked sequence of `Command` objects.

---

# 7. Prompt

## Q26. How should the prompt be constructed?

The prompt needs:

```text
username
hostname
current path
```

The startup directory is the shell's home path for the prompt.

If current directory is inside that path, substitute the prefix with `~`.

Otherwise use the absolute path.

---

## Q27. Why is prefix matching insufficient by itself?

Suppose home is:

```text
/usr/app
```

and CWD is:

```text
/usr/app_data
```

A raw prefix match incorrectly says the CWD begins with home.

After the matched prefix, the next character must be either:

```text
/ or \0
```

This is a directory-boundary check.

---

## Q28. How should the root case work?

When home is `/`, a child path such as `/usr` should display as:

```text
~/usr
```

not:

```text
~usr
```

The root case needs special attention because the home prefix already ends with `/`.

---

# 8. Builtin `hop`

## Q29. Why must `hop` execute inside the shell?

`chdir()` changes the calling process's working directory.

If the shell forks a child and the child executes `chdir`, only the child changes directory. The parent shell remains where it was.

Therefore `hop` must run directly in the shell process when it changes the shell's CWD.

---

## Q30. What are the direct `hop` cases?

```text
hop       -> home
hop ~     -> home
hop .     -> no-op
hop ..    -> parent
hop -     -> previous directory
hop path  -> direct path if it resolves
```

If a name does not resolve directly, the implementation falls back to frecency.

---

## Q31. How should previous-directory state be updated?

Correct sequence:

```text
save current directory
resolve target
attempt chdir(target)
if success:
    previous = saved current directory
if failure:
    previous remains unchanged
```

Do not modify `previous_dir` before knowing that `chdir()` succeeded.

---

# 9. Frecency

## Q32. What does frecency mean mathematically?

A deterministic exponential-decay model can be:

```text
S_new = S_old * exp(-lambda * delta_t) + 1
```

where:

```text
S_old       = previous score
Delta_t     = elapsed time
lambda      = decay constant
1           = current visit contribution
```

The implementation is not required to use exactly this formula. The assignment allows any deterministic scoring, decay, and storage design.

---

## Q33. Why can lazy decay be used?

The shell does not need to update a directory's score every second.

It can store:

```text
path
score
last_visit
```

and apply the elapsed-time decay when the entry is examined.

This keeps the implementation simple and avoids a background maintenance thread.

---

## Q34. How should frecency matching work?

When direct resolution fails:

```text
for each stored entry:
    if path contains query substring:
        compute effective score
choose highest-scoring entry that still exists
```

If the highest-scoring path no longer exists, skip it and continue searching.

---

## Q35. How should frecency history be persisted safely?

A useful lifecycle is:

```text
startup -> load history into memory
hop success -> update memory
hop success -> write updated history safely
shutdown -> optionally flush again
```

For safe writes:

```text
write complete new data to temporary file
flush/close
rename temporary file over target
```

Do not truncate the real database before the replacement data has been successfully generated.

---

# 10. `reveal`

## Q36. Which APIs implement directory traversal?

Typical APIs are:

```c
opendir()
readdir()
closedir()
```

`readdir()` returns directory entries one at a time.

The returned pointer is managed by the directory stream, so names that need to survive later calls should be copied into owned storage.

---

## Q37. How are hidden files identified?

The convention is simply:

```c
entry->d_name[0] == '.'
```

The default mode skips them.

`-a` includes them.

---

## Q38. Why should recursive traversal use `lstat()` carefully?

`stat()` follows symbolic links. A symlink can point back to an ancestor directory.

A recursive walker that follows the link can then recurse forever.

`lstat()` lets the walker identify a symlink itself instead of automatically following it.

The recursive decision should be based on the type of the entry under the traversal rules required by the assignment.

---

## Q39. Why build a full path before metadata lookup?

Suppose the shell is traversing:

```text
/home/project/subdir
```

and `readdir()` returns:

```text
file.txt
```

Calling:

```c
stat("file.txt", ...)
```

uses the process's CWD, not the directory being traversed.

Instead construct:

```text
/home/project/subdir/file.txt
```

and call the metadata function on that path.

---

## Q40. How should ASCII lexicographic sorting work?

For ordinary ASCII filenames, `strcmp` compares byte-by-byte.

The relative order is generally:

```text
0-9
A-Z
a-z
```

For example:

```text
"Zebra" < "apple"
"10" < "2"
```

because the comparison is lexical, not numerical.

---

## Q41. How does `qsort()` work with `char **`?

The comparator receives pointers to array elements.

If the array is:

```c
char **names;
```

then each comparator argument points to a `char *` element, so one dereference gives the actual string:

```c
const char *sa = *(const char * const *)a;
const char *sb = *(const char * const *)b;
return strcmp(sa, sb);
```

If sorting structs instead, the arguments point directly to the struct objects.

---

## Q42. How should recursive output be ordered?

For each directory:

```text
collect entries
filter hidden entries if needed
sort entries by bare filename
print directory entry
then recurse into the directory if required
```

When `-t` is active, directories are displayed with `/` but the slash is presentation only. Sorting uses the bare entry name.

---

# 11. `peek`

## Q43. What does `peek -n` do?

It numbers non-empty logical lines.

The line number belongs to the line as part of the concatenated input stream rather than automatically restarting at every filename.

---

## Q44. What is the critical multiple-file newline edge case?

Suppose:

```text
file1 = "hello"       // no final newline
file2 = " world\n"
```

The combined logical stream is:

```text
hello world\n
```

Therefore the beginning of file2 continues the existing line. The implementation must not automatically assign file2's first line a new number.

---

## Q45. How should `peek -r` handle regular files?

The assignment requires seekable regular files to be processed backwards in fixed-size chunks.

The high-level algorithm is:

```text
fd = open file
file_size = lseek(fd, 0, SEEK_END)
pos = file_size

while pos > 0:
    start = max(0, pos - CHUNK_SIZE)
    lseek(fd, start, SEEK_SET)
    read chunk
    scan chunk from right to left
    emit complete reverse-order lines
    preserve incomplete prefix fragment
    pos = start
```

The difficult case is a line spanning two chunks.

---

## Q46. How do you stitch a line across chunk boundaries?

Suppose the end of the earlier chunk contains:

```text
abc
```

and the beginning of the later chunk contains a partial continuation:

```text
def
```

The implementation must not emit `abc` as a complete line before seeing the continuation. It keeps the incomplete fragment until the adjacent chunk has been processed.

The exact bookkeeping can use a dynamically allocated string or a bounded fragment buffer depending on the chosen implementation.

---

## Q47. Why is `lseek()` unavailable for stdin and pipes?

A terminal or pipe is not a seekable regular file. There is no random-access file offset that can be moved backward over already-consumed bytes.

For non-seekable input, the assignment explicitly permits buffering the full input before reversing it.

Thus the implementation can have two paths:

```text
regular file -> reverse with lseek/chunks
pipe/stdin    -> read all, split into lines, reverse in memory
```

---

# 12. `locate`

## Q48. What is the required search order?

For each command name:

```text
1. current working directory
2. each PATH directory in order
```

All executable matches are printed.

No recursive search inside PATH directories occurs.

Every printed path is absolute.

If there is no executable match:

```text
locate: command not found (name)
```

Processing continues with later names.

---

# 13. External Command Execution

## Q49. How should a normal external command be launched?

The basic process model is:

```text
shell
  |
 fork()
  |
  +--> child
  |      |
  |      +--> set up process group
  |      +--> set up stdin/stdout
  |      +--> execve()
  |
  +--> parent waits or records background job
```

The shell itself must not call `execve()` in place of the child, because `execve()` replaces the calling process image.

---

## Q50. What is the command lookup order?

The assignment requires:

```text
name contains /  -> literal executable path
name has no /    -> check current directory first, then PATH
%name            -> skip current directory, search PATH only
```

A matching file must also satisfy the executable requirement.

---

# 14. Input Redirection

## Q51. How does `<` work?

For:

```text
cat < input.txt
```

open:

```c
open("input.txt", O_RDONLY)
```

then:

```c
dup2(fd, STDIN_FILENO);
close(fd);
```

The child performs this before `execve()`.

If opening any required input file fails, the command must not execute.

---

## Q52. How should multiple input redirections work?

The assignment requires:

```text
command < f1 < f2
```

to supply the concatenation:

```text
contents(f1) + contents(f2)
```

A single repeated `dup2()` onto stdin would be insufficient because the earlier file would be replaced rather than concatenated.

A practical solution is to build a stream containing the contents of all listed input files, usually through an internal pipe or equivalent buffering mechanism, and connect that stream to stdin.

---

# 15. Output Redirection

## Q53. How do `>` and `>>` differ?

`>`:

```c
open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)
```

`>>`:

```c
open(path, O_WRONLY | O_CREAT | O_APPEND, 0644)
```

The descriptor is then duplicated to stdout.

The file mode for new files is `0644` as required by the assignment.

---

## Q54. Why is multiple output redirection harder?

The assignment requires:

```text
command > a > b >> c
```

to send the complete command output to all three targets.

Repeatedly doing:

```c
dup2(fd_a, STDOUT_FILENO);
dup2(fd_b, STDOUT_FILENO);
dup2(fd_c, STDOUT_FILENO);
```

would send output only to the last descriptor.

The shell therefore needs a fan-out mechanism.

One design is:

```text
command stdout
      |
     pipe
      |
   tee worker
   /   |   \
  a    b    c
```

The tee worker reads each chunk and writes that same chunk to every output target.

---

## Q55. What is the most common tee bug?

The reader can hang forever if some process still holds a duplicate of the pipe's write descriptor.

After `fork()`, the pipe descriptors exist in parent and children.

All unused copies must be closed promptly so the reader eventually receives EOF.

---

# 16. Pipelines

## Q56. How many pipes are needed for N commands?

A pipeline of `N` stages needs:

```text
N - 1 pipes
```

For:

```text
a | b | c
```

there are two pipes.

---

## Q57. What are the stdin/stdout connections in a pipeline?

For stage `i`:

```text
if i > 0:
    stdin <- pipe[i-1].read

if i < N-1:
    stdout -> pipe[i].write
```

Each child then closes all descriptors it does not need.

---

## Q58. Why must the parent close all pipe descriptors?

A parent copy of a write end can prevent a downstream process from observing EOF.

The kernel tracks pipe endpoint references. EOF is delivered only when no write-side reference remains.

Therefore the parent must close its copies once they are no longer needed.

---

## Q59. What happens if a pipeline stage cannot be executed?

The assignment explicitly requires the other stages to continue.

For:

```text
badcmd | sort
```

the `badcmd` child reports:

```text
cshell: command not found (badcmd)
```

but `sort` still exists and runs.

This is deliberately different from `;` sequencing.

---

# 17. Sequential Execution

## Q60. How does `;` behave?

For:

```text
a ; b ; c
```

the shell waits for `a`, then starts `b`, waits for `b`, then starts `c`.

Failure means the shell could not start the command.

A command that started and later returned a nonzero exit status is still considered successfully executed for sequencing purposes.

---

## Q61. Why is this distinction important?

Compare:

```text
false ; echo after
```

with:

```text
missing ; echo after
```

The first command started, so the sequence continues.

The second could not be started, so the sequence stops.

This means the executor should communicate whether a process was successfully launched, not simply whether its eventual exit code was zero.

---

# 18. Background Execution

## Q62. What changes when `&` is used?

For:

```text
sleep 10 &
```

the shell creates the child and immediately returns to its input loop.

It must not synchronously wait for the child.

The shell assigns a session-wide monotonically increasing job number.

---

## Q63. Why print the job number before command output?

The required format is:

```text
[job_number] process_id
```

and the assignment requires this line before any command-generated output.

This prevents output from racing ahead of the shell's identification of the new job.

---

## Q64. How are background jobs reaped?

The shell needs `SIGCHLD` handling and nonblocking `waitpid`.

Conceptually:

```text
child exits
   |
   v
SIGCHLD
   |
   v
waitpid(..., WNOHANG)
   |
   v
update job table
```

A `SIGCHLD` handler should avoid doing complex non-async-signal-safe work. A common architecture is for the handler to record that child state needs processing, and for the main execution path to perform the detailed bookkeeping safely.

---

## Q65. What does `WNOHANG` accomplish?

Without `WNOHANG`, the shell could block waiting for a child that is still running.

With `WNOHANG`, `waitpid()` returns immediately if no matching child has a state change ready to reap.

This is what makes background completion detection nonblocking.

---

## Q66. How should background stdin be handled?

Background jobs cannot be allowed to consume the shell's interactive input.

When the intended input source is the terminal or a stream from which the shell itself is reading commands, background stdin should be detached, commonly by opening `/dev/null` and duplicating it to stdin.

This prevents commands such as:

```text
cat &
echo next
```

from allowing the background `cat` to swallow `echo next`.

---

# 19. Job Control

## Q67. What is a process group?

A PID identifies one process.

A process group contains multiple related processes and has a PGID.

A shell job representing a pipeline should have one process group:

```text
job
  PGID = 5000
  |
  +-- PID 5000
  +-- PID 5001
  +-- PID 5002
```

Signals can then be sent to the entire job as a group.

---

## Q68. Why do pipelines need process groups?

Ctrl-C and Ctrl-Z operate through the terminal's foreground process group.

If the stages of a pipeline belonged to unrelated groups, the terminal signal could affect some stages but not others.

The shell therefore creates one group per job/pipeline.

---

## Q69. Why call `setpgid()` in both parent and child?

There is a race between `fork()` and `execve()`.

Calling `setpgid()` in the child makes the child join the intended group before execution proceeds.

Calling it in the parent immediately after `fork()` reduces the window in which the parent sees the wrong group membership.

The assignment explicitly requests both sides.

---

## Q70. What does `tcsetpgrp()` do?

It gives the controlling terminal to a process group.

Foreground execution therefore looks like:

```text
shell owns terminal
       |
       v
launch job
       |
       v
tcsetpgrp -> job PGID
       |
       v
wait job
       |
       v
tcsetpgrp -> shell PGID
```

Background jobs do not receive terminal ownership.

---

# 20. Ctrl-C, Ctrl-Z, and Ctrl-D

## Q71. How should Ctrl-C behave?

The shell must survive `SIGINT`.

If a foreground pipeline owns the terminal, Ctrl-C is delivered to that foreground process group rather than the shell's group.

After the pipeline terminates, the shell reclaims the terminal.

---

## Q72. How should Ctrl-Z behave?

The foreground process group receives `SIGTSTP`.

The shell waits with `WUNTRACED` so it can observe the stopped state.

Then:

```text
mark group stopped
update individual process states
reclaim terminal
print stopped message
return to prompt
```

A process that already exited must not be relabeled as stopped.

---

## Q73. What is the stopped-versus-exited issue in a pipeline?

Suppose a pipeline contains `A | B` and `A` exits just before Ctrl-Z stops `B`.

If the job table only knows that the group stopped, it might print both:

```text
A Stopped
B Stopped
```

which is wrong.

The shell must maintain per-process state and remove or mark exited stages before reporting `activities`.

---

## Q74. What happens on Ctrl-D with stopped jobs?

If there is any stopped job, the first Ctrl-D prints:

```text
cshell: there are stopped jobs
```

and the shell stays alive.

An immediate second Ctrl-D exits.

When exiting, the shell sends `SIGHUP` to the process group of every tracked background or stopped job and does not wait for those jobs afterward.

---

# 21. `activities`

## Q75. What data should be stored for each job?

A practical job structure contains:

```text
job number
process group ID
first PID
command display string
process array/list
stage count
job state
launch order
```

Each process record can contain:

```text
PID
command name
state
```

---

## Q76. What should `activities` print?

For each live group:

```text
[job_number] pgid <pgid>
  pid command Running
  pid command Stopped
```

Jobs are ordered by launch time, oldest first.

Exited processes must be removed before printing.

---

# 22. `resume`

## Q77. What does `resume %N bg` do?

The shell:

```text
resolve job N
send SIGCONT to PGID
mark job Running
leave terminal with shell
print running message
return to prompt
```

It does not wait for the job.

---

## Q78. What does `resume %N fg` do?

The shell:

```text
resolve job
SIGCONT job group
give terminal to group
print command
wait for stop/exit
reclaim terminal
update job table
```

This is effectively a controlled transition from background/stopped job to foreground execution.

---

## Q79. How does `--timeout` work?

For:

```text
resume %1 fg --timeout 3
```

start a timer before waiting.

If the job is still running when the timer expires:

```text
send SIGTERM
print resume: job timed out
reclaim terminal
remove terminated job
```

If the job ends or stops first, cancel the timer.

---

## Q80. Why is timeout handling a race?

Timer expiry and child exit can happen nearly simultaneously.

It is incorrect to assume:

```text
SIGALRM arrived -> job must still be running
```

The shell must check actual tracked process state before declaring a timeout.

A race-safe design treats timer expiry as a request to check the deadline condition, not as unquestionable proof that the job was alive at that exact instant.

---

# 23. `ping`

## Q81. What does `ping` target?

A plain numeric target is a PID.

A target beginning with `%` is a job number.

The shell must reject unknown PIDs even if those PIDs happen to exist in the system, because the assignment allows only shell-spawned tracked processes.

---

## Q82. How is the signal number interpreted?

The original typed non-negative integer is validated first.

The actual signal value sent is:

```text
signal_number % 64
```

The success message echoes the original typed value.

---

## Q83. How can integer parsing avoid overflow?

Digit-by-digit accumulation is safe:

```text
value = 0
for digit d:
    if value > (MAX - d) / 10:
        overflow
    value = value * 10 + d
```

Reject any non-digit character and reject negative values.

The target lookup should happen only after numeric validation succeeds.

---

# 24. `spy`

## Q84. How can `/proc` be used to inspect another process?

On Linux, process information is exposed beneath:

```text
/proc/PID/
```

Useful interfaces include:

```text
/proc/PID/cwd
/proc/PID/exe
/proc/PID/fd/
/proc/PID/maps
```

`readlink()` is useful for retrieving the target of `/proc` symbolic links.

---

## Q85. What should `spy` report?

The required fields are:

```text
PID
FD
TYPE
PATH
```

Special FD labels include:

```text
cwd
mem
txt
```

along with numeric descriptors such as `0`, `1`, and `2` where applicable.

Each unique memory-mapped file should be printed only once.

---

# 25. `snoop`

## Q86. How does `snoop command args...` work?

The tracer forks.

Child:

```text
ptrace(PTRACE_TRACEME, ...)
execve(command, ...)
```

Parent waits for ptrace stops and controls the child using:

```text
PTRACE_SYSCALL
```

This makes the tracee stop at syscall entry and exit points.

---

## Q87. How are syscall times measured?

At syscall entry, store:

```text
syscall number
entry timestamp
```

At syscall exit, compute:

```text
exit timestamp - entry timestamp
```

Then aggregate:

```text
syscall name
number of calls
total time
```

---

## Q88. How should syscall summaries be sorted?

Primary ordering:

```text
call count descending
```

Tie-breaking:

```text
first occurrence order
```

Unknown numbers use:

```text
syscall_N
```

---

## Q89. What is special about `snoop -p pid`?

It attaches to an already running process using `PTRACE_ATTACH`.

The first observed ptrace stop is not necessarily equivalent to a clean syscall-entry event from a newly launched tracee.

If internal entry/exit state is initialized incorrectly, every subsequent syscall can be classified incorrectly, producing shifted counts.

Therefore the attach path needs explicit handling of the initial stop and syscall state.

---

# 26. xv6 Build Selection

## Q90. What does `SCHEDULER=MLFQ` accomplish?

The build system selects one scheduling policy at compile time.

Required behavior:

```text
no SCHEDULER -> original RR
SCHEDULER=MLFQ -> MLFQ
invalid value -> build error
```

This avoids shipping several active schedulers simultaneously.

---

# 27. MLFQ Model

## Q91. How many queues are required?

Four:

```text
0 highest
1
2
3 lowest
```

Time slices:

```text
queue 0 -> 1 tick
queue 1 -> 4 ticks
queue 2 -> 8 ticks
queue 3 -> 16 ticks
```

---

## Q92. Where does a new process go?

Every new process enters:

```text
queue 0
```

at the tail.

The initial slice-usage counter for that queue should also start at zero.

---

## Q93. What does strict priority selection mean?

The scheduler scans from queue 0 downward and selects a process from the highest-priority queue that is non-empty.

Thus a runnable queue-0 process always wins over runnable queue-1, queue-2, or queue-3 processes.

---

## Q94. What happens if a high-priority process arrives while a lower-priority process is running?

The assignment requires preemption at the next tick boundary.

For example:

```text
queue 2 process running
new queue 0 process becomes runnable
next timer tick
kernel regains control
queue 0 selected
```

The scheduler does not have to asynchronously interrupt a CPU instruction in the middle of a tick; the preemption boundary is the timer tick.

---

# 28. MLFQ Time-Slice Accounting

## Q95. What exactly counts toward a slice?

Every tick that the process actually spends running counts.

If a process runs two ticks, gets preempted, and later runs two more ticks in the same queue, then a four-tick slice is exhausted.

The two earlier ticks do not disappear merely because the scheduler dispatched another process in between.

---

## Q96. Why can `qstart` be an incorrect abstraction?

A `qstart` timestamp often measures time since the most recent dispatch.

That is different from cumulative CPU ticks spent in the queue.

If `qstart` is reset every time a process is scheduled, repeated preemptions can accidentally give the process fresh apparent slices.

The correct conceptual variable is:

```text
slice_ticks_used
```

which accumulates only ticks in which the process actually runs.

---

## Q97. What are the exact demotion points?

```text
queue 0: after 1 CPU tick -> queue 1
queue 1: after 4 CPU ticks -> queue 2
queue 2: after 8 CPU ticks -> queue 3
queue 3: after 16 CPU ticks -> queue 3 tail
```

An off-by-one error often comes from checking the counter before accounting for the current tick rather than after it.

A useful invariant is:

> Once a process has accumulated exactly the configured number of CPU ticks in its current queue slice, the slice is exhausted.

---

# 29. Voluntary Yield

## Q98. What happens when an xv6 process voluntarily yields?

If it gives up the CPU before the slice is exhausted:

```text
priority stays the same
slice accounting remains consistent with the current slice
process becomes runnable later at tail of same queue
```

No automatic promotion or demotion happens merely because of the yield.

---

# 30. Queue 3 Round-Robin

## Q99. What happens in queue 3?

Queue 3 is the lowest queue and behaves round-robin.

A queue-3 process that exhausts its 16-tick slice is returned to the tail of queue 3.

If several processes are runnable at queue 3, this produces cyclic sharing.

---

# 31. Priority Boost

## Q100. What happens every 48 ticks?

All active processes are moved to queue 0.

This is the anti-starvation mechanism.

The important effect is:

```text
queue 3 --48-tick boost--> queue 0
queue 2 --48-tick boost--> queue 0
queue 1 --48-tick boost--> queue 0
```

The process's queue-specific slice bookkeeping should be made consistent with the new queue.

---

# 32. `kill()` and Wakeup

## Q101. Where does a process awakened by `kill()` go?

The requirement is:

```text
same queue
but at the tail
```

It should not be accidentally promoted to queue 0.

Conceptually:

```text
SLEEPING in queue 2
      |
    kill/wakeup
      v
RUNNABLE at tail of queue 2
```

---

# 33. `struct proc`

## Q102. What scheduler state belongs in `struct proc`?

The required scheduler information includes concepts equivalent to:

```text
current queue
CPU ticks consumed in current slice
boost-related timing/bookkeeping if needed
```

The names can differ.

The main requirement is that the scheduler's history is explicitly represented rather than reconstructed inaccurately from the last dispatch timestamp.

---

## Q103. What should `allocproc()` initialize?

A new process should start with:

```text
queue = 0
slice_ticks_used = 0
```

and any additional boost bookkeeping should be initialized consistently.

---

# 34. xv6 Scheduler Flow

## Q104. What is the logical scheduler loop?

Conceptually:

```text
forever:
    identify highest-priority non-empty queue
    choose runnable process
    mark it RUNNING
    context switch
    regain kernel control
    account for CPU ticks / state transition
    repeat
```

The actual xv6 implementation must respect the existing locking and scheduling conventions of the kernel.

---

# 35. `procdump`

## Q105. What information should `procdump` expose?

Useful fields include:

```text
PID
name
state
queue number
slice ticks used
boost bookkeeping
```

The purpose is to make scheduler invariants visible while debugging.

For example, if a process is repeatedly interrupted, `procdump` can reveal whether its accumulated slice counter remains intact.

---

# 36. Scheduler Test Program

## Q106. What should `schedulertest` exercise?

The test program should create several child processes with different CPU-burst patterns.

Useful test cases:

```text
very short burst
moderate burst
long CPU-bound burst
repeated voluntary yields
several competing queue-3 processes
long-running process crossing a boost boundary
```

The same logical workload should be usable for FIFO, RR, and MLFQ comparisons.

---

## Q107. How can a CPU-bound process demonstrate demotion?

Make it consume CPU continuously for long enough to cross:

```text
1 tick
4 cumulative ticks in queue 1
8 cumulative ticks in queue 2
16 ticks per queue-3 slice
```

The logs should show each transition.

---

## Q108. How can a voluntary-yield process demonstrate priority retention?

Have a process:

```text
compute briefly
yield
repeat
```

If the process is in queue 1 and yields after two of four ticks, it should later resume in queue 1 rather than queue 2.

---

# 37. Scheduler Event Logs

## Q109. What should scheduler logs record?

Useful fields are:

```text
tick
PID
old queue
new queue
slice ticks used
process state
reason
```

Possible reason labels:

```text
DISPATCH
TICK
PREEMPT
DEMOTE
YIELD
BOOST
EXIT
WAKE
```

With this information, a timeline can be generated rather than guessed.

---

# 38. Plot

## Q110. What should the MLFQ plot show?

The assignment requires:

```text
x-axis = elapsed ticks
 y-axis = queue id 0..3
process identity = PID/color/marker
```

It should visibly show:

```text
queue demotion
queue retention after voluntary yield
queue-3 round-robin residence
48-tick boosts to queue 0
```

The graph should be generated from the actual log and accompanied by the required interpretation.

---

# 39. Scheduler Metrics

## Q111. What is turnaround time?

```text
completion time - arrival time
```

It measures the total time spent in the system.

---

## Q112. What is waiting time?

Waiting time is the time spent runnable/ready but not executing.

Do not count CPU execution time as waiting time.

Do not count blocked/sleeping time as ready-queue waiting time.

---

## Q113. What is response time?

```text
first CPU service time - arrival time
```

It measures how quickly the scheduler gives a process its first opportunity to run.

---

# 40. FIFO vs RR vs MLFQ Reasoning

## Q114. Why can FIFO have poor response time?

A process arriving behind a long-running process waits until the earlier process completes.

Thus a short interactive process may have excellent execution time once it starts but poor response time.

---

## Q115. Why can RR improve response time?

Round-robin gives runnable processes repeated CPU opportunities.

The quantum determines how long one process runs before another gets service.

Smaller quantum usually improves interactivity but increases context-switch overhead.

---

## Q116. Why can MLFQ improve response time?

New processes begin at high priority, so short/interactive work can receive early CPU service.

Long CPU-bound work eventually moves down to lower queues, reducing the extent to which it blocks short jobs.

Priority boosting prevents low-priority processes from starving.

---

# 41. Error Handling Principles

## Q117. What should happen if `open()` fails during redirection?

Print the exact assignment-required error and do not execute the command.

If multiple resources were already opened for that command, close them before returning.

---

## Q118. What should happen if `fork()` fails halfway through a pipeline?

The implementation must clean up all already-created pipe descriptors and any partial job/process bookkeeping.

A partial pipeline cannot be left in the job table as though it were fully launched.

---

## Q119. What should happen if `execve()` fails in a pipeline child?

The child reports the required command-not-found error and terminates.

The other children continue.

The parent still must reap the failed child.

This is distinct from a shell-level failure to start the entire command group.

---

# 42. Signal Safety

## Q120. What should not be done casually inside signal handlers?

Signal handlers can interrupt the shell at arbitrary points. Complex operations that are not async-signal-safe can create reentrancy or state corruption.

A safe design is:

```text
handler records small state/event
main execution path performs complex bookkeeping
```

Examples of state that can be recorded include a flag indicating that SIGCHLD processing is pending or a small queue of already-known child-status events if the implementation is designed for it.

---

# 43. Descriptor Ownership

## Q121. What is a useful descriptor-ownership rule?

Before `fork()`:

```text
parent owns all descriptors it created
```

After `fork()`:

```text
child closes descriptors it does not need
parent closes descriptors that are no longer needed by parent
```

Every descriptor should have an obvious lifetime.

This single discipline prevents many shell hangs and leaks.

---

# 44. Common Shell Failure Modes

## Q122. Why does a pipeline hang after the producer exits?

Usually because some process still holds the write end of the pipe.

Check:

```text
parent write end closed?
consumer write end closed?
intermediate stages closed unused ends?
```

If not, EOF never reaches the reader.

---

## Q123. Why does a background `cat` consume shell commands?

It inherited the shell's stdin and is allowed to read from it.

Redirect background stdin away from the shell command stream.

---

## Q124. Why does `activities` show stale processes?

The job table is not synchronizing its individual process records with `waitpid()` results.

The fix is to process child state transitions and remove exited PIDs before displaying jobs.

---

## Q125. Why does prompt text disappear after a background event?

The shell printed asynchronous output without preserving and redrawing the partially typed input line.

The input buffer must be independent of prompt rendering.

---

# 45. Common xv6 Failure Modes

## Q126. Why can MLFQ demote too late?

The process's slice counter may be reset whenever the scheduler dispatches it again.

That measures dispatch duration rather than cumulative CPU time.

---

## Q127. Why can MLFQ demote too early?

The code may count a scheduler event as a CPU tick or increment/check the counter in the wrong order.

Only actual running ticks should contribute to slice consumption.

---

## Q128. Why does a process appear to skip a queue?

Possible causes include:

```text
slice accounting off by one
queue update performed twice
wakeup path assigning queue 0 accidentally
boost happening at the wrong tick
```

The scheduler log should distinguish each event.

---

## Q129. Why can a process starve?

If strict priority is implemented without the 48-tick boost, a lower queue can theoretically be postponed indefinitely by continuously arriving high-priority work.

The boost is the anti-starvation mechanism required by the assignment.

---

# 46. Evaluation-Oriented Questions

## Q130. Why is the parser allowed to avoid a full AST?

The assignment says any convenient structure is valid as long as it represents enough information for later execution.

A simple token list plus command/pipeline structures is therefore sufficient.

---

## Q131. Why should builtins be distinguished from external commands?

Some builtins modify shell state:

```text
hop
activities
resume
ping
```

and some operate by inspecting shell-owned state.

A directory-changing builtin is the clearest example: executing it in a child would modify only the child's CWD.

Other builtins can also require direct access to the shell's job table, terminal ownership, or process state.

---

## Q132. What should happen if a builtin appears inside a pipeline?

The implementation needs a clear policy consistent with the assignment's expected behavior. In general, a shell has to decide whether a builtin should execute in the shell process or a pipeline child based on whether it needs to modify shell-global state.

The safe design is to keep state-changing builtins in the shell where they can change the shell itself, while pipeline execution should preserve the pipeline semantics required by the assignment.

---

# 47. Test Cases Derived from the Specification

## Q133. Parsing tests

```text


spaces only
 echo hi 
echo "a b"
echo 'a b'
echo a\ b
echo "a > b"
echo "a | b"
>>
cat <
| sort
cat | ; sort
echo hi ;
echo hi & &
echo "unterminated
echo trailing\
```

Expected behavior must match the exact syntax rules.

---

## Q134. Execution tests

```text
echo hello
./local_program
local_program
%local_program
missing_command
```

Test with a matching executable in CWD and then remove it to test PATH lookup.

---

## Q135. Redirection tests

```text
echo one > a
echo two >> a
cat < a
cat < a < b
echo x > a > b
cat < a > b >> c
```

Check file contents and whether output is still printed to the terminal.

---

## Q136. Pipeline tests

```text
printf "banana\napple" | sort
badcmd | sort
sort | badcmd
cat < input | sort > output
```

Check both output correctness and process termination.

---

## Q137. Background tests

```text
sleep 1 &
sleep 2 & sleep 3 &
cat &
sleep 1 & echo hi
cat | sort &
```

For each test verify:

```text
job number
reported PID
prompt returns immediately
completion message appears
background stdin does not swallow shell commands
```

---

## Q138. Job-control tests

```text
sleep 100
Ctrl-C

sleep 100
Ctrl-Z
activities
resume %1 bg
resume %1 fg
resume %1 fg --timeout 1
ping <pid> 9
ping %1 15
```

Also test a pipeline where one stage exits before the group is stopped.

---

# 48. Builtin Test Cases

## Q139. `hop` tests

```text
hop
hop ~
hop .
hop ..
hop -
hop absolute/path
hop nonexistent-name
```

Test previous-directory behavior when a `chdir()` fails.

Test frecency persistence across shell sessions.

---

## Q140. `reveal` tests

```text
reveal
reveal -a
reveal -t
reveal -ta
reveal target
reveal -
reveal invalidflag
```

Create filenames with spaces, uppercase/lowercase differences, numeric prefixes, hidden files, directories, and symbolic links.

---

## Q141. `peek` tests

```text
peek file
peek -n file
peek -r file
peek -rn file
peek file1 file2
peek -n file1 file2
peek -r file1 file2
peek -
```

Test files with and without a final newline.

---

## Q142. `locate` tests

```text
locate ls
locate command1 command2 missing
locate local_program
```

Test a local executable versus PATH matches.

---

## Q143. `spy` tests

```text
spy
spy <valid-pid>
spy <nonexistent-pid>
spy p1 p2
```

Check descriptor types and duplicate memory-map suppression.

---

## Q144. `snoop` tests

```text
snoop true
snoop sleep 1
snoop nonexistent
snoop -p <valid-pid>
snoop -p <invalid-pid>
```

For attach mode, choose processes in different execution states and verify syscall counts remain synchronized.

---

# 49. Final Shell Invariants

## Q145. What must always be true for the shell?

```text
Every syntax error is rejected before execution.
Every tracked process belongs to one job.
Every pipeline belongs to one process group.
Foreground job owns terminal while running.
Shell reclaims terminal afterward.
Background jobs never read shell input.
Unused pipe descriptors are closed.
Exited processes do not remain in activities.
Every opened descriptor has a close path.
Every allocated parser object has a cleanup path.
State-changing builtins execute in the shell when required.
```

---

# 50. Final xv6 Invariants

## Q146. What must always be true for MLFQ?

```text
Every runnable process has one queue.
New process -> queue 0 tail.
Highest non-empty queue always wins.
CPU ticks accumulate across scheduler preemptions.
Full slice -> demotion except queue 3.
Voluntary yield -> same queue.
Queue 3 -> round-robin.
Every 48 ticks -> active processes queue 0.
Exited processes are removed.
Wakeup/kill uses current queue tail.
RR remains the default when MLFQ is not selected.
```

---

# 51. Compact syscall reference

## Q147. What does each important syscall/function contribute?

```text
fork()       create child process
execve()     replace current process image
waitpid()    inspect/reap child state
pipe()       create unidirectional byte stream
dup2()       attach descriptor to stdin/stdout/etc.
open()       open/create files
close()      release file descriptor
read()       consume bytes
write()      produce bytes
lseek()      move offset in seekable file
chdir()      change caller's working directory
getcwd()     retrieve current working directory
setpgid()    place process in process group
getpgrp()    retrieve process group
setpgid()    establish job grouping
tcsetpgrp()  give terminal to a process group
sigaction()  install signal disposition
kill()       send signal
alarm()      schedule SIGALRM
setitimer()  configure interval/deadline timers
waitpid(WNOHANG) nonblocking child-state check
opendir()    open directory stream
readdir()    read directory entry
closedir()   close directory stream
stat/lstat() read file metadata
qsort      generic array sorting
ptrace()     trace/control another process
readlink()   read symbolic-link target
```

---

# 52. Compact data-structure reference

## Q148. What structures are useful?

### Token

```text
type
value
```

### Command

```text
argv
input redirections
output redirections
append flags
```

### Pipeline

```text
commands/stages
stage count
background flag
```

### Process record

```text
PID
command name
state
```

### Job

```text
job number
PGID
first PID
command display
process records
launch order
job state
```

### Frecency entry

```text
path
score
last visit
```

### xv6 process scheduler state

```text
queue
slice ticks used
boost bookkeeping
```

---

# 53. Debugging Method

## Q149. How should a difficult bug be attacked?

Use a small reproduction.

For the shell:

```text
reproduce
identify violated invariant
inspect tokens/IR
inspect descriptors/process groups
inspect wait status
fix one subsystem
rerun focused test
rerun regression tests
```

For xv6:

```text
reproduce
log exact tick
inspect proc state
inspect queue and slice counter
compare against specification
fix one transition
rerun identical workload
```

Avoid changing several unrelated subsystems at once because that destroys the causal connection between the bug and the fix.

---

# 54. Implementation Summary Map

## Q150. How do the major shell requirements connect?

```text
lexer
  -> correct tokens

parser
  -> correct command structure

execution
  -> fork/exec/redirection/pipes

job table
  -> PIDs/PGIDs/job numbers/states

signals
  -> Ctrl-C/Ctrl-Z/SIGCHLD/SIGHUP

terminal control
  -> tcsetpgrp and foreground groups

builtins
  -> shell-owned state

input reader
  -> Ctrl-D/prompt redraw/piped one-line consumption

cleanup
  -> no leaked memory/FDs
```

---

## Q151. How do the major xv6 requirements connect?

```text
Makefile
  -> compile-time policy

struct proc
  -> scheduler state

allocproc
  -> new-process initialization

scheduler
  -> strict queue priority

timer accounting
  -> exact slice consumption

demotion
  -> CPU-bound behavior

yield/wakeup
  -> preserve priority

boost
  -> anti-starvation

procdump/logs
  -> observability

test workload
  -> behavioral validation

plot + metrics
  -> analysis and comparison
```

---

# 55. Last-pass checklist before submission

## C shell

```text
[ ] make all succeeds
[ ] shell.out generated in shell directory
[ ] all required source/header files present
[ ] parser rejects invalid syntax before execution
[ ] quoting/escaping exact
[ ] >> recognized as one token
[ ] startup path replacement correct
[ ] root startup path correct
[ ] builtins modify shell state correctly
[ ] frecency persistent and deterministic
[ ] redirections correct
[ ] multiple redirections correct
[ ] pipelines close descriptors correctly
[ ] missing pipeline command does not stop other stages
[ ] ; stops only on failure to start
[ ] & does not block
[ ] SIGCHLD handled
[ ] background stdin isolated
[ ] jobs grouped by PGID
[ ] foreground terminal ownership correct
[ ] Ctrl-C correct
[ ] Ctrl-Z correct
[ ] Ctrl-D correct
[ ] SIGHUP cleanup correct
[ ] activities removes exited processes
[ ] resume bg/fg correct
[ ] resume timeout race handled
[ ] ping validates numbers safely
[ ] reveal recursion/sorting correct
[ ] peek newline and reverse edge cases correct
[ ] locate ordering correct
[ ] spy output correct
[ ] snoop launch/attach paths correct
[ ] memory and FD cleanup checked
```

## xv6

```text
[ ] default RR preserved
[ ] MLFQ compile path works
[ ] invalid SCHEDULER rejected
[ ] queues 0..3 correct
[ ] slices 1/4/8/16 correct
[ ] new process queue 0 tail
[ ] highest queue selected
[ ] higher-priority arrival preempts at tick boundary
[ ] cumulative CPU slice accounting correct
[ ] demotion exact
[ ] queue 3 round-robin
[ ] voluntary yield preserves queue
[ ] kill/wakeup uses same queue tail
[ ] 48-tick boost correct
[ ] exited processes removed
[ ] procdump useful
[ ] scheduler logs validated
[ ] custom scheduler test works
[ ] plot generated from logs
[ ] boost visible in plot
[ ] FIFO/RR/MLFQ use identical workload
[ ] turnaround computed
[ ] waiting computed
[ ] response computed
[ ] report discussion matches measured data
```

---

# 56. Core ideas to remember during evaluation

## Q152. What are the most important shell ideas to be able to explain verbally?

```text
fork creates a new process; exec replaces its program image.
Pipelines require one process per stage and N-1 pipes.
Closing unused pipe descriptors is required for correct EOF behavior.
Process groups let job-control signals affect an entire pipeline.
tcsetpgrp transfers terminal ownership.
SIGCHLD plus nonblocking waitpid supports background reaping.
Builtins that change shell state must run in the shell process.
The parser validates syntax before the executor runs anything.
The background stdin rule prevents jobs from consuming shell commands.
```

---

## Q153. What are the most important xv6 ideas to explain verbally?

```text
MLFQ has four queues with slices 1,4,8,16.
New processes begin at queue 0.
The scheduler always picks the highest-priority non-empty queue.
A full slice causes demotion except in queue 3.
Voluntary yield keeps the same priority.
Queue 3 is round-robin.
Every 48 ticks all active processes return to queue 0.
Slice accounting counts actual CPU ticks and survives preemption.
```

---

# 57. Important source-aligned distinctions

## Q154. What should not be confused?

### Command-not-found versus nonzero exit

```text
cannot start -> execution failure for sequence semantics
started then exit != 0 -> sequence continues
```

### Pipeline command failure versus sequence failure

```text
pipeline stage missing -> other pipeline stages continue
sequential command missing -> later commands stop
```

### Process stopped versus process exited

```text
stopped -> remains tracked
exited -> removed from activities
```

### Foreground versus background

```text
foreground -> owns terminal
background -> no terminal input
```

### Dispatch time versus CPU slice time

```text
time since last dispatch != cumulative CPU ticks
```

### `stat` versus `lstat`

```text
stat -> follows symbolic links
lstat -> describes the link itself
```

### Regular files versus stdin/pipes for reverse reading

```text
regular file -> lseek-based reverse traversal
non-seekable -> buffer then reverse
```

