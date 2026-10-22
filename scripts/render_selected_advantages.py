#!/usr/bin/env python3
"""Plot explicitly selected median leads; full comparisons belong in performance.md.

Example: --panel report.json p99_us libuv --panel report.json requests_per_second asio
Requires the same validated raw evidence as render_performance_chart.py.
"""
import argparse
from pathlib import Path

from render_performance_chart import load_evidence, plt, ROOT

METRICS = {
    "requests_per_second": ("Throughput", "k requests/s · higher is better", 1000, True),
    "p50_us": ("p50 latency", "µs · lower is better", 1, False),
    "p95_us": ("p95 latency", "µs · lower is better", 1, False),
    "p99_us": ("p99 latency", "µs · lower is better", 1, False),
    "peak_process_memory_bytes": ("Process peak memory", "MiB · clients + server", 1048576, False),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--panel", action="append", nargs=3, required=True,
                        metavar=("EVIDENCE", "METRIC", "COMPARATOR"))
    parser.add_argument("--output", type=Path, default=ROOT / "docs/assets/selected-performance.svg")
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    if not 1 <= len(args.panel) <= 3:
        parser.error("select one to three panels")
    selected = []
    try:
        for filename, metric, competitor in args.panel:
            if metric not in METRICS or competitor not in ("asio", "libuv"):
                raise ValueError("select a supported metric and asio or libuv")
            environment, summary = load_evidence(Path(filename))
            rows = {row["backend"]: row for row in summary}
            pace = rows["pace"][f"median_{metric}"]
            baseline = rows[competitor][f"median_{metric}"]
            higher = METRICS[metric][3]
            if not (pace > baseline if higher else pace < baseline):
                raise ValueError(f"{filename}: PACE has no median lead in {metric} vs {competitor}")
            selected.append((environment, rows, metric, competitor))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    if args.output.suffix != ".svg":
        parser.error("output must be SVG")
    input_paths = {Path(panel[0]).resolve() for panel in args.panel}
    if args.output.resolve() in input_paths or (args.preview and
            args.preview.resolve() in input_paths | {args.output.resolve()}):
        parser.error("output paths must not overwrite evidence or each other")

    plt.rcParams.update({"font.family": "DejaVu Sans", "svg.fonttype": "none",
                         "svg.hashsalt": "pace-selected-advantages-v2"})
    figure, axes = plt.subplots(1, len(selected), figsize=(14, 5.2), squeeze=False)
    figure.patch.set_facecolor("#101827")
    figure.suptitle("Selected measured advantages", x=.05, y=.95, ha="left",
                   color="#f3f6fc", fontsize=22, fontweight="bold")
    figure.text(.05, .86, "Pairwise median leads in specific TCP echo workloads · full results include all three libraries",
                color="#b9c8df", fontsize=11)
    for ax, (environment, rows, metric, competitor) in zip(axes[0], selected):
        title, units, scale, higher = METRICS[metric]
        chosen = [rows["pace"], rows[competitor]]
        values = [row[f"median_{metric}"] / scale for row in chosen]
        low = [row[f"min_{metric}"] / scale for row in chosen]
        high = [row[f"max_{metric}"] / scale for row in chosen]
        advantage = (values[0] / values[1] - 1) * 100 if higher else (1 - values[0] / values[1]) * 100
        ax.set_facecolor("#101827")
        ax.barh([1, 0], values, height=.46, color=["#65dfc5", "#7589b4"],
                xerr=[[a-b for a,b in zip(values,low)], [a-b for a,b in zip(high,values)]],
                error_kw={"ecolor":"#e4eaf5", "capsize":3, "elinewidth":1.2})
        ax.set_yticks([1,0], ["PACE", "Asio" if competitor=="asio" else "libuv"], color="#eef4ff", fontsize=11)
        ax.set_xlim(0,max(high)*1.26)
        ax.set_ylim(-.5,1.6)
        ax.set_title(f"{title} · {advantage:.1f}% {'higher' if higher else 'lower'}", loc="left",
                     color="#eef4ff", fontsize=12, pad=12)
        ax.tick_params(axis="x",colors="#99adc9",labelsize=9,length=0)
        ax.tick_params(axis="y",length=0)
        ax.set_xlabel(units,color="#99adc9",fontsize=9)
        ax.grid(axis="x",color="#29364b",linewidth=.7); ax.set_axisbelow(True)
        for spine in ax.spines.values(): spine.set_visible(False)
        for y, value, upper in zip([1,0],values,high):
            ax.text(upper+max(high)*.025,y,f"{value:,.2f}",va="center",color="#eef4ff",fontsize=10)
        config=environment["configuration"]
        ax.text(0,-.44,f"{environment['label']} · {environment.get('started_utc', '')[:10]}\n{config['clients']} clients · {config['bytes']} B · {config['repeats']} trials/backend",
                transform=ax.transAxes,color="#b9c8df",fontsize=9,va="top")
    figure.text(.05,.10,"Bars: medians · whiskers: min–max · selected comparisons are not an overall ranking or statistical confidence intervals.",
                color="#99adc9",fontsize=9)
    figure.text(.05,.045,"See docs/performance.md for throughput, p50/p95/p99, process memory, workload details, raw evidence and regressions.",
                color="#99adc9",fontsize=9)
    figure.subplots_adjust(left=.10,right=.965,bottom=.40,top=.72,wspace=.38)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    figure.savefig(args.output,facecolor=figure.get_facecolor(),metadata={"Date":None,
        "Description":"Explicit selected median leads, all source reports include PACE, Asio and libuv. Whiskers are min-max, not confidence intervals."})
    args.output.write_text("\n".join(line.rstrip() for line in args.output.read_text().splitlines())+"\n")
    if args.preview: figure.savefig(args.preview,dpi=140,facecolor=figure.get_facecolor())
    plt.close(figure)
    print(args.output)


if __name__ == "__main__":
    main()
