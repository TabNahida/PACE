#!/usr/bin/env python3
"""Local Linux, rotating PACE epoll/io_uring vs nginx HTTP workloads; raw validated trials."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import socket
import statistics
import subprocess
import tempfile
import time


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_listening(process, number):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        if process.poll() is not None:
            raise RuntimeError(f"server exited: {process.args}")
        try:
            with socket.create_connection(("127.0.0.1", number), timeout=.1):
                return
        except OSError:
            time.sleep(.02)
    raise TimeoutError("server startup")


def resources(pid):
    pending, visited = [pid], set()
    cpu, memory = 0, 0
    while pending:
        child = pending.pop()
        if child in visited:
            continue
        visited.add(child)
        root = Path(f"/proc/{child}")
        try:
            pending.extend(map(int, (root / "task" / str(child) / "children").read_text().split()))
            fields = (root / "stat").read_text().rsplit(") ", 1)[1].split()
            cpu += int(fields[11]) + int(fields[12])
            status = (root / "status").read_text().splitlines()
            memory += next(int(line.split()[1]) * 1024 for line in status if line.startswith("VmHWM:"))
        except (OSError, StopIteration):
            pass
    return {"cpu_seconds": cpu / os.sysconf("SC_CLK_TCK"), "sum_process_high_water_rss_bytes": memory}


def host_snapshot():
    ticks = list(map(int, Path("/proc/stat").read_text().splitlines()[0].split()[1:9]))
    return {"load_average": list(os.getloadavg()), "total_ticks": sum(ticks), "idle_ticks": ticks[3] + ticks[4]}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def nginx_config(directory, listen, workers, scenario, upstreams):
    cache = scenario.endswith("cached")
    proxy = scenario.startswith("proxy")
    head = f'''daemon off;
master_process on;
pid {directory}/nginx.pid;
error_log {directory}/error.log warn;
worker_processes {workers};
events {{ worker_connections 8192; }}
http {{
    access_log off;
    default_type application/octet-stream;
    sendfile on;
    tcp_nodelay on;
    keepalive_timeout 60;
    keepalive_requests 10000000;
    client_body_temp_path {directory}/body;
    proxy_temp_path {directory}/proxy_temp;
'''
    if proxy:
        head += "    upstream origin {\n" + "".join(f"        server 127.0.0.1:{p} max_fails=1 fail_timeout=1s;\n" for p in upstreams) + "        keepalive 256;\n    }\n"
        if cache:
            head += f"    proxy_cache_path {directory}/cache levels=1:2 keys_zone=responses:8m max_size=64m inactive=60s;\n"
        location = '''proxy_pass http://origin;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_next_upstream off;
            proxy_buffering on;
            proxy_buffers 8 64k;
            proxy_busy_buffers_size 128k;
            proxy_max_temp_file_size 0;
            proxy_connect_timeout 30s;
            proxy_read_timeout 30s;'''
        if cache:
            location += '\n            proxy_cache responses;\n            proxy_cache_valid 200 60s;'
    else:
        location = f"root {directory}/public;"
        if cache:
            location += "\n            open_file_cache max=1024 inactive=60s;\n            open_file_cache_valid 60s;"
    return head + f"    server {{ listen 127.0.0.1:{listen} reuseport; location / {{ {location} }} }}\n}}\n"


def run_trial(args, backend, scenario, workers):
    processes, logs = [], []
    def start(command, directory, env=None):
        log = open(directory / f"process-{len(logs)}.log", "w+")
        logs.append(log)
        p = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        processes.append(p)
        return p
    with tempfile.TemporaryDirectory(prefix="pace-gateway-bench-") as temporary:
        directory = Path(temporary)
        (directory / "public").mkdir()
        (directory / "public" / "data.bin").write_bytes(b"x" * args.bytes)
        try:
            origins = []
            if scenario.startswith("proxy"):
                for _ in range(2):
                    upstream = port()
                    p = start([args.pace, "origin", str(upstream), "2", "-", "0", str(args.bytes)], directory,
                              dict(os.environ, PACE_IO_BACKEND="epoll"))
                    wait_listening(p, upstream)
                    origins.append(upstream)
            listen = port()
            if backend == "nginx":
                conf = directory / "nginx.conf"
                conf.write_text(nginx_config(directory, listen, workers, scenario, origins))
                p = start([args.nginx, "-p", str(directory), "-c", str(conf)], directory)
                config = conf.read_text()
            else:
                mode = "proxy" if origins else "static"
                resource = ",".join(f"http://127.0.0.1:{n}" for n in origins) if origins else str(directory / "public")
                capacity = 64 * 1024 * 1024 if scenario.endswith("cached") else 0
                p = start([args.baseline if backend == "baseline" else args.pace, mode, str(listen), str(workers), resource, str(capacity), str(args.bytes)], directory,
                          dict(os.environ, PACE_IO_BACKEND="epoll" if backend == "baseline" else backend))
                config = {"mode": mode, "workers": workers, "cache_bytes": capacity, "upstreams": 2 if origins else 0}
            wait_listening(p, listen)
            load = subprocess.Popen([args.load, str(listen), str(args.connections), str(args.iterations), str(args.warmup), str(args.bytes), "--barrier"],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            # Bound the entire client lifetime, including the warmup/readiness barrier.
            import selectors
            with selectors.DefaultSelector() as selector:
                selector.register(load.stdout, selectors.EVENT_READ)
                if not selector.select(args.timeout):
                    load.kill(); load.communicate(); raise TimeoutError("warmup")
            if load.stdout.readline().strip() != "READY":
                _, error = load.communicate(timeout=1)
                raise RuntimeError(f"warmup failed: {error}")
            host_before = host_snapshot()
            before = resources(p.pid)
            try:
                output, error = load.communicate("G", timeout=args.timeout)
            except subprocess.TimeoutExpired:
                load.kill(); load.communicate(); raise
            after = resources(p.pid)
            host_after = host_snapshot()
            if load.returncode:
                raise RuntimeError(f"load failed: {error}")
            record = json.loads(output)
            if record.get("validated") is not True or record["completed_requests"] != args.connections * args.iterations:
                raise ValueError("unvalidated/incomplete trial")
            if backend != "nginx":
                logs[-1].seek(0)
                if logs[-1].readline().strip() != ("epoll" if backend == "baseline" else backend):
                    raise ValueError("requested backend did not execute")
            record.update(host_before=host_before, host_after=host_after, backend=backend, scenario=scenario, workers=workers, server_config=config,
                          server_cpu_seconds=after["cpu_seconds"] - before["cpu_seconds"],
                          server_process_high_water_rss_bytes=after["sum_process_high_water_rss_bytes"])
            return record
        except Exception:
            for log in logs:
                log.seek(0)
                print(log.read()[-4000:], flush=True)
            raise
        finally:
            for p in reversed(processes):
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGTERM)
                    try: p.wait(timeout=3)
                    except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL); p.wait()
            for log in logs:
                log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("pace", "nginx", "load"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--baseline", help="Optional alternate PACE binary; runs as baseline with epoll")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--connections", type=int, default=32)
    parser.add_argument("--iterations", type=int, default=5000)
    parser.add_argument("--warmup", type=int, default=200)
    parser.add_argument("--bytes", type=int, default=4096)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--workers", default="1,4")
    parser.add_argument("--backends", default="epoll,io_uring,nginx")
    parser.add_argument("--scenarios", default="static,static_cached,proxy,proxy_cached")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if platform.system() != "Linux": parser.error("performance measurements are Linux-only")
    if not (1 <= args.connections <= 4096 and 1 <= args.iterations <= 1000000 and 0 <= args.warmup <= 100000 and
            1 <= args.bytes <= 16*1024*1024 and 1 <= args.repeats <= 100 and args.timeout > 0): parser.error("invalid workload")
    backends = args.backends.split(","); workers = list(map(int, args.workers.split(","))); scenarios = args.scenarios.split(",")
    if not set(backends) <= {"epoll", "io_uring", "nginx", "baseline"} or not set(scenarios) <= {"static", "static_cached", "proxy", "proxy_cached"} or not all(1 <= n <= 64 for n in workers): parser.error("invalid cases")
    if "baseline" in backends and not args.baseline: parser.error("baseline backend requires --baseline")
    if args.baseline: args.baseline = str(Path(args.baseline).resolve(strict=True))
    for name in ("pace", "nginx", "load"):
        setattr(args, name, str(Path(getattr(args, name)).resolve(strict=True)))
    result = {"schema": 1, "environment": {"platform": platform.platform(), "cpu": platform.processor(),
               "cpuinfo": Path("/proc/cpuinfo").read_text(), "nginx": subprocess.check_output([args.nginx, "-V"], stderr=subprocess.STDOUT, text=True),
               "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
               "compiler": subprocess.check_output(["c++", "--version"], text=True),
               "source_sha256": {str(p): digest(p) for root in ("src", "include", "benchmarks", "xmake") for p in sorted(Path(root).rglob("*")) if p.suffix in (".cpp", ".hpp", ".lua")},
               "runner_sha256": digest(__file__),
               "binary_sha256": {name: digest(getattr(args, name)) for name in ("pace", "nginx", "load", "baseline") if getattr(args, name)}},
              "workload": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}, "trials": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for scenario in scenarios:
        for worker_count in workers:
            for repeat in range(args.repeats):
                order = backends[repeat % len(backends):] + backends[:repeat % len(backends)]
                for backend in order:
                    record = run_trial(args, backend, scenario, worker_count)
                    record["trial"] = repeat + 1
                    result["trials"].append(record)
                    args.output.write_text(json.dumps(result, indent=2) + "\n")
                    print(f'{scenario} workers={worker_count} {backend} trial={repeat+1}: {record["requests_per_second"]:.0f} req/s, p99={record["p99_us"]:.1f} us', flush=True)
    summaries = []
    for scenario in scenarios:
        for worker_count in workers:
            for backend in backends:
                rows = [r for r in result["trials"] if (r["scenario"], r["workers"], r["backend"]) == (scenario, worker_count, backend)]
                summaries.append({"scenario": scenario, "workers": worker_count, "backend": backend,
                                  **{metric: {"median": statistics.median(r[metric] for r in rows), "min": min(r[metric] for r in rows), "max": max(r[metric] for r in rows)}
                                     for metric in ("requests_per_second", "p50_us", "p95_us", "p99_us", "server_cpu_seconds", "server_process_high_water_rss_bytes")}})
    result["summaries"] = summaries
    args.output.write_text(json.dumps(result, indent=2) + "\n")

if __name__ == "__main__":
    main()
