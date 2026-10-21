#!/usr/bin/env python3
"""Render an explicit, newly measured TCP comparison with latency and process memory.

Optional documentation dependency: matplotlib (not required to build PACE).
Example: python scripts/render_performance_chart.py --evidence benchmark-results/linux.json
Historical reports lacking extended metrics are rejected; nothing is imputed.
"""

import argparse
import json
from pathlib import Path
from types import SimpleNamespace

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from run_benchmarks import MEMORY_SOURCES, summarize, validate_record

ROOT = Path(__file__).resolve().parents[1]
BACKENDS = ("pace", "asio", "libuv")
LABELS = ("PACE", "Asio", "libuv")


def load_evidence(path):
    report = json.loads(path.read_text(encoding="utf-8"))
    environment = report.get("environment", {})
    if (environment.get("status") != "completed" or environment.get("failure") is not None or
            environment.get("measurement_kind") != "uninstrumented_benchmark" or
            environment.get("fair_backend_comparison") is not True):
        raise ValueError("chart requires a completed, uninstrumented backend comparison")
    build = environment.get("backend_versions", {})
    if (type(build.get("extended_metrics_version")) is not int or build["extended_metrics_version"] != 1 or
            build.get("pace_profiling") is not False or
            not isinstance(build.get("peak_process_memory_source"), str) or
            build["peak_process_memory_source"] not in MEMORY_SOURCES):
        raise ValueError("chart requires measured extended metrics; legacy reports are insufficient")
    configuration = environment.get("configuration", {})
    if type(configuration.get("repeats")) is not int or configuration["repeats"] < 2:
        raise ValueError("chart requires at least two trials per backend")
    for field in ("clients", "requests", "bytes", "warmup"):
        if type(configuration.get(field)) is not int or configuration[field] < (0 if field == "warmup" else 1):
            raise ValueError(f"invalid workload: {field}")
    config = SimpleNamespace(**configuration)
    records = report.get("runs", [])
    if not isinstance(records, list) or len(records) != 3 * config.repeats:
        raise ValueError("chart requires all three backends and every repeated trial")
    for record in records:
        if not isinstance(record, dict) or record.get("backend") not in BACKENDS:
            raise ValueError("unexpected backend")
        validate_record(record, record["backend"], record.get("trial"), config,
                        require_extended=True, expected_memory_source=build["peak_process_memory_source"])
    rows = summarize(records, BACKENDS, config.repeats)
    stored = report.get("summary")
    if not isinstance(stored, list) or len(stored) != 3:
        raise ValueError("missing aggregate evidence")
    by_backend = {row["backend"]: row for row in stored}
    for row in rows:
        if by_backend.get(row["backend"]) != row:
            raise ValueError("stored summary differs from raw records")
    return environment, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", required=True, type=Path, help="New combined JSON report or directory of reports")
    parser.add_argument("--platform", default="ubuntu-latest-cxx20", help="Report basename when --evidence is a directory")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/assets/selected-performance.svg")
    parser.add_argument("--preview", type=Path, help="Optional local PNG preview")
    args = parser.parse_args()
    source = args.evidence / (args.platform + ".json") if args.evidence.is_dir() else args.evidence
    try:
        environment, rows = load_evidence(source)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    if args.output.suffix.lower() != ".svg":
        parser.error("--output must end in .svg")
    if args.output.resolve() == source.resolve() or (args.preview and args.preview.resolve() in (source.resolve(), args.output.resolve())):
        parser.error("output paths must not overwrite evidence or each other")
    config = environment["configuration"]
    plt.rcParams.update({"font.family": "DejaVu Sans", "svg.fonttype": "none",
                         "svg.hashsalt": "pace-multi-metric-performance-v1"})
    fig, axes = plt.subplots(1, 3, figsize=(14, 5.4))
    fig.patch.set_facecolor("#101827")
    fig.suptitle("TCP echo · throughput, latency and process memory", x=.05, y=.95,
                 ha="left", color="#f3f6fc", fontsize=20, fontweight="bold")
    fig.text(.05, .86, f"{environment['label']} · all three backends · selected workload, not universal superiority",
             color="#b9c8df", fontsize=11)
    metrics = [("requests_per_second", 1000, "Throughput", "k requests/s · higher is better"),
               ("p99_us", 1, "p99 round-trip latency", "µs · lower is better"),
               ("peak_process_memory_bytes", 1048576, "Whole-process peak memory", "MiB · includes clients + server")]
    for ax, (metric, scale, title, units) in zip(axes, metrics):
        values = [row[f"median_{metric}"] / scale for row in rows]
        low = [row[f"min_{metric}"] / scale for row in rows]
        high = [row[f"max_{metric}"] / scale for row in rows]
        ax.set_facecolor("#101827")
        bars = ax.barh([2, 1, 0], values, color=["#65dfc5", "#7589b4", "#af91db"], height=.48,
                       xerr=[[median-minimum for median, minimum in zip(values, low)],
                             [maximum-median for maximum, median in zip(high, values)]],
                       error_kw={"ecolor": "#e4eaf5", "capsize": 3, "elinewidth": 1.2})
        ax.set_yticks([2, 1, 0], LABELS, color="#eef4ff", fontsize=11)
        maximum = max(high)
        ax.set_xlim(0, maximum * 1.32)
        ax.set_ylim(-.55, 2.6)
        ax.tick_params(axis="x", colors="#99adc9", labelsize=9, length=0)
        ax.tick_params(axis="y", length=0)
        ax.grid(axis="x", color="#29364b", linewidth=.7)
        ax.set_axisbelow(True)
        ax.set_xlabel(units, color="#99adc9", fontsize=9)
        ax.set_title(title, color="#eef4ff", fontsize=12, loc="left", pad=12)
        for spine in ax.spines.values():
            spine.set_visible(False)
        for bar, value, upper in zip(bars, values, high):
            ax.text(upper + maximum * .025, bar.get_y() + bar.get_height()/2, f"{value:,.2f}",
                    va="center", color="#eef4ff", fontsize=10)
    fig.text(.05, .22, f"{config['repeats']} trials/backend · 1 server thread · {config['clients']} clients · {config['bytes']} B · "
             f"{config['requests']:,} requests/client + {config['warmup']:,} warmup", color="#b9c8df", fontsize=9)
    fig.text(.05, .17, "Bars: medians across trials · whiskers: min–max · p99 is the median of per-trial percentiles",
             color="#99adc9", fontsize=9)
    fig.text(.05, .12, "Memory: OS peak RSS/working set through shutdown, including clients, stacks and latency samples; not isolated library footprint.",
             color="#99adc9", fontsize=9)
    fig.text(.05, .07, f"Snapshot: {environment.get('started_utc', '')[:10]} · inspect full evidence and regressions before generalizing.",
             color="#99adc9", fontsize=9)
    fig.subplots_adjust(left=.10, right=.965, bottom=.35, top=.72, wspace=.40)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, facecolor=fig.get_facecolor(), metadata={"Date": None, "Description":
                "Selected workload, all backends. Whole-process peak memory includes blocking clients and server; not isolated library footprint."})
    args.output.write_text("\n".join(line.rstrip() for line in args.output.read_text(encoding="utf-8").splitlines()) + "\n", encoding="utf-8")
    if args.preview:
        fig.savefig(args.preview, dpi=140, facecolor=fig.get_facecolor())
    plt.close(fig)
    print(args.output)


if __name__ == "__main__":
    main()
