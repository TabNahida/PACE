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
| HTTP/2 over TCP | 163.036 ms | 20.500 ms | 7.95× |
| HTTP/2 over TLS | 163.167 ms | 20.847 ms | 7.83× |
| HTTP/3 over QUIC | 164.040 ms | 21.016 ms | 7.81× |

The expected timer floor changes from roughly 160 ms to 20 ms. These numbers
show that independent handler waits overlap; they are not general throughput
multipliers. No claim is made about CPU-bound handlers, production latency,
other reactor/thread layouts, or Asio/libuv HTTP performance.

[All final samples](final/samples.csv), [summary](final/summary.json), and
[environment, exact commands and SHA-256 provenance](final/metadata.json) are retained.
The final measurement uses the clean source revision
[`ea6ca41`](https://github.com/TabNahida/PACE/commit/ea6ca4141d64044ed0084bc75f224c5dad5c8321),
including the TLS duplex/backpressure and TCP half-close fixes. The metadata
also records per-source and binary hashes. Dependencies are OpenSSL 3.5.7, nghttp2 1.70.0 and
nghttp3 1.18.0, pinned by the recorded xmake files. The release build command was:

```sh
XMAKE_GLOBALDIR=/tmp/pace-xmake XMAKE_CONFIGDIR=/tmp/pace-protocol-perf-config \
  xmake f -y --http1=n --websocket=n --http2=y --tls=y --http3=y \
  --protocol_benchmarks=y -m release -o /tmp/pace-protocol-perf
XMAKE_GLOBALDIR=/tmp/pace-xmake XMAKE_CONFIGDIR=/tmp/pace-protocol-perf-config \
  xmake b -j 4 pace_protocol_multiplex
python3 scripts/run_protocol_multiplex.py \
  /tmp/pace-protocol-perf/linux/x86_64/release/pace_protocol_multiplex \
  --output /tmp/pace-protocol-multiplex-results --batches 10
```

The runner enforces Linux-only measurement. Remote CI may run the probe to check
correctness, but its times are not used in these results or performance rankings.
Use a new output directory; the recorder refuses to overwrite earlier evidence.

The earlier driver iteration's [samples](samples.csv), [summary](summary.json)
and [source manifest](metadata.json) remain available separately. Those values
precede the bounded TLS ciphertext driver and are not used in the table above.
