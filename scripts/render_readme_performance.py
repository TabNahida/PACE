#!/usr/bin/env python3
"""Render the README's four-metric SVG from validated local benchmark evidence.

Uses only the Python standard library and the existing benchmark validator.
The source report is read-only. Regenerate with:
    python3 scripts/render_readme_performance.py
"""

import argparse
import hashlib
from html import escape
import json
import math
from pathlib import Path
from types import SimpleNamespace

from run_benchmarks import summarize, validate_record


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "benchmarks/results/linux-optimization-2026-10-22/final-c32-b64-session1.json"
BACKENDS = ("pace", "asio", "libuv")
LABELS = {"pace": "PACE", "asio": "Asio", "libuv": "libuv"}
COLORS = {"pace": "#70DFC3", "asio": "#A9AEF7", "libuv": "#8D9DB7"}


def load_evidence(path):
    report = json.loads(path.read_text(encoding="utf-8"))
    env = report["environment"]
    if (env.get("status") != "completed" or env.get("failure") is not None or
            env.get("measurement_kind") != "uninstrumented_benchmark" or
            env.get("fair_backend_comparison") is not True or
            not env.get("platform", "").startswith("Linux-")):
        raise ValueError("a completed, uninstrumented local Linux comparison is required")
    if env["backend_versions"].get("pace_profiling") is not False:
        raise ValueError("profiling evidence cannot be used in this comparison")
    config = SimpleNamespace(**env["configuration"])
    if config.repeats < 2 or len(report["runs"]) != 3 * config.repeats:
        raise ValueError("every trial for all three backends must be present")
    for record in report["runs"]:
        if record.get("backend") not in BACKENDS:
            raise ValueError("unexpected backend")
        validate_record(record, record["backend"], record["trial"], config,
                        require_extended=True, expected_metrics_version=2,
                        expected_memory_source=env["backend_versions"]["peak_process_memory_source"])
    rows = summarize(report["runs"], BACKENDS, config.repeats)
    if rows != report["summary"]:
        raise ValueError("stored summary does not match the raw measurements")
    return env, {row["backend"]: row for row in rows}


