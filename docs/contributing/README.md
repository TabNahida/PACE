# Contributing

[简体中文](README.zh.md) · [Documentation](../README.md)

How to build, test and change PACE.

| Page | Covers |
|:--|:--|
| [Writing documentation](documentation.md) | Layout, translation, the reference template, `///` comments |
| [CI](ci.md) | What every workflow checks and on which platforms |
| [Windows arm64 OpenSSL](windows-arm64-openssl.md) | Why that build uses a pinned OpenSSL package, and how it was diagnosed |

## Build

```sh
xmake f -m debug -y                                                  # runtime, net, HTTP/1.1, WebSocket
xmake f -m debug --tls=y --dns=y --quic=y --http2=y --http3=y -y     # every module
xmake -y
```

Use a separate build directory per configuration (`-o build/<name>`) to switch between them without rebuilding.
Inside a nested git worktree, pass `-P .` to every xmake command, or xmake finds the parent checkout's project.
[Every option](../reference/build-options.md)

## Test

```sh
xmake test                     # every test program of this configuration
xmake run pace_network_tests   # one program; doctest options such as -tc="name*" select test cases
```

Tests use [doctest](https://github.com/doctest/doctest) and live in `tests/<module>/`. Each test case runs under a
watchdog that ends the process with exit code 124 if it hangs. Before sending a change, run the tests in the
configurations it touches; for runtime and socket changes that includes `--sanitizers=y` (AddressSanitizer and
UndefinedBehaviorSanitizer). Tests must not depend on wall-clock timing: synchronize on events, not on sleeps.

CI also builds each module on its own and compiles every public header alone (`--header_check=y`), so a header
must include what it uses.

## Style

- Formatting is clang-format 22.1.8 with the repository's `.clang-format` (Allman braces, 120 columns, braces on
  every control statement): `pip install clang-format==22.1.8`, then `python3 scripts/format.py` to format and
  `python3 scripts/format.py --check` to verify.
- clang-tidy 22.1.8 with `.clang-tidy` runs in CI on the lines each change touches and fails on any warning. A
  check that does not fit a line is silenced with `// NOLINT(check): reason`, never without a reason.
- Names follow the standard library: `lower_case` for types, functions and variables, a trailing `_` on private
  data members, `CamelCase` template parameters, `PACE_` macros.
- Public declarations carry `///` comments; implementation comments say why, not what.
- Headers that only `.cpp` files use go in `src/<module>/`, not in `include/`.
- A change to the hot paths in `src/runtime/` or `src/net/` needs a paired benchmark comparison
  (`scripts/compare_runtime_perf.py`); innocent-looking signature changes have changed the generated code before.

## Fuzzing

```sh
xmake f -m releasedbg --toolchain=clang --fuzz=libfuzzer -y && xmake -y pace_fuzz_http1_server
xmake f -m debug --fuzz=replay -y && xmake -y && xmake test      # replay the seed corpus without libFuzzer
```

Fuzz targets are in `tests/fuzz/` with their seed corpora in `tests/fuzz/corpus/`. Add a corpus file for every
parser bug you fix.

## Interoperability

```sh
python3 tests/interop/run_interop.py --build-dir build/<name> --list
python3 tests/interop/run_interop.py --build-dir build/<name>
```

The runner starts `pace_interop_server` (built with the tests) and runs every external peer it finds, such as
curl and h2spec; a suite whose peer is not installed is skipped. Peers and suites are listed in
`tests/interop/peers.json`.

## Benchmarks

Performance claims need numbers measured as described in [Performance](../performance/README.md#how-numbers-are-made).
`scripts/run_baseline.py` runs every suite; `scripts/compare_runtime_perf.py` compares two builds with alternating
runs. Results that a page quotes go under `benchmarks/results/<name>/` with their raw data.

## Documentation

Every page exists in English and Chinese; see [Writing documentation](documentation.md). Before sending a docs
change, run `python3 scripts/check_docs.py` and `mkdocs build --strict`. If you change benchmark results quoted by
the README, regenerate its figures with `python3 scripts/render_readme_figures.py`.

## Commits

One logical change per commit, with an imperative subject and a body that explains what changed and why.
Large mechanical changes (formatting, moves) go in commits of their own and into `.git-blame-ignore-revs`.
