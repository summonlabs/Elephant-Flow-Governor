# Elephant Flow Governor

Open-source, vendor-neutral C++20 runtime for generation-bound identification and
governance of long-lived high-volume network flows under capacity, policy,
service, and contention constraints.

**Question this runtime answers.** Given authoritative flow-volume, duration,
rate, path, service-class, capacity, and contention evidence, which flows qualify
as elephants *now*, what impact are they creating, which governance action is
*authorized*, and when must that classification or action be revoked, fenced, or
revalidated?

**Boundary.** Elephant Flow Governor owns elephant classification and bounded
governance intent. It does not own generic flow classification, path placement,
flow scheduling, rate enforcement, congestion synthesis, admission, reservations,
pacing execution, or forwarding. See [docs/BOUNDARY.md](docs/BOUNDARY.md) for the
full statement.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and nothing else. There are
no third-party dependencies and no network access is used at configure time.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows the helper scripts locate the Visual Studio toolchain themselves:

```
scripts\build.cmd Release
scripts\consumer_smoke.cmd
scripts\fresh_clone_check.cmd
```

| Option | Default | Meaning |
| --- | --- | --- |
| `EFG_BUILD_APPS` | ON when top level | Build `efg-governor`, `efg-coordinator`, `efg-worker`, `efg-bench` |
| `EFG_BUILD_TESTS` | ON when top level | Build the test suites |
| `EFG_WARNINGS_AS_ERRORS` | ON | MSVC `/W4 /WX`, GCC/Clang `-Wall -Wextra -Werror` |
| `EFG_SANITIZE` | OFF | AddressSanitizer (`/fsanitize=address` on MSVC, `-fsanitize=address,undefined` elsewhere) |

## Install and consume

```
cmake --install build --prefix /some/prefix
```

```cmake
find_package(EFG 1.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE efg::efg)
```

[examples/downstream_consumer](examples/downstream_consumer) is an independent
project that only knows what the installed package publishes. It is built and run
by `scripts/consumer_smoke.cmd`.

## The model

Everything authoritative is bound to an explicit identity and generation.

| Concept | Type | Notes |
| --- | --- | --- |
| Flow | `FlowId` + `Generation` | A rollover or an identity reuse advances the generation and voids every older classification |
| Evidence window | `EvidenceWindowId` + sequence | Non-overlapping, strictly ordered, digest checked |
| Path | `PathId` + `Generation` | A re-route restarts the measurement epoch |
| Capacity | `CapacitySnapshotId` + `Generation` | Reserved capacity is never exceeded and never targeted |
| Policy | `PolicyId` + `Generation` | A new generation revokes every classification the old one justified |
| Classification | `ClassificationId` + `Generation` | Bound to one authority vector |
| Intent | `IntentId` + `AttemptId` | Idempotent on the attempt identifier |
| Incarnation | `EpochId` + `BootId` | A restart mints a new boot and advances the epoch, fencing all older authority |
| Publisher | `PublisherId` + publisher boot | Evidence from a different publisher incarnation does not inherit authority |

### Evidence

Each `FlowSample` carries the cumulative volume since the flow generation began
and the volume observed inside its own window. That makes duplicates, replays,
contradictions and cumulative regressions detectable rather than silently
absorbed. Retained windows must form a contiguous run: any discontinuity — a
path change, a short interruption, or a telemetry gap — starts a new measurement
epoch, so a rate is never computed across a boundary that no evidence covers.

### Classification

Classification is a three-valued decision over a composed rule:

- **Elephant** — the policy's enter rule is definitively true, the confirmation
  requirement is met, and the evidence is fresh and generation-bound;
- **NotElephant** — the rule is definitively false;
- **Unknown** — the rule cannot be decided from the evidence available. UNKNOWN
  never classifies and never carries authority;
- **Suspended** — a previously authoritative classification lost its
  justification: evidence aged out, a telemetry gap opened, a path, capacity or
  policy generation moved, or the publisher incarnation was replaced. Authority
  is withdrawn immediately; re-earning it requires fresh evidence.

Classification is not punishment. A protected or reserved flow classifies as an
elephant and is still never throttled: its authority level is `observe`, not
`govern`.

### Hysteresis

Entering and leaving the elephant state are separate decisions with separate
confirmation counts. The exit rule is derived from the *instantaneous*
thresholds of the enter rule (rate, share, contention), scaled down; monotone
history thresholds such as cumulative volume and elapsed duration are excluded,
because they can never become false and would make the exit rule unreachable. A
flow that has fallen below the exit threshold still needs
`exit_confirm_windows` consecutive evidence windows before it leaves the state.

### Governance intent

An intent is a bounded statement, never an action:

```
ObserveOnly                  classification holds, no action is taken
ProtectReservedFlow          a protected obligation is recorded as untouched
RequestAlternatePlacement    a request, with no path chosen
RequestRateShaping           a bounded rate reduction in basis points
RequestSchedulingIsolation   a request, with no scheduler programmed
ReduceCompetingAdmission     a bounded target count
EscalateCongestion           an escalation, not a congestion algorithm
```

Every intent carries the authority vector that justified it, a requested
envelope, a granted envelope that is never wider than the policy ceiling, and a
validity window. State transitions are `Authorized`, `Clamped`, `Suppressed`,
`Active`, `Revoked`, `Fenced`, `Expired` and `Completed`.

### Explanation

