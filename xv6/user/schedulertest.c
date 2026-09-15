// schedulertest - a workload for exercising the scheduler.
//
// It forks a fixed set of children with different shapes: some that only ever
// compute, some that compute a little and then sleep, which is what an I/O
// bound process looks like from the scheduler's side. The parent reaps them
// with waitstats() and reports turnaround, waiting and response times, so the
// same workload can be run under each policy and the numbers compared.
//
//   schedulertest        run the workload and report
//   schedulertest log    same, with the scheduler's event log turned on,
//                        which is what the timeline plot is drawn from

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NCHILD 5

// Roughly twenty units of this go by in one tick on qemu, so a unit is small
// enough that a process is usually preempted part way through a burst rather
// than neatly between two of them.
static void
burn(int units)
{
  volatile int sink = 0;

  for (int i = 0; i < units; i++)
    for (int j = 0; j < 2000000; j++)
      sink += j;
}

// The five shapes. Numbers are chosen so the whole run takes a few hundred
// ticks: long enough for several priority boosts, short enough to sit through.
static void
workload(int id)
{
  switch (id) {
  case 0: // about 70 ticks of nothing but computing
    burn(1400);
    break;
  case 1: // a little work, then a long sleep: the I/O bound shape
    for (int i = 0; i < 20; i++) {
      burn(10);
      pause(2);
    }
    break;
  case 2: // about 40 ticks of nothing but computing
    burn(800);
    break;
  case 3: // longer bursts and shorter sleeps: half and half
    for (int i = 0; i < 16; i++) {
      burn(40);
      pause(1);
    }
    break;
  default: // about 70 ticks of nothing but computing
    burn(1400);
    break;
  }
}

int
main(int argc, char *argv[])
{
  // Filled in as the children are reaped, so in the order they finished.
  int pids[NCHILD];
  int turnaround[NCHILD], waiting[NCHILD], response[NCHILD];
  int logging = argc > 1 && strcmp(argv[1], "log") == 0;
  int tsum = 0, wsum = 0, rsum = 0;
  int reaped = 0;
  int start;

  if (logging)
    schedlog(1);

  start = uptime();
  for (int i = 0; i < NCHILD; i++) {
    int pid = fork();

    if (pid < 0) {
      printf("schedulertest: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      workload(i);
      exit(0);
    }
    printf("child %d pid %d\n", i, pid);
  }

  for (int i = 0; i < NCHILD; i++) {
    int t = 0, w = 0, r = 0;
    int pid = waitstats(&t, &w, &r);

    if (pid < 0)
      break;
    turnaround[reaped] = t;
    waiting[reaped] = w;
    response[reaped] = r;
    pids[reaped] = pid;
    reaped++;
    tsum += t;
    wsum += w;
    rsum += r;
  }

  if (logging)
    schedlog(0);

  printf("\n");
  printf("pid  turnaround  waiting  response\n");
  for (int i = 0; i < reaped; i++)
    printf("%d    %d          %d       %d\n", pids[i], turnaround[i],
           waiting[i], response[i]);

  if (reaped > 0) {
    // No floating point in xv6's printf, so the averages are printed to one
    // decimal place by hand.
    printf("\naverage turnaround %d.%d\n", tsum / reaped,
           (tsum * 10 / reaped) % 10);
    printf("average waiting    %d.%d\n", wsum / reaped,
           (wsum * 10 / reaped) % 10);
    printf("average response   %d.%d\n", rsum / reaped,
           (rsum * 10 / reaped) % 10);
  }
  printf("total ticks        %d\n", uptime() - start);
  exit(0);
}