def render(source, env, rows):
    config = env["configuration"]
    # Shared latency scale makes p50/p95/p99 comparable. All bars start at zero.
    latency_limit = math.ceil(max(rows[b][f"max_{m}"] for b in BACKENDS
                                  for m in ("p50_us", "p95_us", "p99_us")) / 50) * 50
    throughput_limit = math.ceil(max(rows[b]["max_requests_per_second"] for b in BACKENDS) / 100000) * 100
    metric_specs = [
        ("requests_per_second", "Throughput", "k requests/s", "higher is better", 1000, throughput_limit, 100),
        ("p50_us", "p50 latency", "µs", "lower is better", 1, latency_limit, 50),
        ("p95_us", "p95 latency", "µs", "lower is better", 1, latency_limit, 50),
        ("p99_us", "p99 latency", "µs", "lower is better", 1, latency_limit, 50),
    ]
    metadata = {
        "source": str(source.relative_to(ROOT)) if source.is_relative_to(ROOT) else source.name,
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "aggregation": "Median of trial metrics; whiskers are observed min–max, not confidence intervals",
        "latency_scale": [0, latency_limit],
        "rows": [{"backend": b, **{m: rows[b][f"median_{m}"] for m, *_ in metric_specs}}
                 for b in BACKENDS],
    }
    descriptions = "; ".join(
        f'{LABELS[b]}: {rows[b]["median_requests_per_second"]:,.0f} requests/s, '
        f'p50 {rows[b]["median_p50_us"]:.2f}, p95 {rows[b]["median_p95_us"]:.2f}, '
        f'p99 {rows[b]["median_p99_us"]:.2f} microseconds' for b in BACKENDS)
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="690" viewBox="0 0 1280 690" role="img" aria-labelledby="title description">',
           '<title id="title">PACE, Asio and libuv — local Linux TCP echo comparison</title>',
           f'<desc id="description">{escape(descriptions)}. All axes start at zero. This is one local workload, not an overall ranking.</desc>',
           f'<metadata>{escape(json.dumps(metadata, ensure_ascii=False, sort_keys=True))}</metadata>',
           '<rect x="1" y="1" width="1278" height="688" rx="24" fill="#0C1421" stroke="#263347" stroke-width="2"/>',
           '<g font-family="Inter, Segoe UI, Arial, sans-serif">']

    def text(x, y, content, size=18, color="#A4B2C8", weight=400, anchor="start"):
        svg.append(f'<text x="{x}" y="{y}" fill="{color}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}">{escape(str(content))}</text>')

    text(48, 58, "Measured on Linux", 30, "#F0F4FC", 700)
    text(1232, 56, env["started_utc"][:10], 17, anchor="end")
    text(48, 92, f'TCP echo · {config["clients"]} connections · {config["bytes"]}-byte payloads · {env["server_threads"]} server thread · {config["repeats"]} trials/backend', 19)

    for index, (metric, title, unit, direction, divisor, upper, step) in enumerate(metric_specs):
        x, y = (48 if index % 2 == 0 else 680), (140 if index < 2 else 392)
        baseline, plot_width = x + 84, 374
        text(x, y + 17, title, 24, "#F0F4FC", 650)
        text(x, y + 44, f"{unit} · {direction}", 17)
        for tick in range(0, int(upper) + 1, step):
            tx = baseline + plot_width * tick / upper
            svg.append(f'<path d="M{tx:.2f} {y+64}V{y+188}" stroke="{"#61728C" if tick == 0 else "#243146"}" stroke-width="{1.5 if tick == 0 else 1}"/>')
            text(f"{tx:.2f}", y + 214, tick, 16, "#91A1B9", anchor="middle")
        for position, backend in enumerate(BACKENDS):
            row, cy = rows[backend], y + 82 + position * 46
            value = row[f"median_{metric}"] / divisor
            low, high = row[f"min_{metric}"] / divisor, row[f"max_{metric}"] / divisor
            width = value / upper * plot_width
            text(x, cy + 6, LABELS[backend], 20, COLORS[backend], 600)
            svg.append(f'<rect x="{baseline}" y="{cy-13}" width="{width:.3f}" height="26" rx="3" fill="{COLORS[backend]}"/>')
            lx, hx = baseline + low / upper * plot_width, baseline + high / upper * plot_width
            svg.append(f'<path d="M{lx:.3f} {cy}H{hx:.3f}M{lx:.3f} {cy-5}V{cy+5}M{hx:.3f} {cy-5}V{cy+5}" fill="none" stroke="#F8FAFE" stroke-width="1.5"/>')
            text(x + 552, cy + 7, f"{value:.1f}" if divisor == 1000 else f"{value:.2f}", 22, "#F0F4FC", 600, "end")
    svg.append('<path d="M48 630H1232" stroke="#263347"/>')
    text(48, 655, "Bars: medians · whiskers: trial min–max, not confidence intervals · latency panels share one scale", 16)
    versions = env["backend_versions"]
    asio = versions["asio_version"]
    platform = "Linux / WSL2" if "WSL2" in env["platform"] else "Linux"
    text(48, 678, f'{platform} · {versions["compiler"]} · Asio {asio // 100000}.{asio // 100 % 1000}.{asio % 100} · libuv {versions["libuv_version"]} · closed-loop localhost workload', 15, "#91A1B9")
    svg.extend(['</g>', '</svg>'])
    return "\n".join(svg) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=SOURCE)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/assets/readme-performance.svg")
    args = parser.parse_args()
    source, output = args.evidence.resolve(), args.output.resolve()
    if output == source or output.suffix.lower() != ".svg":
        parser.error("choose an SVG output distinct from the evidence")
    try:
        env, rows = load_evidence(source)
        result = render(source, env, rows)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(result, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
