# Deadlock and lock-reentrancy audit

This is a written inspection, not a test result. Every concurrency primitive in
the runtime is listed here with the discipline that governs it.

## Inventory of synchronisation

| Primitive | File | Guards |
| --- | --- | --- |
| `GovernorService::mu_` | `include/efg/service.hpp` | request queue, service state machine, queue high-water mark |
| `g_runtime_mutex` | `src/transport.cpp` | the Winsock/BSD socket runtime reference count, a file-local static |
| `NetworkRuntime` reference count | `src/transport.cpp` | socket runtime lifetime |

There is no other mutex, no recursive mutex, no condition variable, no semaphore
and no lock in the library. In particular `Governor`, `IntentLedger`,
`FlowLedger`, `CapacityTable`, `PathTable`, `Journal`, `SnapshotFile`,
`Coordinator` and `Worker` are **not internally synchronised**: they are
single-threaded objects, and the one place that puts them on another thread
(`GovernorService`, and the coordinator's session thread in tests) is analysed
below.

## Findings against the required checklist

| Pattern | Present? | Analysis |
| --- | --- | --- |
| Read-lock to write-lock re-entry on the same lock | No | There is no reader/writer lock anywhere. `mu_` is a plain `std::mutex`. |
| Write lock held across callbacks or code that reacquires the lock | No | The worker releases `mu_` before calling `process_request`, which is the only path that invokes the governor or the decision sink. Verified by the test `service.a_decision_sink_may_reenter_the_service_from_the_worker_thread`, where the sink calls `stats()`, `queue_depth()` and `state()` — each of which takes `mu_`. If the lock were held, that test would deadlock rather than pass. |
| Mutex re-entry through callbacks | No | `DecisionSink` is invoked from `process_request` with no lock held. There is no other callback surface. |
| Event emission while internal locks are held | No | `Governor::submit_evidence`, `Governor::tick` and `IntentLedger::record` perform no I/O, no logging and no callbacks. Durable writes happen in `DurableGovernor`, which is never called under `mu_`. |
| Worker shutdown while holding locks workers need | No | `stop()` and `cancel()` set state and notify under `mu_`, then **release the lock**, then join. The worker's exit path takes `mu_` only after the queue predicate has already let it out of the wait. |
| Joining a thread while holding state required by that thread | No | Both join sites (`GovernorService::stop`, `GovernorService::cancel`) are outside every lock region; the join statement appears after the `std::lock_guard` scope has ended. |
| Reversed lock ordering on cancellation paths | No | There is only one lock in the process, so no ordering exists to reverse. `g_runtime_mutex` is taken only inside `NetworkRuntime`'s constructor and destructor and never nests with `mu_`. |
| Progress callbacks that re-enter mutable state | No | The only callback is the decision sink, and it runs lock-free on the worker thread by construction. A sink that re-enters service queries is explicitly supported and tested. |
| Shutdown paths that prevent work from completing while waiting for it | No | `stop()` clears `accepting_` but keeps draining the queue: the worker's wait predicate is `!queue_.empty() || state_ != Running`, and the loop only breaks when the queue is empty **and** the state left `Running`. `cancel()` clears the queue under the lock, which makes the predicate immediately true. Both paths are tested (`every_accepted_request_is_processed_before_stop_returns`, `cancellation_discards_pending_work_without_reporting_success`). |
| Nested resource acquisition with inconsistent global ordering | No | No nested acquisition exists: the deepest acquisition is one mutex, held over a bounded deque operation. |
| Losing a wakeup | No | Every wait uses the predicate form of `wait`, so a spurious or missed notification cannot produce an unbounded wait. |
| Reading state while another thread owns it | **Yes, once — fixed** | The multiprocess suite originally polled `Coordinator::sessions()` and `governor()` from the test thread while `run()` owned them on the serving thread. That was a real data race in the test, and it is recorded here rather than hidden: the test now waits on files published by the worker process and only inspects coordinator state after `run()` has returned. `Coordinator::run()`'s documentation states the ownership rule explicitly. |

## Conditions that would create a lock cycle, and why they cannot occur

1. *A sink that blocks forever.* The sink runs on the worker thread with no lock
   held, so a blocking sink stalls the service but cannot deadlock it. The
   service is documented as the place to do bounded work.
2. *`stop()` called from inside a sink.* A sink runs on the worker thread;
   `stop()` joins the worker. Joining the calling thread is self-deadlock.
   `GovernorService::on_worker_thread()` exists precisely so a sink can detect
   this. The library does not call `stop()` from a sink and the documentation
   names the condition.
3. *Coordinator session thread racing the accessors.* Eliminated by the
   documented ownership rule above: sessions are served one at a time and the
   accessors belong to the thread that called `run()` after it returns.

## Shutdown invariants

- After `stop()` or `cancel()` returns, `queue_depth() == 0`,
  `queue_high_water == 0` and no worker thread is joinable.
- After `stop()` returns, `stats().submitted == stats().processed`: every
  request that was accepted was processed. `cancel()` instead satisfies
  `processed + cancelled == submitted`, and cancelled work is never reported as
  success.
- After `stop()` returns, the governor's counters are at a valid baseline: the
  same counters a single-threaded run would have produced.
- The service can be started again; the counters accumulate across incarnations
  rather than silently resetting.
