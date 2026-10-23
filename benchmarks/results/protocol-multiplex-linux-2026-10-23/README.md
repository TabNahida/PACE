# HTTP/2 and HTTP/3 handler concurrency, local Linux

This experiment isolates application head-of-line blocking on one connection.
The same updated release binary is run with one or eight handler slots. Every
batch contains 16 requests; every handler awaits a 10 ms PACE timer and produces
1 KiB of validated response data. This models independent asynchronous work;
it does not measure CPU parallelism or compare PACE with another HTTP library.
The cap-one case is not a build of v0.1.1.

Ten fresh-connection batches were measured for each protocol and cap. Connection
setup and TLS/QUIC handshakes are excluded; HTTP SETTINGS and exchange teardown
remain inside the timed exchange. Both peers use one context on one thread.
TCP_NODELAY is enabled on both TCP/TLS peers. All 60 samples passed complete
response body and status checks. Other local builds/tests were paused during
measurement.

| Protocol | 1 handler median | 8 handlers median | Ratio |
|---|---:|---:|---:|
| HTTP/2 over TCP | 162.411 ms | 20.322 ms | 7.99× |
| HTTP/2 over TLS | 162.775 ms | 20.576 ms | 7.91× |
| HTTP/3 over QUIC | 163.877 ms | 20.923 ms | 7.83× |

The expected timer floor changes from roughly 160 ms to 20 ms. These numbers
show that independent handler waits overlap; they are not general throughput
multipliers. No claim is made about CPU-bound handlers, production latency,
other reactor/thread layouts, or Asio/libuv HTTP performance.

[All samples](samples.csv), [summary](summary.json), and
[environment, exact commands and SHA-256 provenance](metadata.json) are retained.
The metadata identifies an uncommitted working tree by per-source hashes as well
as its base Git revision. Dependencies are OpenSSL 3.5.7, nghttp2 1.70.0 and
nghttp3 1.18.0, pinned by the recorded xmake files. The release build command was:

```sh
XMAKE_GLOBALDIR=/tmp/pace-xmake XMAKE_CONFIGDIR=/tmp/pace-protocol-perf-config \
  xmake f -y --http1=n --websocket=n --http2=y --tls=y --http3=y \
  --protocol_benchmarks=y -m release -o /tmp/pace-protocol-perf
XMAKE_GLOBALDIR=/tmp/pace-xmake XMAKE_CONFIGDIR=/tmp/pace-protocol-perf-config \
  xmake b -j 4 pace_protocol_multiplex
python3 scripts/run_protocol_multiplex.py \
  /tmp/pace-protocol-perf/linux/x86_64/release/pace_protocol_multiplex \
  --output benchmarks/results/protocol-multiplex-linux-2026-10-23 --batches 10
```

The runner enforces Linux-only measurement. Remote CI may run the probe to check
correctness, but its times are not used in these results or performance rankings.
