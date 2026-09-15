#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern uint ticks;

// Handed out in order, so a smaller number means a place nearer the head of
// whichever queue the process is sitting in. Putting a process at the back of
// a queue is just giving it the next number.
static uint64 nextqseq = 1;

// Whether the scheduler prints a line every time a process changes queue or
// gets the CPU. It is turned on and off from user space with schedlog(), so
// that an ordinary session is not drowned in output.
int sched_logging = 0;

static uint64
queue_seq(void)
{
  return __sync_fetch_and_add(&nextqseq, 1);
}

// One line per scheduling event. The timeline plot in the report is built out
// of these: the tick it happened on, which process, the queue it is in now,
// and what happened to it.
static void
schedlog(char event, struct proc *p)
{
  if (sched_logging)
    printk("MLFQ %d %d %d %c\n", ticks, p->pid, p->qlevel, event);
}

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  p->ctime = ticks;
  p->etime = 0;
  p->rtime = 0;
  p->wtime = 0;
  p->sltime = 0;
  p->first_run = -1;
  // A new process is pushed to the back of the highest queue.
  p->qlevel = 0;
  p->qticks = 0;
  p->qstart = ticks;
  p->qseq = queue_seq();
  schedlog('N', p);

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->etime = ticks;
  // It leaves the queuing network here: the scheduler only ever looks at
  // runnable processes.
  schedlog('E', p);

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Hand the three comparison metrics of an exited child back to its parent.
static int
copy_times(struct proc *parent, struct proc *child, uint64 taddr, uint64 waddr,
           uint64 raddr)
{
  int turnaround = child->etime - child->ctime;
  int waiting = child->wtime;
  int response = child->first_run < 0 ? -1 : child->first_run - child->ctime;

  if (taddr != 0 && copyout(parent->pagetable, parent->sz, taddr,
                            (char *)&turnaround, sizeof(turnaround)) < 0)
    return -1;
  if (waddr != 0 && copyout(parent->pagetable, parent->sz, waddr,
                            (char *)&waiting, sizeof(waiting)) < 0)
    return -1;
  if (raddr != 0 && copyout(parent->pagetable, parent->sz, raddr,
                            (char *)&response, sizeof(response)) < 0)
    return -1;
  return 0;
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
//
// The last three addresses are where the child's turnaround, waiting and
// response times are written, and any of them may be zero to say the caller
// is not interested. Plain wait() passes zero for all three; waitstats(),
// which the scheduler comparison uses, does not.
int
kwait(uint64 addr, uint64 taddr, uint64 waddr, uint64 raddr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent == p) {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // Found one.
          pid = pp->pid;
          if (addr != 0 &&
              copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          // Read out of the child before freeproc() wipes it. Waiting time is
          // measured directly, as the ticks it spent runnable without a CPU,
          // rather than worked out from the other two, so that time spent
          // asleep is not counted as time spent waiting for a turn.
          if (copy_times(p, pp, taddr, waddr, raddr) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          pp->parent = 0;
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

// Bring everyone's timing up to date and charge the running processes for the
// tick that has just gone by. Called once a tick, from the timer interrupt on
// CPU 0, so a process running on any CPU is charged exactly once.
void
update_time(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    switch (p->state) {
    case RUNNING:
      p->rtime++;
#ifdef MLFQ
      // The tick that has just ended is charged against the slice only if the
      // process held the CPU for the whole of it. A process that was given the
      // CPU part way through, ran for a moment and is still there when the
      // interrupt lands has not used a slice's worth of anything, and billing
      // it for one would push short lived work down the queues for no reason.
      if ((int)ticks - 1 > p->qstart)
        p->qticks++;
#endif
      break;
    case RUNNABLE:
      p->wtime++;
      break;
    case SLEEPING:
      p->sltime++;
      break;
    default:
      break;
    }
    release(&p->lock);
  }

#ifdef MLFQ
  mlfq_boost();
#endif
}

#ifdef MLFQ

// Ticks gone by since the last priority boost, kept out here so that procdump
// can show how close the next one is.
static int ticks_since_boost = 0;

// The slice, in ticks, that each queue hands out.
static int
mlfq_slice(int level)
{
  static const int slice[NQUEUE] = { 1, 4, 8, 16 };

  if (level < 0)
    level = 0;
  if (level >= NQUEUE)
    level = NQUEUE - 1;
  return slice[level];
}

// Every MLFQ_BOOST ticks everything in the system goes back to the highest
// queue. Without it a queue full of short jobs would starve whatever has sunk
// to the bottom. Positions are left alone, so processes keep the order they
// were already in.
void
mlfq_boost(void)
{
  struct proc *p;

  if (++ticks_since_boost < MLFQ_BOOST)
    return;
  ticks_since_boost = 0;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state != UNUSED && p->state != ZOMBIE) {
      p->qlevel = 0;
      p->qticks = 0;
      schedlog('B', p);
    }
    release(&p->lock);
  }
}

// Is anything runnable in a queue above this one?
static int
mlfq_higher_runnable(struct proc *self, int level)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    if (p == self)
      continue;
    acquire(&p->lock);
    if (p->state == RUNNABLE && p->qlevel < level) {
      release(&p->lock);
      return 1;
    }
    release(&p->lock);
  }
  return 0;
}

