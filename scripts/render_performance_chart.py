#!/usr/bin/env python3
"""Render the README's explicitly selected median leads from archived evidence.

Optional documentation dependency: matplotlib (not required to build PACE).
Run from any directory: python scripts/render_performance_chart.py
"""
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "benchmarks/results/ci-workers-2026-10-20"
# Explicit editorial selection, not a claim about every backend or platform.
SELECTION = [
    ("macOS arm64 · C++20", "macos-latest-cxx20.json", "asio", "Asio"),
    ("Linux · C++23", "ubuntu-latest-cxx23.json", "libuv", "libuv"),
]


def main():
    plt.rcParams.update({"font.family": "DejaVu Sans", "svg.fonttype": "none",
                         "svg.hashsalt": "pace-selected-performance-v1"})
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))
    fig.patch.set_facecolor("#101827")
    fig.suptitle("Selected TCP echo median leads", x=.065, y=.95, ha="left",
                 color="#f3f6fc", fontsize=21, fontweight="bold")
    fig.text(.065, .835, "PACE vs one named comparator per panel · higher is better",
             color="#b9c8df", fontsize=11)
    for ax, (platform, filename, backend, label) in zip(axes, SELECTION):
        report = json.loads((EVIDENCE / filename).read_text())
        rows = {row["backend"]: row for row in report["summary"]}
        assert all(not row["instrumented"] and row["trials"] == 5 for row in rows.values())
        baseline = rows[backend]["median_requests_per_second"]
        pace = rows["pace"]["median_requests_per_second"]
        ratio = pace / baseline
        assert ratio > 1
        ax.set_facecolor("#101827")
        bars = ax.barh([1, 0], [ratio, 1], color=["#65dfc5", "#7589b4"], height=.47)
        ax.set_yticks([1, 0], ["PACE", label], color="#eef4ff", fontsize=12)
        ax.set_xlim(0, 1.65)
        ax.set_ylim(-.55, 1.55)
        ax.xaxis.set_major_locator(MultipleLocator(.5))
        ax.tick_params(axis="x", colors="#99adc9", labelsize=9, length=0)
        ax.tick_params(axis="y", length=0)
        ax.grid(axis="x", color="#29364b", linewidth=.7)
        ax.set_axisbelow(True)
        ax.set_xlabel("Throughput / comparator median (×)", color="#99adc9", fontsize=9)
        ax.set_title(platform, color="#eef4ff", fontsize=12, loc="left", pad=12)
        for spine in ax.spines.values():
            spine.set_visible(False)
        for bar, value, rps in zip(bars, [ratio, 1], [pace, baseline]):
            ax.text(value + .04, bar.get_y() + bar.get_height()/2,
                    f"{value:.2f}×\n{rps:,.0f} req/s", va="center",
                    color="#eef4ff", fontsize=10)
    fig.text(.065, .135, "5 trials · 1 server thread · 4 clients · 64 B · 10,000 requests/client + 1,000 warmup",
             color="#b9c8df", fontsize=9)
    fig.text(.065, .075, "Shared CI snapshot · 20 Oct 2026 · selected medians, not statistical superiority",
             color="#99adc9", fontsize=9)
    fig.subplots_adjust(left=.12, right=.965, bottom=.29, top=.72, wspace=.36)
    destination = ROOT / "docs/assets/selected-performance.svg"
    fig.savefig(destination, facecolor=fig.get_facecolor(), metadata={"Date": None})
    destination.write_text("\n".join(line.rstrip() for line in
                                    destination.read_text(encoding="utf-8").splitlines()) + "\n",
                           encoding="utf-8")
    # PNG is a local preview; only the crisp, accessible SVG is checked in.
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    if args.preview:
        fig.savefig(args.preview, dpi=140, facecolor=fig.get_facecolor())
    plt.close(fig)
    print(destination)


if __name__ == "__main__":
    main()
