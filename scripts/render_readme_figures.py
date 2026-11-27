#!/usr/bin/env python3
"""Render the README's banner, headline numbers and comparison chart as SVG, from raw benchmark results.

Nothing in the figures is typed by hand: every number is read from one result directory written by
scripts/run_baseline.py, and every figure carries that run's version, date and host. Each figure is written in a
light and a dark version for the README's <picture> elements.

    python3 scripts/render_readme_figures.py            # regenerate docs/assets/readme/
    python3 scripts/render_readme_figures.py --check    # fail if the files are missing or out of date

To move the README to a new measurement, run scripts/run_baseline.py, point RESULTS below at the new directory
and regenerate. The README text that quotes these numbers must change with them; --check also verifies that the
README names the version shown in the figures.
"""

import argparse
import json
import statistics
import sys
from datetime import datetime
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "benchmarks/results/baseline-0.6.1-macos-arm64"
OUTPUT = ROOT / "docs/assets/readme"
READMES = (ROOT / "README.md", ROOT / "README.zh.md")

FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace"

THEMES = {
    "light": {
        "text": "#1f2328", "muted": "#59636e", "faint": "#818b98", "card": "#ffffff", "border": "#d1d9e0",
        "accent": "#0f766e", "accent_soft": "#ccfbf1", "bar": "#0f766e", "other": "#b7c0ca", "grid": "#eef1f4",
        "banner_top": "#f0fdfa", "banner_bottom": "#ffffff", "layer": ["#0f766e", "#14897f", "#2aa396", "#5cbdb0"],
        "layer_text": "#ffffff",
    },
    "dark": {
        "text": "#e6edf3", "muted": "#9198a1", "faint": "#6e7681", "card": "#151b23", "border": "#3d444d",
        "accent": "#2dd4bf", "accent_soft": "#0d3a36", "bar": "#2dd4bf", "other": "#4a535d", "grid": "#212830",
        "banner_top": "#0b2a28", "banner_bottom": "#0d1117", "layer": ["#115e59", "#0f766e", "#14897f", "#1aa395"],
        "layer_text": "#e6edf3",
    },
}


# -- data ---------------------------------------------------------------------------------------------------------

def load_jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def median_of(records, workload, key):
    values = [row["record"][key] for row in records if row["workload"] == workload]
    if not values:
        sys.exit(f"{workload!r} / {key} missing from the results")
    return statistics.median(values)


def measurements():
    manifest = json.loads((RESULTS / "manifest.json").read_text())
    measured = datetime.fromisoformat(manifest["measured_at"])
    version = manifest["source"]["describe"].lstrip("v")
    host = manifest["label"].split(",")[0]
    systems = ("macOS", "Linux", "Ubuntu", "Debian", "Fedora", "FreeBSD", "Windows")
    parts = [part.strip() for part in manifest["label"].split(",")]
    system = next((part for part in parts if part.startswith(systems)), manifest["host"]["platform"])

    components = json.loads((RESULTS / "components.json").read_text())
    yields = [run["ns_per_operation"] for run in components["runs"] if run["component"] == "yield"]

    tcp = {row["backend"]: row for row in json.loads((RESULTS / "tcp-echo/summary.json").read_text())}
    tcp_configuration = load_jsonl(RESULTS / "tcp-echo/raw.jsonl")[0]

    websocket = load_jsonl(RESULTS / "raw/websocket.jsonl")
    return {
        "version": version,
        "date": measured.strftime("%Y-%m-%d"),
        "host": host,
        "system": system,
        "trials": manifest["repeats"],
        "yield_ns": statistics.median(yields),
        "tcp": {name: {"rps": tcp[name]["median_requests_per_second"], "p50": tcp[name]["median_p50_us"],
                       "p99": tcp[name]["median_p99_us"]} for name in ("pace", "asio", "libuv")},
        "tcp_connections": tcp_configuration["connections"],
        "tcp_payload": tcp_configuration["payload_bytes"],
        "tcp_server_threads": tcp_configuration["server_threads"],
        "ws_rps": median_of(websocket, "64 B", "roundtrips_per_second"),
        "ws_p50": median_of(websocket, "64 B", "p50_us"),
    }


