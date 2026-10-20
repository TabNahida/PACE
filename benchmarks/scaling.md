# Worker and execution scaling

`pace_scaling` measures PACE TCP echo with an explicit number of independent
reactors. It compares execution choices and fixed-client worker scaling. It is
separate from the single-server-thread [Asio/libuv comparison](README.md) and the
[component microbenchmarks](components.md).

| Mode | N active reactors | Calling thread |
|---|---|---|
| `runtime` | N threads started by `runtime::run()` | Waits for those threads to finish |
| `caller` | N−1 spawned threads plus the caller in `runtime::run_on_current_thread()` | Runs context 0; N=1 starts no reactor worker thread |
| `external` | N application-owned `io_context` objects, each on an application-created thread | Joins those threads |

Every mode also uses one coordinator thread and one blocking thread per client.
The caller mode changes who runs a reactor; it still has N active reactor
threads. Worker counts are explicit and may be below, equal to, or above the
hardware thread count. CPU affinity is not set.

## Workload and timing boundaries

Each worker listens on its own ephemeral IPv4 loopback port. Client `i` connects
to worker `i % N`; the fixed total client count must be divisible by every chosen
worker count. Connections persist through warmup and measurement, use
`TCP_NODELAY`, and exchange one request/response at a time (pipeline=1). All
clients use the same blocking socket implementation, independent of execution
mode. Each response is checked byte for byte against a payload containing a
changing request sequence and connection tag. The executable and runner verify
the total request count, balanced per-worker completion counts and a checksum
derived from returned sequences. Failed or incomplete runs produce no successful
measurement record.

| Field | Exactly what is measured |
|---|---|
| `setup_ns` | Context and listener construction, port binding, accept-task submission and initial readiness-marker submission. |
| `launch_ns` | Entry to the selected execution path until every reactor dispatches its initial posted marker. Listeners are already bound; this does not include client connection setup. |
| `startup_ns` | Sum of setup and launch. Coordinator creation between those two intervals is excluded, so this is not a continuous process-start timestamp. |
| `client_setup_warmup_ns` | After all reactors report ready: creating client threads, connecting, warming every connection and reaching the common start gate. |
| `steady_ns` | Just before releasing all clients from the common gate until all client threads join. Includes response verification, checksum/sampling work, client scheduling, socket close and join overhead; excludes server startup and client warmup. |
| `shutdown_ns` | Immediately before calling stop until the selected execution call returns after joining reactor threads. Context/listener destruction and the final coordinator join are excluded. |

Throughput is completed measured requests divided by `steady_ns`. Round-trip
MiB/s counts application payload in both directions, excluding TCP/IP overhead.
p50/p99 use nearest-rank percentiles of per-request wall times across all clients
within one trial. Reports take the median of each trial's percentile, not a
percentile over pooled trials. Startup/launch and shutdown are single observations
per process; inspect repeated-process spread.

## Reproduce

Build without profiling or sanitizers:

```sh
xmake f -m release --scaling=y --profiling=n --sanitizers=n -y
xmake pace_scaling
python3 scripts/run_scaling.py --workers 1,2,4,8 --clients 8 \
  --modes runtime,caller,external --requests 2000 --warmup 200 --repeats 3 \
  --output benchmark-results/scaling.json
```

The default matrix is 12 mode/worker cells and three trials per cell. The runner
rotates cell order between trials, launches a fresh process per cell, applies a
60-second whole-process timeout, and retains every command and validated raw
record in JSON. A Markdown summary reports medians and throughput ranges;
JSON also retains all timing ranges. Failed runs retain an explicit failure
record and remove any stale success summary. Use `--timeout` for slower machines
and `--timeout-ms` for socket send/receive bounds. The process timeout also bounds
startup and connection failure paths.

Use `--build-dir` to restrict automatic discovery, or `--executable` to select a
specific binary. On Windows, use `python` and put each command on one line.
`--list` exposes modes, compiler, C++ standard, release/debug marker and profiling
status. Both binary and runner reject instrumented performance measurements.
Reports include OS/CPU metadata, logical CPU count, binary SHA-256, tracked Git
state and source hashes at invocation. Keep the actual compiler command beside
published evidence: current source hashes alone cannot identify the source used
to build a supplied executable.

For a small correctness smoke after building (not performance evidence):

```sh
python3 scripts/run_scaling.py --workers 1,2 --clients 2 --requests 20 \
  --warmup 2 --repeats 1 --output benchmark-results/scaling-smoke.json
```

To explore oversubscription on a machine reporting 24 logical CPUs, keep the
total load fixed at 48 clients and choose the worker counts explicitly:

```sh
python3 scripts/run_scaling.py --workers 1,2,4,8,24,48 --clients 48 \
  --requests 2000 --warmup 200 --repeats 3 \
  --output benchmark-results/scaling-48-clients.json
```

This is a different workload from the eight-client matrix. Compare worker counts
within a fixed-client study; do not interpret the difference between those two
matrices as an isolated worker-count effect.

## Interpretation

Clients and server compete for CPU on the same host. Increasing workers changes
connections per reactor while total connection and request counts remain fixed.
Results therefore show this complete loopback workload, including client and
kernel costs; they do not establish linear scaling or isolated server capacity.
Per-worker ports provide deterministic balancing and do not test shared-listener
accept distribution or `SO_REUSEPORT` behavior. HTTP, remote networks, pipelining,
large connection populations and mixed CPU work require separate workloads.

Run no other builds or benchmarks during measurements. Repeat on deployment
hardware, inspect spread, and use sufficiently long measured phases before
making a performance claim. No speed threshold changes the runner's exit code:
correct, complete, uninstrumented measurements are required; faster execution is
an observation to assess against its workload and uncertainty.
