# Operations

## Single process

```
efg-governor --mice 64 --elephants 16 --ticks 64 --window 4 \
             --resources 4 --capacity 65536 --reserved 8192 \
             --paths 8 --path-resources 2 --explain 3 --report report.json
```

Runs one synthetic population, sweeps the horizon and prints the summary, the
explanation of up to `--explain` qualified flows, and the decision digest. With
`--report` it also writes a machine-readable summary.

## Coordinator and workers

The coordinator owns the policy, the capacity snapshots, the path descriptors and
the epoch. Workers publish evidence only.

```
efg-coordinator --boot 1 --epoch 1 --workers 3 --final-tick 1400 \
                --port-file port.txt --report coordinator.json \
                --mice 120 --elephants 32 --ticks 64 --window 4 \
                --resources 4 --capacity 65536 --reserved 8192 \
                --paths 8 --path-resources 2
```

```
efg-worker --port <port> --boot 7 --epoch 1 --nonce 1 --worker-id 1 \
           --label shard-0 --shard 0 --shards 3 --max-batch 64 \
           --mice 120 --elephants 32 --ticks 64 --window 4 \
           --resources 4 --capacity 65536 --reserved 8192 \
           --paths 8 --path-resources 2
```

Every worker must be given the same scenario options as the coordinator, because
the path identifiers, resource identifiers and policy thresholds have to agree.
Each worker takes a distinct `--shard` of `--shards`.

### Options that matter operationally

| Option | Meaning |
| --- | --- |
| `--epoch` | Must equal the coordinator's epoch. An older or newer epoch is fenced. |
| `--nonce` | Must be fresh for each session. A replayed nonce is fenced. |
| `--boot` | Identifies one worker incarnation. A new boot supersedes the old one. |
| `--ready-file` | Written once the session is registered, so a supervisor can wait on a fact |
| `--progress-file` | Rewritten after every acknowledged batch |
| `--abort-after` | Terminate the process abruptly after N flushed batches, with no bye |
| `--final-tick` | Coordinator-only: sweep at this tick after all sessions close |
| `--store` | Coordinator-only: keep committed state in a durable store directory |

### Interpreting the coordinator report

`coordinator.json` contains one entry per session:

- `clean_bye` — the worker closed the session itself. A session without it ended
  by connection loss, which is how an abruptly killed publisher is detected.
- `fenced` and `fence` — the session was refused. `epoch_stale`,
  `epoch_ahead`, `worker_duplicate`, `boot_mismatch` and `frame_malformed`
  are the reasons an operator will see.
- `accepted`, `duplicates`, `rejected` — evidence accounting for the session.

`suspended_decisions` counts classifications whose authority was withdrawn. After
a `--final-tick` sweep this includes every flow whose evidence aged out, which is
exactly the set a dead publisher was reporting on.

## Durable state

Pass `--store <directory>` to the coordinator to keep committed state on disk.
The directory holds:

| File | Contents |
| --- | --- |
| `journal.efgj` | Append-only, length-prefixed, CRC-32C checked records |
| `snapshot.efgs` | Crash-safe compaction artifact, written to a temporary then renamed |
| `snapshot.efgs.tmp` | Only present if a snapshot write was interrupted; safe to delete |

A restart must use a boot identity distinct from the stored one and an epoch
beyond it. The runtime refuses anything else rather than repairing it.

## Timeouts

None of the tools apply a timeout to a peer. A worker that stalls is a defect to
diagnose; a worker that dies produces end of stream, which the coordinator
handles. The only bounded waits in the codebase are readiness polls in the test
suite, and each of them fails loudly if the event never occurs.