`Governor::explain` returns a bounded, structured explanation and renders it as
text or as deterministic JSON: the state and authority level, every reason code,
the derived measurements, the affected binding resource and share, every
evaluated threshold with its observed and threshold values, the impact
assessment, the authorized and suppressed intents, and the exact authority
vector. Rendering is hand written — no locale, no iostream state, no floating
point formatting differences between platforms.

## Durable state

`DurableGovernor` wraps a journal and a crash-safe snapshot. The journal is
append-only, length-prefixed and CRC-32C checked per record; the snapshot is
written to a temporary file, forced to stable storage, then atomically renamed.
Evidence is made durable **before** it can influence a decision.

Recovery distinguishes durable configuration, committed authoritative state,
unfinished attempts, ambiguous outcomes, stale live authority, and evidence that
requires revalidation. The governing rule is that durable state never silently
restores liveness, telemetry freshness, lease validity, publisher authority or
hardware effect:

- restoring requires a boot identity distinct from the stored one and an epoch
  beyond it — anything else is refused, not repaired;
- every restored classification is demoted to `Suspended`;
- every live intent restored from the snapshot is fenced;
- a snapshot that fails integrity checking is reported as an ambiguous outcome,
  and the state it held is reported as lost rather than reconstructed.

## Multi-publisher operation

`efg-coordinator` owns the authoritative governor and an epoch.
`efg-worker` is a publisher: it registers one incarnation, submits generation-
bound evidence over real loopback sockets with a real length-prefixed framing
layer, and holds no authority of its own.

Sessions are served one at a time, in accept order, which removes an entire class
of lock-ordering hazards: the coordinator's governor is single-threaded and no
lock is ever held across a socket operation. Fencing is fail-closed:

- a Hello carrying an epoch below the coordinator's is fenced;
- a Hello carrying an epoch above the coordinator's is fenced;
- a replayed session nonce is fenced;
- any frame whose epoch, boot or worker identity disagrees with the registered
  session is fenced and the connection is closed;
- a worker that dies mid-stream is detected as end of stream; the evidence it
  supplied ages out and the flows it was reporting on are suspended on the next
  sweep rather than remaining authoritative elephants.

## Tools

| Tool | Purpose |
| --- | --- |
| `efg-governor` | Run one synthetic population through the governor and print the decision, explanations and intent |
| `efg-bench` | Measure completed classification work over a synthetic mixed population |
| `efg-coordinator` | Serve worker processes, own the epoch, write a session report |
| `efg-worker` | Publish one shard of generation-bound evidence |

All four take the same scenario options (`--mice`, `--elephants`, `--seed`,
`--ticks`, `--window`, `--resources`, `--capacity`, `--reserved`, `--paths`,
`--path-resources`, `--complexity`, `--protected-bp`, `--abandon-bp`).

## Test suites

| Suite | What it defends |
| --- | --- |
| `efg.unit` | Arithmetic, hashing, identity, logical time, evidence ledger, capacity and path tables, canonical codecs |
| `efg.governance` | Classification invariants, hysteresis, protection, intent bounds and idempotency, explanation content |
| `efg.persistence` | Journal integrity, torn tails, mid-file corruption, snapshot atomicity, restart and revalidation |
| `efg.concurrency` | Service lifecycle, bounded queues, cancellation, drain-on-stop, sink re-entrancy |
| `efg.adversarial` | Malformed, truncated, corrupt, oversized, contradictory and hostile input at every decoder |
| `efg.property` | Seeded randomized replay determinism and mutation detection |
| `efg.benchmark` | Synthetic population behaviour and determinism |
| `efg.cluster` | Real worker processes over real sockets: clean sessions, abrupt death, termination, epoch and nonce fencing |

No test applies a timeout. A wait is always on a definite event — a process
handle, a published file, a queue predicate — and a hang is a defect to diagnose
rather than something to hide behind a watchdog.

## Proof surface

| Claim | Status |
| --- | --- |
| Classification determinism | REAL — two governors fed identically produce identical decision digests, history digests and rendered explanations (`efg.property`, `efg.governance`) |
| Protected-flow behaviour | REAL — protected flows classify and are never correctly governed (`efg.governance`, `efg.benchmark`) |
| Stale-state refusal | REAL — evidence, path, capacity and policy staleness each withdraw authority (`efg.governance`) |
| Bounded corrective intent | REAL — granted envelopes are never wider than the policy ceiling (`efg.governance`) |
| Restart and revalidation | REAL — on-disk journal and snapshot, new boot and advanced epoch, demotion and fencing (`efg.persistence`) |
| Real multiprocess claims | REAL — worker processes spawned by the test, real loopback sockets, real framing, real process kill and termination (`efg.cluster`) |
| AddressSanitizer | REAL on MSVC — `EFG_SANITIZE=ON` builds every target with `/fsanitize=address` and the suite passes under it |
| UndefinedBehaviorSanitizer | UNSUPPORTED on MSVC; the CMake option applies it on GCC and Clang only |
| Synthetic population throughput | SYNTHETIC — produced by `efg-bench` from a generated population. These are library numbers on the machine that ran them, never physical network measurements |
| Multi-node, multi-switch, RDMA, NIC, DPU, optical validation | UNSUPPORTED — no such hardware was exercised, and nothing in this repository claims it |

## Documentation

- [docs/BOUNDARY.md](docs/BOUNDARY.md) — what the runtime owns and what it refuses to own
- [docs/LOCK_AUDIT.md](docs/LOCK_AUDIT.md) — deadlock and lock-reentrancy audit
- [docs/OPERATIONS.md](docs/OPERATIONS.md) — running the coordinator and workers
- [docs/VALIDATION.md](docs/VALIDATION.md) — the closure record for this release

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
