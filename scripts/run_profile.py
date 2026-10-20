#!/usr/bin/env python3
"""Summarize PACE stage timings from an instrumented diagnostic report."""

import argparse
import json
import os
from pathlib import Path
import statistics
import sys
from types import SimpleNamespace

from run_benchmarks import PROFILE_WINDOW, validate_record


def summarize(report):
    if not isinstance(report, dict):
        raise ValueError("profile report must be a JSON object")
    environment = report.get("environment")
    if not isinstance(environment, dict) or environment.get("status") != "completed":
        raise ValueError("profile report must be completed")
    if (environment.get("measurement_kind") != "instrumented_diagnostic" or
            environment.get("fair_backend_comparison") is not False or
            environment.get("profile_window") != PROFILE_WINDOW):
        raise ValueError("stage summaries require an explicitly instrumented diagnostic report")
    configuration = environment.get("configuration")
    if not isinstance(configuration, dict) or configuration.get("backends") != "pace":
        raise ValueError("stage summaries require PACE-only diagnostics")
    for field in ("repeats", "clients", "requests", "bytes", "warmup"):
        minimum = 0 if field == "warmup" else 1
        if type(configuration.get(field)) is not int or configuration[field] < minimum:
            raise ValueError(f"invalid diagnostic configuration: {field}")
    runs = report.get("runs")
    if not isinstance(runs, list) or len(runs) != configuration["repeats"]:
        raise ValueError("profile report must contain every requested trial")
    config = SimpleNamespace(**configuration)
    samples = []
    stage_names = None
    for trial, record in enumerate(runs, start=1):
        validate_record(record, "pace", trial, config, instrumented=True)
        stages = {stage["name"]: stage for stage in record["pace_profile"]["stages"]}
        if stage_names is None:
            stage_names = set(stages)
        elif set(stages) != stage_names:
            raise ValueError("profile stage sets differ between trials")
        # Each recorded scope belongs to the one reactor root. This exact integer
        # identity catches missing stages or broken nesting before showing shares.
        if sum(stage["exclusive_ns"] for stage in stages.values()) != stages["reactor_run"]["inclusive_ns"]:
            raise ValueError("exclusive stage totals do not account for the reactor lifetime")
        samples.append(stages)
    rows = []
    for name in sorted(stage_names):
        values = [sample[name] for sample in samples]
        averages = [value["inclusive_ns"] / value["calls"] for value in values if value["calls"]]
        rows.append({
            "stage": name,
            "median_calls": statistics.median(value["calls"] for value in values),
            "median_inclusive_ns": statistics.median(value["inclusive_ns"] for value in values),
            "median_exclusive_ns": statistics.median(value["exclusive_ns"] for value in values),
            "min_exclusive_ns": min(value["exclusive_ns"] for value in values),
            "max_exclusive_ns": max(value["exclusive_ns"] for value in values),
            "median_inclusive_ns_per_call": statistics.median(averages) if averages else None,
            "median_exclusive_share_percent": statistics.median(
                sample[name]["exclusive_ns"] * 100 / sample["reactor_run"]["inclusive_ns"]
                for sample in samples),
        })
    rows.sort(key=lambda row: (-row["median_exclusive_ns"], row["stage"]))
    return {
        "schema_version": 1,
        "measurement_kind": "instrumented_stage_diagnostic",
        "label": environment.get("label", "PACE"),
        "trials": len(samples),
        "profile_window": PROFILE_WINDOW,
        "aggregation": "Per-trial totals, exclusive lifetime shares and inclusive ns/call, then medians across trials",
        "limitations": [
            "Timing scopes add overhead; diagnostic throughput must not be compared with uninstrumented Asio or libuv",
            "Wall-clock accounting includes descheduling and blocking waits; it is not CPU sampling or per-request latency",
            "The profile includes event-loop startup, connection setup, warmup and shutdown; throughput excludes startup and warmup",
            "Listener construction and initial spawn on the main thread precede the worker reset and are excluded",
            "Inclusive stages overlap and must not be added; exclusive shares are of each trial's reactor lifetime",
            "Per-stage medians and median shares need not sum to a median total or 100 percent",
            "Average ns/call excludes trials with zero calls; it is a stage average, not a latency percentile",
        ],
        "stages": rows,
    }


def markdown(summary):
    lines = [
        "# PACE stage timing diagnostic", "",
        "**Instrumentation adds overhead. These timings cannot establish performance against Asio or libuv.**", "",
        f"{summary['trials']} trials. Values below are medians across trials; ranges show exclusive-time spread.", "",
        "| Stage | Calls | Inclusive ms | Exclusive ms | Exclusive ms min-max | Exclusive share | Inclusive ns/call |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summary["stages"]:
        average = row["median_inclusive_ns_per_call"]
        average_text = f"{average:,.1f}" if average is not None else "n/a"
        lines.append(f"| {row['stage']} | {row['median_calls']:,.1f} | "
                     f"{row['median_inclusive_ns'] / 1e6:.3f} | {row['median_exclusive_ns'] / 1e6:.3f} | "
                     f"{row['min_exclusive_ns'] / 1e6:.3f}-{row['max_exclusive_ns'] / 1e6:.3f} | "
                     f"{row['median_exclusive_share_percent']:.2f}% | {average_text} |")
    lines += ["", "`poll_blocking` includes idle waiting. `reactor_run` exclusive time is work outside named child scopes.", ""]
    lines += [f"- {limitation}" for limitation in summary["limitations"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Combined JSON report from run_benchmarks.py --backends pace")
    parser.add_argument("--output", type=Path, help="Markdown destination; a .json stage summary is saved beside it")
    args = parser.parse_args()
    source = args.input.resolve()
    output = args.output.resolve() if args.output else source.with_name(source.stem + "-stages.md")
    if output.suffix.lower() != ".md":
        parser.error("--output must end in .md")
    if source in (output, output.with_suffix(".json")):
        parser.error("summary paths must not overwrite the raw input report")
    try:
        summary = summarize(json.loads(source.read_text(encoding="utf-8")))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    summary["source_report"] = str(source)
    text = markdown(summary)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    output.with_suffix(".json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as destination:
            destination.write("\n" + text)
    print(text)
    print(f"Saved stage summary to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
