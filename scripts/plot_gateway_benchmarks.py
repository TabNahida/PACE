#!/usr/bin/env python3
"""Render the complete gateway throughput comparison from its raw JSON report."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
data = json.loads(args.input.read_text())
backends = ["epoll", "io_uring", "nginx"]
colors = ["#45a98e", "#8276cf", "#798da4"]
scenarios = ["static", "static_cached", "proxy", "proxy_cached"]
fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharey=True)
for ax, workers in zip(axes, [1, 4]):
    for index, backend in enumerate(backends):
        stats = [next(s["requests_per_second"] for s in data["summaries"]
                      if (s["scenario"], s["workers"], s["backend"]) == (scenario, workers, backend)) for scenario in scenarios]
        values = [s["median"] / 1000 for s in stats]
        errors = [[(s["median"] - s["min"]) / 1000 for s in stats], [(s["max"] - s["median"]) / 1000 for s in stats]]
        ax.bar([x + (index - 1) * .24 for x in range(4)], values, .23, yerr=errors,
               label=["PACE epoll", "PACE io_uring", "nginx"][index], color=colors[index], capsize=3)
    ax.set_title(f"{workers} frontend worker{'s' if workers > 1 else ''}")
    ax.set_xticks(range(4), ["Static", "Static\ncached", "Proxy", "Proxy\ncached"])
    ax.set_axisbelow(True); ax.grid(axis="y", alpha=.2)
    ax.spines[["top", "right"]].set_visible(False)
axes[0].set_ylabel("Throughput (thousand requests/s)")
axes[0].set_ylim(0, max(s["requests_per_second"]["max"] for s in data["summaries"]) / 1000 * 1.10)
fig.legend(*axes[1].get_legend_handles_labels(), loc="upper center", bbox_to_anchor=(.5, .93), ncol=3, frameon=False, fontsize=9)
fig.suptitle("Local Linux / WSL2 — shared host, complete workload matrix", fontsize=13)
fig.text(.5, .025, "32 connections · 4 KiB responses · medians of 5 trials · whiskers: trial min–max, not confidence intervals", ha="center", fontsize=9)
fig.tight_layout(rect=(0, .07, 1, .88))
args.output.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(args.output)
