# Windows ARM64 OpenSSL handshake diagnosis

This is a functional correctness investigation, not a performance measurement.
The candidate build correction is restricted to native Windows ARM64 and
OpenSSL 3.5.7. Other platforms continue to use the upstream xmake package.

## Reproduction and captured failure

[CI run 40020194516, Windows ARM64 job 120642440161](https://github.com/TabNahida/PACE/actions/runs/40020194516/job/120642440161)
tested revision `75c0d326ed63e8e7a5c7e5a7bd7643a45139b036`.
OpenSSL reported `VC-WIN64-ARM`, static `/MT`, `/O2`, and `/Gs0`.
The PE architecture check confirmed an ARM64 executable.
The captured executable's SHA-256 is
`5c0f825e368c1691ecf546f1f7372f86a0fee3bc398aae99d0deb391afd524c1`.

`pace_openssl_diagnostic` links OpenSSL directly, without PACE sockets,
coroutines or TLS transport code. Its memory BIO client/server handshake
requires both certificate-chain and `localhost` hostname verification, then
checks application data and bidirectional `close_notify`.

| Independent process | Result before correction |
|---|---|
| RAND, SHA256, AES-GCM, X25519 import, P-256 key generation | Pass |
| Contexts, default trust paths, certificate and private-key loading | Pass |
| Legacy custom BIO callbacks with `BIO_read_ex` / `BIO_write_ex` | Pass |
| ML-KEM-768 generation, encapsulation and decapsulation | Pass |
| Default TLS handshake | Access violation |
| TLS 1.2 only / TLS 1.3 only | Access violation in both |
| TLS 1.3 with X25519 only / P-256 only | Access violation in both |

All five handshake variants captured the same first-chance exception:

```text
exception=0xc0000005  rva=0x86f20
operation=0 (read)   address=0x879
x0=0x1              lr=image+0x86f1c
tls_parse_all_extensions+0x10
```

The matching executable's disassembly contains:

```asm
; tls_parse_all_extensions, RVA 0x86f10
sub sp, sp, #0x60
mov x15, #1
bl  __chkstk                 ; RVA 0x86f18: overwrites x30/LR
sub sp, sp, x15, lsl #4      ; RVA 0x86f1c
ldr x8, [x0, #0x878]         ; RVA 0x86f20: captured fault
; ...
str x30, [sp, #0x60]         ; RVA 0x86f48: saves LR too late
; ... successful return path ...
ldr x30, [sp, #0x60]
; restore saved registers
mov w0, #1
add sp, sp, #0x70
ret
```

The stack-probe call precedes the first save of the caller's return address.
Consequently, the success return branches back into the function at
`image+0x86f1c`, with the integer result `1` in `x0`. The following load reads
`1 + 0x878 = 0x879`, exactly matching the captured exception. The repeated
`tls_parse_all_extensions+0xc` entries in the diagnostic stack are therefore
not evidence of recursive source-level calls.

## Scoped build correction

The local `pace-openssl3-arm64` package inherits the upstream OpenSSL 3.5.7
source URL, checksum and installer. Before calling that installer it changes
the single `/Gs0 /GF /Gy` configuration entry to `/Gs4096 /GF /Gy`.
The replacement rejects unexpected upstream configuration changes. A distinct
package identity and disabled precompiled-package lookup prevent reusing the
old `/Gs0` dependency binary.

`/Gs4096` is Microsoft's normal stack-probe threshold. Large frames still
probe stack pages; `/GS`, `/O2`, TLS versions, certificate verification,
elliptic curves and post-quantum algorithms are unchanged. The rationale is
documented in Microsoft's [stack-checking option reference](https://learn.microsoft.com/en-us/cpp/build/reference/gs-control-stack-checking-calls)
and [ARM64 prologue/unwind reference](https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling).

At this revision this is a candidate correction. Acceptance requires a fresh
native ARM64 build, all nine independent OpenSSL diagnostic processes, and the
ordinary TLS-dependent test suites. No TLS tests are skipped or allowed to
pass by ignoring failures.

## Inspecting the evidence

The job uploads per-stage stdout/stderr and `summary.json` in its test-results
artifact, plus the executable, PDB and linker map in
`openssl-diagnostic-windows-arm64-symbols`. GitHub artifacts expire according
to the workflow retention period; the fault and relevant instructions are
preserved above.

```sh
gh run download 40020194516 --repo TabNahida/PACE \
  --name openssl-diagnostic-windows-arm64-symbols --dir arm64-before
llvm-objdump -d --start-address=0x140086f10 --stop-address=0x140087118 \
  arm64-before/build/ci/windows/arm64/release/pace_openssl_diagnostic.exe
```

For another build, use its own linker map to find the symbol; these addresses
belong only to the captured binary. The diagnostic's default-group handshake
remains part of ordinary `xmake test` on every TLS-enabled platform.
