"""Run on Linux with two independently built pace_scaling executables."""
import argparse
import json
from pathlib import Path
import random
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("before", type=Path)
parser.add_argument("after", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--perf", default="perf")
parser.add_argument("--server-cpus", default="0-7")
parser.add_argument("--client-cpus", default="8-15")
parser.add_argument("--counts", type=Path, help="compiled epoll_counts.cpp shared library")
parser.add_argument("--profile", action="store_true")
args = parser.parse_args()
args.output.mkdir(exist_ok=True, parents=True)


def run(variant, workers, clients, depth, rep, instrument=None):
    name = f"{variant}-{workers}-{clients}-d{depth}-{rep}"
    total = 1600000 if instrument else 480000
    binary = getattr(args, variant).resolve()
    common = [str(binary), "--pin-threads", "--clients", str(clients), "--workers", str(workers),
              "--requests", str(total // clients), "--warmup", "1000", "--pipeline", str(depth)]
    def command(side, cpus, options):
        prefix = []
        if instrument == "record":
            prefix = [args.perf, "record", "-q", "-F", "499", "-e", "cpu-clock:u", "-g",
                      "-o", str(args.output / f"{name}-{side}.data"), "--"]
        elif instrument == "stat":
            prefix = [args.perf, "stat", "-x,", "-e", "task-clock:u,page-faults:u",
                      "-o", str(args.output / f"{name}-{side}.stat"), "--"]
        elif instrument == "counts" and side == "server":
            prefix = ["env", f"LD_PRELOAD={args.counts.resolve()}"]
        return prefix + ["/usr/bin/time", "-f", "%U %S %w %c", "-o", str(args.output / f"{name}-{side}.time"),
                         "taskset", "-c", cpus] + common + ["--role", side] + options

    with (args.output / f"{name}-server.log").open("w") as log, subprocess.Popen(
        command("server", args.server_cpus, []), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=log, text=True) as server:
        try:
            ports = server.stdout.readline().strip()
            if len(ports.split(",")) != workers or not all(p.isdecimal() for p in ports.split(",")):
                raise RuntimeError(f"invalid server readiness announcement: {ports!r}")
            result = json.loads(subprocess.check_output(command("client", args.client_cpus, ["--ports", ports]),
                                                          text=True, timeout=120))
        finally:
            server.communicate("\n", timeout=15)
        if server.returncode:
            raise RuntimeError(f"server exited with {server.returncode}")
    result.update(variant=variant, repetition=rep, instrument=instrument, server_cpus=args.server_cpus,
                  client_cpus=args.client_cpus)
    for side in ["server", "client"]:
        values = (args.output / f"{name}-{side}.time").read_text().split()
        result[side] = dict(zip(["user_s", "system_s", "voluntary", "involuntary"], map(float, values)))
    if instrument == "counts":
        rows = [line.split()[1:] for line in (args.output / f"{name}-server.log").read_text().splitlines()
                if line.startswith("epoll-counts ")]
        if len(rows) != workers:
            raise RuntimeError(f"expected counts for {workers} workers, got {len(rows)}")
        result["epoll"] = dict(zip(["waits", "events", "single", "multiple"],
                                   (sum(map(int, col)) for col in zip(*rows))))
    print(name, round(result["requests_per_second"]), flush=True)
    return result


with (args.output / ("profiles.jsonl" if args.profile else "trials.jsonl")).open("x") as output:
    if args.profile:
        for instrument in ["record", "stat"] + (["counts"] if args.counts else []):
            for workers, clients in [(4, 8), (8, 8), (4, 32), (8, 32)]:
                print(json.dumps(run("after", workers, clients, 1, instrument, instrument)), file=output, flush=True)
    else:
        for i in range(9):
            cases = [(4, 8), (8, 8), (4, 32), (8, 32)]
            random.Random(i + 347).shuffle(cases)
            for j, (workers, clients) in enumerate(cases):
                variants = ["before", "after"] if (i + j) % 2 else ["after", "before"]
                for variant in variants:
                    print(json.dumps(run(variant, workers, clients, 1, i)), file=output, flush=True)
                if clients == 8:
                    print(json.dumps(run("after", workers, clients, 8, i)), file=output, flush=True)
