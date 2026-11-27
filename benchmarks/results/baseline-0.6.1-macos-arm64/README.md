# Baseline: v0.6.1

- Host: Apple M6, 12 logical CPUs, 16 GiB, macOS 27.2, on AC power, otherwise idle (macOS-27.2.1-arm64-arm-64bit, arm64, 12 logical CPUs)
- Repeats per workload: 5
- Values are the median of all trials, with min–max spread; raw records are in `raw/` and the runner-owned files next to this summary.

## Suites

| Suite | Status | Seconds | Note |
|:--|:--|--:|:--|
| runtime_hotpaths | ok | 1.7 |  |
| net_hotpaths | ok | 1.5 |  |
| components | ok | 6.6 |  |
| router | ok | 0.2 |  |
| inbox_allocations | ok | 0.0 |  |
| tcp_echo | ok | 3.2 |  |
| scaling | ok | 4.8 |  |
| websocket | ok | 12.3 |  |
| http_close | ok | 12.3 |  |
| protocol_multiplex | ok | 33.8 |  |
| gateway_components | ok | 9.3 |  |
| quic_transport | skipped | 0.0 | pace_quic_transport is built only on Linux |
| http_gateway | skipped | 0.0 | the nginx gateway comparison uses pace_http_load, which is built only on Linux |

## Directly measured workloads