def provenance(data):
    return (f"PACE {data['version']} · {data['host']} · {data['system']} · {data['date']} · "
            f"median of {data['trials']} trials")


def thousands(value):
    return f"{value / 1000:.1f}k" if value < 100_000 else f"{value / 1000:.0f}k"


# -- svg helpers --------------------------------------------------------------------------------------------------

def svg(width, height, title, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" role="img" aria-label="{escape(title)}">\n'
            f"<title>{escape(title)}</title>\n{body}</svg>\n")


def text(x, y, content, size, color, weight=400, anchor="start", family=FONT, extra=""):
    return (f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" '
            f'fill="{color}" text-anchor="{anchor}"{extra}>{escape(content)}</text>\n')


# -- figures ------------------------------------------------------------------------------------------------------

def banner(theme):
    t = THEMES[theme]
    width, height = 1280, 300
    body = [
        f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0" stop-color="{t["banner_top"]}"/><stop offset="1" stop-color="{t["banner_bottom"]}"/>'
        f"</linearGradient></defs>\n",
        f'<rect width="{width}" height="{height}" rx="16" fill="url(#bg)"/>\n',
        f'<rect x="0.5" y="0.5" width="{width - 1}" height="{height - 1}" rx="16" fill="none" '
        f'stroke="{t["border"]}"/>\n',
        text(72, 138, "PACE", 92, t["text"], 800, extra=' letter-spacing="6"'),
        text(76, 186, "C++20 coroutine networking", 30, t["accent"], 600),
        text(76, 228, "From sockets to HTTP/3 — on one thread, a worker pool, or threads you own.", 20,
             t["muted"]),
    ]
    layers = [
        ("HTTP/1.1 · HTTP/2 · HTTP/3 · WebSocket", 3),
        ("TLS · DNS · QUIC", 2),
        ("TCP · UDP", 1),
        ("Runtime · tasks · timers · reactors", 0),
    ]
    x, y, w, h, gap = 760, 52, 448, 44, 10
    for index, (label, level) in enumerate(layers):
        top = y + index * (h + gap)
        body.append(f'<rect x="{x}" y="{top}" width="{w}" height="{h}" rx="10" fill="{t["layer"][level]}"/>\n')
        body.append(text(x + 20, top + 28, label, 17, t["layer_text"], 600))
        body.append(text(x + w - 18, top + 28, f"L{level}", 14, t["layer_text"], 500, "end", MONO,
                         ' opacity="0.75"'))
    return svg(width, height, "PACE — C++20 coroutine networking", "".join(body))


def cards(theme, data):
    t = THEMES[theme]
    width, height = 1280, 236
    entries = [
        ("Coroutine switch", f"{data['yield_ns']:.1f}", "ns", "yield and resume on one thread"),
        ("TCP echo", thousands(data["tcp"]["pace"]["rps"]), "req/s",
         f"{data['tcp_server_threads']} server thread · {data['tcp_connections']} connections · "
         f"{data['tcp_payload']} B"),
        ("WebSocket", thousands(data["ws_rps"]), "round trips/s", f"64 B messages · p50 {data['ws_p50']:.1f} µs"),
    ]
    gap, card_width, card_height = 24, (width - 2 * 24) / 3, 196
    body = []
    for index, (label, value, unit, detail) in enumerate(entries):
        x = index * (card_width + gap)
        body.append(f'<rect x="{x + 0.5:.1f}" y="0.5" width="{card_width - 1:.1f}" height="{card_height - 1}" '
                    f'rx="14" fill="{t["card"]}" stroke="{t["border"]}"/>\n')
        body.append(f'<rect x="{x + 28:.1f}" y="30" width="36" height="4" rx="2" fill="{t["accent"]}"/>\n')
        body.append(text(f"{x + 28:.1f}", 64, label.upper(), 15, t["muted"], 600, extra=' letter-spacing="1.5"'))
        body.append(f'<text x="{x + 28:.1f}" y="128" font-family="{FONT}" fill="{t["text"]}">'
                    f'<tspan font-size="58" font-weight="700">{escape(value)}</tspan>'
                    f'<tspan font-size="22" font-weight="500" fill="{t["muted"]}" dx="10">{escape(unit)}</tspan>'
                    f"</text>\n")
        body.append(text(f"{x + 28:.1f}", 166, detail, 16, t["muted"]))
    body.append(text(width / 2, height - 10, provenance(data), 14, t["faint"], anchor="middle"))
    title = f"PACE {data['version']}: " + "; ".join(f"{label} {value} {unit}"
                                                     for label, value, unit, _ in entries)
    return svg(width, height, title, "".join(body))