// Called from the timer interrupt, and the only place a running process is
// taken off the CPU against its will. Either it has used up the slice its
// queue hands out, and drops a queue for it, or something in a higher queue
// has become runnable, and it keeps its queue but goes to the back of it.
// Both only ever happen on a tick boundary, which is all the specification
// asks for.
void
mlfq_on_timer(void)
{
  struct proc *p = myproc();
  int demote;

  if (p == 0)
    return;

  acquire(&p->lock);
  if (p->state != RUNNING) {
    release(&p->lock);
    return;
  }
  demote = p->qticks >= mlfq_slice(p->qlevel);
  release(&p->lock);

  if (!demote && !mlfq_higher_runnable(p, p->qlevel))
    return;

  acquire(&p->lock);
  if (demote) {
    // Down a queue, or back to the end of the bottom one, and the allowance
    // starts again because the queue it is now in hands out a different one.
    if (p->qlevel < NQUEUE - 1)
      p->qlevel++;
    p->qticks = 0;
  }
  // A process pushed aside by something more urgent keeps what it has already
  // used of its slice. Otherwise a busy machine could keep a process at the
  // top of the network for ever by never letting it finish a slice.
  p->qseq = queue_seq();
  p->state = RUNNABLE;
  schedlog(demote ? 'D' : 'P', p);
  sched();
  release(&p->lock);
}

// Per-CPU process scheduler, multi level feedback queue.
//
// The queues are not kept as four lists. Each process carries the queue it is
// in and its place in that queue, and the scheduler picks the runnable process
// with the smallest (queue, place) pair. That is the same thing as taking the
// head of the highest non-empty queue, and it needs no data structure of its
// own, so nothing has to be kept in step with the process table.
void
scheduler(void)
{
  struct proc *p, *best;
  struct cpu *c = mycpu();
  int level;
  uint64 seq;

  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();
    intr_off();

    best = 0;
    level = 0;
    seq = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE &&
          (best == 0 || p->qlevel < level ||
           (p->qlevel == level && p->qseq < seq))) {
        best = p;
        level = p->qlevel;
        seq = p->qseq;
      }
      release(&p->lock);
    }

    if (best == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
      continue;
    }

    acquire(&best->lock);
    // It may have been taken by another CPU while the table was being read.
    if (best->state == RUNNABLE) {
      best->state = RUNNING;
      best->qstart = ticks;
      if (best->first_run < 0)
        best->first_run = ticks;
      schedlog('R', best);
      c->proc = best;
      swtch(&c->context, &best->context);

      // Don't re-enable interrupts on release.
      mycpu()->intena = 0;
      c->proc = 0;
    }
    release(&best->lock);
  }
}

#elif defined(FIFO)

// Per-CPU process scheduler, first come first served.
//
// The oldest runnable process wins and keeps the CPU until it exits or blocks:
// the timer never takes it away (see usertrap). Age is the tick the process
// was created on, with the pid breaking ties between processes created on the
// same tick.
void
scheduler(void)
{
  struct proc *p, *best;
  struct cpu *c = mycpu();
  int ctime, pid;

  c->proc = 0;
  for (;;) {
    intr_on();
    intr_off();

    best = 0;
    ctime = 0;
    pid = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE &&
          (best == 0 || p->ctime < ctime ||
           (p->ctime == ctime && p->pid < pid))) {
        best = p;
        ctime = p->ctime;
        pid = p->pid;
      }
      release(&p->lock);
    }

    if (best == 0) {
      asm volatile("wfi");
      continue;
    }

    acquire(&best->lock);
    if (best->state == RUNNABLE) {
      best->state = RUNNING;
      if (best->first_run < 0)
        best->first_run = ticks;
      c->proc = best;
      swtch(&c->context, &best->context);

      mycpu()->intena = 0;
      c->proc = 0;
    }
    release(&best->lock);
  }
}

#else

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        if (p->first_run < 0)
          p->first_run = ticks;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Don't re-enable interrupts on release.
        mycpu()->intena = 0;

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

#endif

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (__atomic_load_n(&first, __ATOMIC_ACQUIRE)) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    // ensure other cores see first=0.
    __atomic_store_n(&first, 0, __ATOMIC_RELEASE);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    // It leaves the queuing network here, with its priority untouched, and
    // the slice it had started is over: what it did not use is not carried
    // into the next one.
    p->qticks = 0;
    schedlog('Y', p);
    sched();
  }
  release(&p->lock);
}

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;

      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNING.
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
        // A process that gave the CPU up on its own comes back to the tail of
        // the queue it left, at the priority it left with.
        p->qseq = queue_seq();
        schedlog('W', p);
      }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie"
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("%d %s %s", p->pid, state, p->name);
#ifdef MLFQ
    printk(" q%d slice %d/%d boost_in %d", p->qlevel, p->qticks,
           mlfq_slice(p->qlevel), MLFQ_BOOST - ticks_since_boost);
#endif
    printk(" run %d wait %d sleep %d", p->rtime, p->wtime, p->sltime);
    printk("\n");
  }
}
