# Validation record

This file records what was actually executed for release 1.0.0, on the machine
described below, from the committed sources.

## Environment

| Item | Value |
| --- | --- |
| Operating system | Windows |
| Compiler | Microsoft (R) C/C++ Optimizing Compiler 19.44.35222 for x64 |
| Build system | CMake 4.3.2 with Ninja 1.13.2 |
| Language standard | C++20 (`/std:c++20`, extensions disabled) |
| Warning policy | `/W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8` |

## Configurations built and tested

| Configuration | Flags | Result |
| --- | --- | --- |
| Release | `-DCMAKE_BUILD_TYPE=Release` | Builds warning-free under `/W4 /WX`; all 8 suites pass |
| Debug | `-DCMAKE_BUILD_TYPE=Debug` | Builds warning-free under `/W4 /WX`; all 8 suites pass |
| AddressSanitizer | `-DEFG_SANITIZE=ON`, `RelWithDebInfo` | Builds with `/fsanitize=address` on every target; all 8 suites pass |

The sanitizer runtime is present and active: a deliberate heap-buffer-overflow
probe compiled with the same flags is reported by AddressSanitizer, which is the
evidence that the sanitized suite is not running un-instrumented.

UndefinedBehaviorSanitizer is **not** available from MSVC. The CMake option
applies `-fsanitize=address,undefined` for GNU and Clang toolchains; it was not
exercised on this platform and no result is claimed for it.

## Static analysis

`/analyze` was run over the whole library with the external-header filter
enabled. One first-party finding was reported and fixed: the CRC-32C table
constructor tripped the standard library's subscript precondition. The table is
now a plain array with an explicitly bounded loop, and the lookup index is masked
before the narrowing conversion so the 0..255 bound is syntactically evident.

## Install and downstream consumption

The package is installed with `cmake --install`, exporting `EFGTargets.cmake`,
`EFGConfig.cmake` and `EFGConfigVersion.cmake` under `lib/cmake/EFG`.
`examples/downstream_consumer` is a separate project that configures only
against the installed prefix, includes only `<efg/...>` public headers, links
`efg::efg`, and builds under `/W4 /WX` itself. It runs, classifies a flow,
prints the explanation and exits zero. The whole path is reproducible with
`scripts/consumer_smoke.cmd`.

## Fresh clone closure

`scripts/fresh_clone_check.cmd` clones the repository from its own committed
history into a temporary directory, then configures, builds, tests, installs and
consumes it there. Nothing outside the committed tree is required: there are no
third-party dependencies, no `FetchContent`, and no network access.

## Multiprocess claims

`efg.cluster` spawns real worker processes with `CreateProcess`, which speak to
a coordinator over real loopback sockets through the real length-prefixed framing
layer. The suite covers a clean two-worker session, a worker that kills itself
mid-stream, a worker terminated from outside, a worker from an older epoch, a
replayed session nonce, a peer that connects without a hello, and the shipped
`efg-coordinator` and `efg-worker` binaries end to end.

These are **REAL** process, socket and framing results on one machine. No
multi-node, multi-switch, RDMA, NVLink, optical, NIC or DPU hardware was
exercised and no result is claimed for any of it.

## Benchmark

`efg-bench` measures completed classification work — flows submitted, decided and
governed — not enqueue or submission latency. The population is generated and
labelled SYNTHETIC in both the text and JSON renderings. The numbers are library
throughput on the machine that ran them and are never presented as physical
network measurements.

Recorded run (24,000 synthetic flows, 384,000 evidence windows, seed 20260101)
and the smaller mixed run are reproduced in the release report.
