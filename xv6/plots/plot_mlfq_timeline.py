# Draws the queue timeline for one MLFQ run.
#
# Input is the scheduler's event log, one line per event, as printed by the
# kernel when schedulertest is run as "schedulertest log":
#
#     MLFQ <tick> <pid> <queue> <event>
#
# Events are N new, R picked to run, D demoted a queue, P preempted by
# something more urgent, Y gave the CPU up on its own, W woken again,
# B boosted, E exited.
#
#     python3 plot_mlfq_timeline.py [mlfq_events.log [mlfq_timeline.png]]

import re
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LOG = sys.argv[1] if len(sys.argv) > 1 else "mlfq_events.log"
OUT = sys.argv[2] if len(sys.argv) > 2 else "mlfq_timeline.png"

LINE = re.compile(r"MLFQ (\d+) (\d+) (\d+) ([A-Z])\s*$")

events = []
with open(LOG, errors="replace") as f:
    for line in f:
        m = LINE.match(line.strip())
        if m:
            events.append((int(m.group(1)), int(m.group(2)),
                           int(m.group(3)), m.group(4)))

if not events:
    sys.exit("no events in " + LOG)

# The shell and init are in the log too. Only the test processes are of
# interest, and those are the ones that were created while logging was on.
test_pids = sorted({pid for _, pid, _, ev in events if ev == "N"})

# The queue each process is in over time, as a step function: an event says
# what queue the process is in from that tick until its next event.
track = defaultdict(list)
for t, pid, q, ev in events:
    if pid in test_pids:
        track[pid].append((t, q, ev))

end = max(t for t, _, _, _ in events)
boosts = sorted({t for t, _, _, ev in events if ev == "B"})

fig, ax = plt.subplots(figsize=(11, 5))
colours = plt.get_cmap("tab10")

for i, pid in enumerate(test_pids):
    xs, ys = [], []
    for t, q, _ in track[pid]:
        xs.append(t)
        ys.append(q)
    # carry the last queue out to the end of the run
    last_ev = track[pid][-1][2]
    if last_ev != "E":
        xs.append(end)
        ys.append(ys[-1])
    colour = colours(i % 10)
    # a small vertical offset per process, or overlapping lines hide each other
    off = (i - (len(test_pids) - 1) / 2) * 0.08
    ax.step(xs, [y + off for y in ys], where="post", color=colour,
            linewidth=1.4, alpha=0.85, label="pid %d" % pid)
    runs = [(t, q) for t, q, ev in track[pid] if ev == "R"]
    if runs:
        ax.scatter([t for t, _ in runs], [q + off for _, q in runs],
                   color=colour, s=14, zorder=3)

for j, b in enumerate(boosts):
    ax.axvline(b, color="gray", linestyle="--", linewidth=0.9, alpha=0.7,
               label="priority boost" if j == 0 else None)

ax.set_xlabel("time since scheduler start (ticks)")
ax.set_ylabel("queue")
ax.set_yticks([0, 1, 2, 3])
ax.set_ylim(3.6, -0.6)
ax.set_xlim(0, end)
ax.set_title("MLFQ: queue occupied by each process over time "
             "(dots mark when a process was given the CPU)")
ax.grid(axis="y", alpha=0.25)
ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.13), ncol=6,
          fontsize=9, frameon=False)

plt.text(
    0.95, 0.95, "athmeeya.kashyap",
    ha='right', va='top',
    transform=plt.gca().transAxes,
    fontsize=10, color="gray", alpha=0.7
)

plt.tight_layout()
plt.savefig(OUT, dpi=150, bbox_inches="tight")
print("wrote", OUT)
