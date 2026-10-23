# HTTP/1 response completion: local Linux evidence

[Issue #15](https://github.com/TabNahida/PACE/issues/15) · [API semantics](../../../docs/reference/http.md#final-response-close-policy) · [Probe](../../http_close.cpp) · [Recorder](../../../scripts/run_http_close.py)

The old client parsed the complete HTTP response, then waited for transport
shutdown before returning it. A deliberately delayed peer FIN/close_notify made
that delay visible even with complete Content-Length framing. The updated default
attempts local shutdown with a zero peer-wait budget; `graceful` remains opt-in.
These are PACE-before/after completion measurements, not an overall HTTP throughput
comparison or a comparison with Asio/libuv. No issue-reported Windows numbers are
used here.

## Main matched comparison

Median `send()` duration in **milliseconds**, 10 trials per cell. Connect and TLS
handshake are outside the timed interval. Both endpoints use TCP_NODELAY.
Response payload is two bytes. The server writes the response, waits the requested
delay, then begins graceful close. Request/response both request close for this table.

| Peer close delay | TCP before | TCP no_wait | TLS before | TLS no_wait |
|---:|---:|---:|---:|---:|
| 0 | 0.0291 | 0.0308 | 0.1198 | 0.1229 |
| 1 | 1.1057 | 0.0113 | 1.1818 | 0.1039 |
| 10 | 10.1079 | 0.0126 | 10.2082 | 0.1030 |
| 50 | 50.1444 | 0.0187 | 50.2601 | 0.1098 |
| 200 | 100.2227 | 0.0251 | 100.3165 | 0.1199 |

For all 80 updated closing samples with a 1/10/50/200 ms peer delay, `send()`
completed before the peer started closing. The old client waited for peer closure
or its 100 ms default shutdown budget. With a zero peer delay, the peer had already
started closing before either client finished, so no teardown wait was eliminated.

The keep-alive controls below use the same delayed peer but return without final
cleanup in both versions. The probe then closes the client explicitly, outside
`send()`. They are **not** measurements of repeated requests on a warmed connection.

| Peer close delay | TCP before | TCP no_wait | TLS before | TLS no_wait |
|---:|---:|---:|---:|---:|
| 0 | 0.0245 | 0.0260 | 0.1105 | 0.1130 |
| 1 | 0.0092 | 0.0089 | 0.0961 | 0.0966 |
| 10 | 0.0107 | 0.0098 | 0.0945 | 0.0981 |
| 50 | 0.0161 | 0.0139 | 0.0975 | 0.1020 |
| 200 | 0.0217 | 0.0204 | 0.1074 | 0.1117 |

Each JSON summary includes min/max in addition to the median. Ten samples support
this large induced-wait diagnosis; they do not establish a sub-microsecond ranking
or general tail-latency distribution.

## Policy and framing controls

Explicit `graceful`, 100 ms budget, five samples per cell:

| Peer close delay (ms) | TCP median send (ms) | TLS median send (ms) |
|---:|---:|---:|
| 0 | 0.0313 | 0.1233 |
| 1 | 1.0839 | 1.2090 |
| 10 | 10.1066 | 10.2444 |
| 50 | 50.1512 | 50.2872 |
| 200 | 100.2329 | 100.3412 |

Other framing, 50 ms peer delay, two samples per cell (functional controls, not
stable percentile estimates):

| Framing | TCP median send (ms) | TLS median send (ms) | Returned before peer close |
|---|---:|---:|---|
| chunked | 0.0347 | 0.1414 | Yes, 4/4 |
| head | 0.0328 | 0.1258 | Yes, 4/4 |
| 204 | 0.0339 | 0.1258 | Yes, 4/4 |
| eof | 50.1663 | 50.2987 | No, 0/4 |

EOF framing still waits for peer EOF; TLS additionally authenticates its close
notification. The no-wait policy is applied only after the response is complete.
It does not turn truncation into success. Server final-response drain is unchanged,
including early rejection before reading the whole request body.

## Raw data and build provenance

- [Before CSV / JSON](before/): 200 samples; [environment](before/environment.json), [samples](before/samples.json), [summary](before/summary.json).
- [Updated default CSV / JSON](after/): 200 samples; [environment](after/environment.json), [samples](after/samples.json), [summary](after/summary.json).
- [Explicit graceful](graceful/): 50 samples; [summary](graceful/summary.json).
- [Other framing controls](framing/): 16 samples; [summary](framing/summary.json).
- [Before build manifest](before-build.json), [after build manifest](after-build.json), [compiler/link details](build-notes.json), [HTTP implementation patch](implementation.patch), [common build patch](common-build.patch).
- [Checksum inventory](SHA256SUMS) covers all stored evidence files; verify from this directory with `sha256sum --check SHA256SUMS`.

The HTTP baseline is `61fe0e21d62205fca77d1fb1e07e91aa200b6050` (v0.1.1).
These binaries were built in the shared development checkout, **not** a pristine
release checkout. Both contain the same additive TLS protocol-driver work from
the concurrent HTTP/2 investigation; HTTP/1 never calls those new methods. The
common patch records that exact addition. Runtime/networking/TLS compiled-source
hashes match between binaries. The effective source differences are
`include/pace/http.hpp` and `src/application/http.cpp`. A changed `quic.hpp`
inventory entry is entirely excluded by the disabled `PACE_HAS_QUIC` guard.
Other protocol headers recorded for inventory are not compiled into this probe.

The manifests' `revision` and before label identify the HTTP baseline source.
The environment files record the measurement checkout HEAD (`0efd2a8`, a CI-only
commit on top of it), uncommitted state, frozen executable hash, build-manifest
hash and compiled-source hashes separately from the then-current working tree.
The pre-fix binary was compiled before changing the HTTP implementation. Both
use the same probe; `PACE_HTTP_CLOSE_BASELINE` only selects the old constructor.

Environment: local x86_64 Linux under WSL2, kernel
`6.18.40.1-microsoft-standard-WSL2`, Intel Core Ultra 7 270K Plus, glibc 2.39,
GCC 13.3.0, C++20 `-O3 -DNDEBUG`, statically linked OpenSSL 3.5.7.
Formal runs occurred 2026-10-23 20:04:51–20:05:16 UTC, sequentially before, after,
graceful, framing. Other task agents paused their local builds/tests for this
window. There was no CPU affinity/frequency control or external host-load guarantee.
The recorder checks sample count, matrix completeness, duplicates, configuration,
finite timings and validated payloads; all 466 formal samples passed.

## Initial socket-policy diagnostic

[The initial raw run](before-nagle-default/) and its
[probe snapshot](before-nagle-default-probe.cpp),
[recorder snapshot](before-nagle-default-runner.py) and
[build manifest](before-nagle-default-build.json) are retained, not discarded.
Its raw TLS peer left Nagle enabled, introducing roughly 40 ms in many keep-alive
controls as well. This confounded isolation of shutdown waiting. The real PACE
HTTP server already enables TCP_NODELAY; these initial numbers are **not** normal
PACE HTTP-server overhead. The formal before/after comparison above explicitly
sets TCP_NODELAY on both endpoints and records it in every row.

## Reproduce

Build the current probe through xmake:

```sh
xmake f -m release --http1=y --tls=y --protocol_benchmarks=y -y
xmake -y pace_http_close_probe
python3 scripts/run_http_close.py \
  --executable build/linux/x86_64/release/pace_http_close_probe \
  --output /tmp/http-close-new-run --label current --trials 10
```

The output directory must not exist. Keep other performance runs and builds idle.
The runner accepts an optional `--build-manifest` and verifies its executable hash.
For the exact old/new source comparison, use the recorded patches and the following
Bash commands from the repository root; replace the OpenSSL installation path
with one containing `include/`, `lib/libssl.a` and `lib/libcrypto.a`:

```bash
pace_close_repo="$PWD"
pace_close_evidence="$pace_close_repo/benchmarks/results/http-close-linux-2026-10-23"
pace_close_work="$(mktemp -d /tmp/pace-close-repro.XXXXXX)"
pace_close_openssl=/path/to/openssl-3.5.7
mkdir "$pace_close_work/before" "$pace_close_work/after"
git archive 61fe0e21d62205fca77d1fb1e07e91aa200b6050 | tar -x -C "$pace_close_work/before"
git -C "$pace_close_work/before" apply "$pace_close_evidence/common-build.patch"
cp -a "$pace_close_work/before/." "$pace_close_work/after/"
git -C "$pace_close_work/after" apply "$pace_close_evidence/implementation.patch"
for pace_close_variant in before after; do
  pace_close_tree="$pace_close_work/$pace_close_variant"
  pace_close_defines=()
  if [ "$pace_close_variant" = before ]; then
    pace_close_defines=(-DPACE_HTTP_CLOSE_BASELINE)
  fi
  g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Werror -DPACE_HAS_TLS \
    "${pace_close_defines[@]}" -I"$pace_close_tree/include" -I"$pace_close_openssl/include" \
    "$pace_close_repo/benchmarks/http_close.cpp" \
    "$pace_close_tree/src/application/http.cpp" "$pace_close_tree/src/extensions/tls.cpp" \
    "$pace_close_tree/src/networking/net.cpp" "$pace_close_tree/src/networking/udp.cpp" \
    "$pace_close_tree/src/runtime/io_context.cpp" "$pace_close_tree/src/runtime/poller.cpp" \
    "$pace_close_tree/src/runtime/runtime.cpp" \
    "$pace_close_openssl/lib/libssl.a" "$pace_close_openssl/lib/libcrypto.a" \
    -pthread -ldl -o "$pace_close_work/http-close-$pace_close_variant"
done
# Run only after both builds finish; do not compile during measurements.
for pace_close_variant in before after; do
  python3 scripts/run_http_close.py \
    --executable "$pace_close_work/http-close-$pace_close_variant" \
    --output "$pace_close_work/results-$pace_close_variant" \
    --label "$pace_close_variant" --trials 10 --transports tcp tls --framings length
done
python3 scripts/run_http_close.py --executable "$pace_close_work/http-close-after" \
  --output "$pace_close_work/results-graceful" --label graceful \
  --trials 5 --connection close --close-policy graceful
python3 scripts/run_http_close.py --executable "$pace_close_work/http-close-after" \
  --output "$pace_close_work/results-framing" --label framing \
  --trials 2 --delays-ms 50 --connection close --framings chunked head 204 eof
```

Use the probe and recorder matching the hashes in `build-notes.json` when
reproducing this snapshot. Executable hashes may change with paths/toolchain;
source and dependency hashes record what was used for the published run.

## Regression validation

`pace_http_extended_tests` passed all 22 groups in release and in a separate
AddressSanitizer + UndefinedBehaviorSanitizer build (`detect_leaks=1`, both
`halt_on_error=1`). The new tests include TCP/TLS gated peer close, Content-Length,
chunked trailers, HEAD, 204/304/205, request-only close, EOF with both policies,
explicit graceful peer/budget completion, zero-budget get/fetch forwarding,
truncation and malformed trailers, abrupt TLS EOF authentication, invalid
negative budget, and cancellation after graceful cleanup has begun. Existing
server early-admission/drain, streaming, deadlines and cancellation tests still pass.
The gated peer cannot start closing before the client releases it, avoiding a
performance threshold as the only correctness check.
