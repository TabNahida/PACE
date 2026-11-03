# Local Linux evidence for 0.5.0

[Full results and interpretation](../../../docs/performance.md).

The production sources were frozen as `6e1be7350027236f81e4e3d3f70b2bb17503e664`.
The TCP run began before that commit was created, from the identical source tree;
`source-manifest.json` records the source fingerprints. Subsequent release commits
change presentation/evidence only. `a343fa6` is the previous optimized gateway
baseline (binary SHA256 `44e1c28ec7ec97d5958f0780742bd01b330048cb0e3bdd9ee76e486a14560204`).

- `tcp/report.json`: 10 rotating trials per backend, complete environment and summaries; `raw.jsonl` retains each trial.
- `gateway.json`: 5 rotating trials per backend / scenario / worker count, including all latency, CPU, memory, load and configuration data.
- `proxy-cache-confirmation.json`: longer seven-pair cached-proxy follow-up, 20,000 requests per connection, both worker counts. The original matrix remains the headline.
- `proxy-allocations.json`: intrusive allocation-call census. Single frontend reactor thread only, startup and warmup included. Do not use it for throughput comparisons.
- `proxy-screen1.json`, `proxy-screen2.json`: preliminary uncached proxy screens. Corresponding patches apply to `a343fa6`; their source hashes were checked against each report. They are exploratory intermediate implementations, not the release result.

No performance figures come from remote CI. No trial is removed from a completed
session. The README uses the default epoll backend, not whichever backend wins.
All perf comparisons use uninstrumented release binaries; the allocator preload
is applied only by the separate diagnostic script.

## Reproduce

Build a separate baseline checkout at `a343fa6` with the same compiler/options
and save its `pace_http_gateway` binary. Build nginx 1.30.6 from the official
source with `--without-http_rewrite_module --without-http_gzip_module --with-threads`.
Use explicit paths for your resulting binaries below:

```sh
xmake f -c -m release -o build/perf --uring=y --protocol_benchmarks=y --benchmarks=y --tests=n --examples=n -y
xmake -y
python3 scripts/run_benchmarks.py \
  --executable build/perf/linux/x86_64/release/pace_bench --require-all \
  --clients 32 --requests 60000 --warmup 5000 --bytes 64 --repeats 10 \
  --label local-linux-0.5.0 --output /tmp/pace-tcp-rerun
python3 scripts/run_gateway_benchmarks.py \
  --pace build/perf/linux/x86_64/release/pace_http_gateway \
  --baseline /path/to/a343fa6/pace_http_gateway --nginx /path/to/nginx \
  --load build/perf/linux/x86_64/release/pace_http_load \
  --workers 1,4 --backends baseline,epoll,io_uring,nginx \
  --iterations 10000 --repeats 5 --output /tmp/pace-gateway-rerun.json
```

The two origins always use the final binary, identically for baseline, epoll,
io_uring and nginx frontends. Run comparisons sequentially, without simultaneous
builds/tests. These are bounded loopback workload results, not universal rankings.
The TCP executable uses 32 blocking client threads; the HTTP generator uses one
epoll thread. Do not compare their absolute throughput or memory to each other.

Optional Linux/glibc allocation diagnostic (never preload normal comparisons):

```sh
cc -O2 -shared -fPIC benchmarks/diagnostics/allocation_probe.c -o /tmp/pace-alloc.so
python3 scripts/probe_proxy_allocations.py \
  --pace build/perf/linux/x86_64/release/pace_http_gateway \
  --baseline /path/to/a343fa6/pace_http_gateway \
  --load build/perf/linux/x86_64/release/pace_http_load \
  --preload /tmp/pace-alloc.so --output /tmp/pace-allocations.json
```

The probe is a Linux/glibc diagnostic aid, not a portable allocator or production
library. The fixed 352,000-exchange census keeps its signal-safe call counters
within range. It counts allocation API calls, not live allocations or bytes.

Regenerate the figures without editing data:

```sh
python3 scripts/render_readme_performance.py
python3 scripts/plot_gateway_optimization.py \
  benchmarks/results/linux-release-0.5.0/gateway.json \
  docs/assets/gateway-0.5.0.svg --all-backends
```

The gateway plot requires matplotlib; the TCP SVG renderer uses Python's standard
library and validates every record before rendering. Whiskers are trial min–max,
not confidence intervals. All latency panels in the TCP chart share one zero-based
scale.
