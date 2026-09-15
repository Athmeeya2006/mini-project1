# Bar chart of the three metrics under the three schedulers.
#
# Input is the output of schedulertest, one file per policy, as saved from the
# xv6 console: run_fifo.txt, run_rr.txt and run_mlfq.txt. The averages are read
# out of the "average ..." lines at the end of each file.
#
#     python3 plot_comparison.py [scheduler_comparison.png]

import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

OUT = sys.argv[1] if len(sys.argv) > 1 else "scheduler_comparison.png"

FILES = [("FIFO", "run_fifo.txt"), ("RR", "run_rr.txt"), ("MLFQ", "run_mlfq.txt")]
METRICS = ["turnaround", "waiting", "response"]


def averages(path):
    out = {}
    for line in open(path, errors="replace"):
        m = re.match(r"average (\w+)\s+([0-9.]+)", line.strip())
        if m:
            out[m.group(1)] = float(m.group(2))
    missing = [k for k in METRICS if k not in out]
    if missing:
        sys.exit("%s: no %s line" % (path, ", ".join(missing)))
    return out


data = {name: averages(path) for name, path in FILES}

x = np.arange(len(METRICS))
width = 0.26
fig, ax = plt.subplots(figsize=(8, 5))
colours = {"FIFO": "#8a8f98", "RR": "#4c78a8", "MLFQ": "#e45756"}

for i, (name, _) in enumerate(FILES):
    vals = [data[name][m] for m in METRICS]
    bars = ax.bar(x + (i - 1) * width, vals, width, label=name,
                  color=colours[name])
    ax.bar_label(bars, fmt="%.1f", fontsize=9, padding=2)

ax.set_xticks(x)
ax.set_xticklabels(["turnaround", "waiting", "response"])
ax.set_ylabel("ticks (average over the five test processes)")
ax.set_title("Same workload under each scheduler, one CPU, lower is better")
ax.legend()
ax.grid(axis="y", alpha=0.25)
ax.set_axisbelow(True)

plt.text(
    0.95, 0.95, "athmeeya.kashyap",
    ha='right', va='top',
    transform=plt.gca().transAxes,
    fontsize=10, color="gray", alpha=0.7
)

plt.tight_layout()
plt.savefig(OUT, dpi=150)
print("wrote", OUT)