def tcp_chart(theme, data):
    t = THEMES[theme]
    width, height = 1280, 330
    rows = [("PACE", data["tcp"]["pace"], True), ("Asio", data["tcp"]["asio"], False),
            ("libuv", data["tcp"]["libuv"], False)]
    peak = max(row["rps"] for _, row, _ in rows)
    left, bar_left, bar_width, top, row_height = 40, 150, 760, 96, 52
    body = [
        f'<rect x="0.5" y="0.5" width="{width - 1}" height="{height - 1}" rx="14" fill="{t["card"]}" '
        f'stroke="{t["border"]}"/>\n',
        text(left, 50, "TCP echo throughput and latency", 24, t["text"], 700),
        text(left, 78, f"{data['tcp_server_threads']} server thread · {data['tcp_connections']} persistent "
                       f"connections · {data['tcp_payload']}-byte messages · one request in flight per connection",
             16, t["muted"]),
        text(1010, 78, "p50", 15, t["muted"], 600, "end"),
        text(1110, 78, "p99", 15, t["muted"], 600, "end"),
        text(1238, 78, "req/s", 15, t["muted"], 600, "end"),
    ]
    for step in range(1, 5):
        x = bar_left + bar_width * step / 4
        body.append(f'<line x1="{x:.1f}" y1="{top - 8}" x2="{x:.1f}" y2="{top + row_height * 3 - 12}" '
                    f'stroke="{t["grid"]}" stroke-width="1"/>\n')
    for index, (name, row, highlight) in enumerate(rows):
        y = top + index * row_height
        length = bar_width * row["rps"] / peak
        color = t["bar"] if highlight else t["other"]
        body.append(text(left, y + 25, name, 19, t["text"], 700 if highlight else 500))
        body.append(f'<rect x="{bar_left}" y="{y + 6}" width="{length:.1f}" height="28" rx="6" fill="{color}"/>\n')
        body.append(text(1010, y + 25, f"{row['p50']:.1f} µs", 17, t["text"], 500, "end", MONO))
        body.append(text(1110, y + 25, f"{row['p99']:.1f} µs", 17, t["text"], 500, "end", MONO))
        body.append(text(1238, y + 25, f"{row['rps']:,.0f}", 17, t["text"], 700 if highlight else 500, "end",
                         MONO))
    body.append(text(left, height - 28, provenance(data) + " · identical blocking clients for every library", 14,
                     t["faint"]))
    summary = "; ".join(f"{name} {row['rps']:,.0f} req/s, p50 {row['p50']:.1f} µs, p99 {row['p99']:.1f} µs"
                        for name, row, _ in rows)
    return svg(width, height, f"TCP echo, PACE {data['version']}: {summary}", "".join(body))


def figures():
    data = measurements()
    result = {}
    for theme in THEMES:
        result[f"banner-{theme}.svg"] = banner(theme)
        result[f"numbers-{theme}.svg"] = cards(theme, data)
        result[f"tcp-echo-{theme}.svg"] = tcp_chart(theme, data)
    return data, result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify instead of writing")
    arguments = parser.parse_args()
    data, files = figures()
    if arguments.check:
        problems = []
        for name, content in files.items():
            path = OUTPUT / name
            if not path.exists():
                problems.append(f"{path.relative_to(ROOT)} is missing")
            elif path.read_text() != content:
                problems.append(f"{path.relative_to(ROOT)} is out of date")
        for readme in READMES:
            if f"PACE {data['version']}" not in readme.read_text():
                problems.append(f"{readme.name} does not name PACE {data['version']}, the version in the figures")
        for problem in problems:
            print(f"error: {problem}")
        print(f"{len(files)} figures from {RESULTS.relative_to(ROOT)}: {'ok' if not problems else 'stale'}")
        return 1 if problems else 0
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        (OUTPUT / name).write_text(content)
    print(f"wrote {len(files)} figures to {OUTPUT.relative_to(ROOT)} from {RESULTS.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