| Suite | Workload | Metric | Median | Min | Max | Samples |
|:--|:--|:--|--:|--:|--:|--:|
| runtime-hotpaths | cancel sparse / 0 background | p50_ns | 250.000 | 250.000 | 250.000 | 5 |
| runtime-hotpaths | cancel sparse / 0 background | p99_ns | 292.000 | 291.000 | 292.000 | 5 |
| runtime-hotpaths | cancel sparse / 65536 background | p50_ns | 250.000 | 250.000 | 292.000 | 5 |
| runtime-hotpaths | cancel sparse / 65536 background | p99_ns | 292.000 | 292.000 | 1,250.000 | 5 |
| runtime-hotpaths | readiness / 1 socket | p50_ns | 1,750.000 | 1,709.000 | 6,417.000 | 5 |
| runtime-hotpaths | readiness / 1 socket | p99_ns | 5,125.000 | 4,458.000 | 22,833.000 | 5 |
| runtime-hotpaths | readiness / 256 sockets | p50_ns | 25,000.000 | 24,750.000 | 25,375.000 | 5 |
| runtime-hotpaths | readiness / 256 sockets | p99_ns | 46,875.000 | 44,291.000 | 98,875.000 | 5 |
| runtime-hotpaths | inbox / 65536 roots | p50_ns | 3,875.000 | 3,833.000 | 4,042.000 | 5 |
| runtime-hotpaths | inbox / 65536 roots | p99_ns | 6,958.000 | 5,542.000 | 8,709.000 | 5 |
| net-hotpaths | poll-dense | ns_per_operation | 117.188 | 116.211 | 120.117 | 5 |
| net-hotpaths | poll-sparse | ns_per_operation | 2,292.000 | 1,791.000 | 2,417.000 | 5 |
| net-hotpaths | udp | ns_per_operation | 53,504.100 | 49,765.100 | 58,708.100 | 5 |
| router | 16 routes | ns_per_operation | 67.915 | 66.651 | 68.600 | 5 |
| router | 1024 routes | ns_per_operation | 82.838 | 80.160 | 84.120 | 5 |
| inbox-allocations | 10000 rounds | reactor_allocations | 2.000 | 2.000 | 2.000 | 1 |
| websocket | 64 B | roundtrips_per_second | 98,532.428 | 92,359.414 | 99,123.200 | 5 |
| websocket | 64 B | p50_us | 9.208 | 9.125 | 9.250 | 5 |
| websocket | 64 B | p99_us | 21.500 | 20.875 | 32.959 | 5 |
| websocket | 16 KiB | roundtrips_per_second | 54,592.102 | 53,769.106 | 55,007.404 | 5 |
| websocket | 16 KiB | p50_us | 16.958 | 16.958 | 17.000 | 5 |
| websocket | 16 KiB | p99_us | 34.042 | 32.250 | 38.875 | 5 |
| websocket | 16 KiB / 1 KiB fragments | roundtrips_per_second | 19,796.934 | 19,553.988 | 20,727.494 | 5 |
| websocket | 16 KiB / 1 KiB fragments | p50_us | 43.209 | 43.167 | 43.500 | 5 |
| websocket | 16 KiB / 1 KiB fragments | p99_us | 151.375 | 149.000 | 152.083 | 5 |
| http-close | tcp / length / peer close +0 ms | request_ms | 0.040 | 0.017 | 0.103 | 200 |
| http-close | tcp / length / peer close +0 ms | after_response_written_ms | 0.020 | 0.006 | 0.053 | 200 |
| http-close | tcp / length / peer close +10 ms | request_ms | 0.087 | 0.019 | 0.136 | 200 |
| http-close | tcp / length / peer close +10 ms | after_response_written_ms | 0.037 | 0.004 | 0.073 | 200 |
| http-close | tcp / chunked / peer close +0 ms | request_ms | 0.037 | 0.025 | 0.098 | 200 |
| http-close | tcp / chunked / peer close +0 ms | after_response_written_ms | 0.018 | 0.009 | 0.045 | 200 |
| http-close | tcp / chunked / peer close +10 ms | request_ms | 0.093 | 0.038 | 0.158 | 200 |
| http-close | tcp / chunked / peer close +10 ms | after_response_written_ms | 0.042 | 0.018 | 0.099 | 200 |
| http-close | tls / length / peer close +0 ms | request_ms | 0.117 | 0.071 | 0.291 | 200 |
| http-close | tls / length / peer close +0 ms | after_response_written_ms | 0.035 | 0.018 | 0.090 | 200 |
| http-close | tls / length / peer close +10 ms | request_ms | 0.244 | 0.090 | 0.550 | 200 |
| http-close | tls / length / peer close +10 ms | after_response_written_ms | 0.056 | 0.018 | 0.259 | 200 |
| http-close | tls / chunked / peer close +0 ms | request_ms | 0.121 | 0.082 | 0.306 | 200 |
| http-close | tls / chunked / peer close +0 ms | after_response_written_ms | 0.036 | 0.019 | 0.104 | 200 |
| http-close | tls / chunked / peer close +10 ms | request_ms | 0.279 | 0.084 | 0.629 | 200 |
| http-close | tls / chunked / peer close +10 ms | after_response_written_ms | 0.069 | 0.019 | 0.169 | 200 |
| protocol-multiplex | h2 / 1 handlers | batch_ms | 188.486 | 180.327 | 194.263 | 50 |
| protocol-multiplex | h2 / 8 handlers | batch_ms | 24.605 | 21.689 | 25.515 | 50 |
| protocol-multiplex | h2tls / 1 handlers | batch_ms | 192.149 | 181.229 | 199.322 | 50 |
| protocol-multiplex | h2tls / 8 handlers | batch_ms | 24.508 | 22.158 | 25.742 | 50 |
| protocol-multiplex | h3 / 1 handlers | batch_ms | 187.975 | 181.472 | 191.813 | 50 |
| protocol-multiplex | h3 / 8 handlers | batch_ms | 25.595 | 22.643 | 28.373 | 50 |
| gateway-components | fnv / 1 workers | operations_per_second | 285,341.000 | 279,139.000 | 288,276.000 | 5 |
| gateway-components | xxh / 1 workers | operations_per_second | 7,107,640.000 | 6,908,380.000 | 7,130,810.000 | 5 |
| gateway-components | shared / 1 workers | operations_per_second | 6,801,950.000 | 6,617,780.000 | 6,962,690.000 | 5 |
| gateway-components | local / 1 workers | operations_per_second | 6,713,170.000 | 6,535,250.000 | 7,041,070.000 | 5 |
| gateway-components | shared / 2 workers | operations_per_second | 3,695,040.000 | 3,447,560.000 | 3,943,280.000 | 5 |
| gateway-components | local / 2 workers | operations_per_second | 9,447,460.000 | 9,213,260.000 | 9,676,700.000 | 5 |
| gateway-components | shared / 4 workers | operations_per_second | 3,794,930.000 | 3,785,220.000 | 3,820,810.000 | 5 |
| gateway-components | local / 4 workers | operations_per_second | 23,094,600.000 | 21,052,700.000 | 23,115,900.000 | 5 |
| gateway-components | shared / 8 workers | operations_per_second | 2,685,130.000 | 2,669,870.000 | 2,693,880.000 | 5 |
| gateway-components | local / 8 workers | operations_per_second | 34,126,000.000 | 32,909,300.000 | 35,518,600.000 | 5 |

TCP echo, components and scaling keep their own reports (`tcp-echo/`, `components.json`, `scaling.json`), written by the existing runners.
