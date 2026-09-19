# Systems boundary

Elephant Flow Governor owns exactly one decision surface:

> Given authoritative flow-volume, duration, rate, path, service-class, capacity
> and contention evidence, which flows qualify as elephants **now**, what impact
> are they creating, which bounded governance action is **authorized**, and when
> must that classification or action be revoked, fenced or revalidated?

## What this runtime owns

| Owned | Where |
| --- | --- |
| Flow identity and generation | `efg::FlowId`, `efg::Generation`, `efg/identity.hpp` |
| Evidence windows and derived measurements | `efg::FlowLedger`, `efg::FlowMeasurements` |
| Elephant classification and hysteresis | `efg::Governor`, `efg/classification.hpp` |
| Impact assessment | `efg::compute_impact`, `efg/impact.hpp` |
| Bounded governance intent | `efg::GovernanceIntent`, `efg::IntentLedger` |
| Explanation and authority vectors | `efg::Explanation`, `efg/explain.hpp` |
| Durable classification history and restart policy | `efg::Journal`, `efg::DurableGovernor` |
| Multi-publisher epoch and boot fencing | `efg::Coordinator`, `efg::Worker` |

## What this runtime explicitly does not own

These are adjacent systems. Nothing in this repository implements them, and no
API in it performs them:

- **generic flow classification** — the governor classifies one question only:
  whether a flow is an elephant under a supplied policy. It is not a general
  classifier, an application identifier or an anomaly detector;
- **path placement** — `RequestAlternatePlacement` is a request addressed to a
  placement system. The governor never chooses or installs a path;
- **flow scheduling** — `RequestSchedulingIsolation` states an intent. The
  governor never programs a scheduler;
- **rate enforcement** — `RequestRateShaping` carries a bounded reduction in
  basis points. The governor never shapes, drops, marks or polices a packet;
- **congestion synthesis** — `EscalateCongestion` is an escalation, not a
  congestion-control algorithm;
- **admission** — `ReduceCompetingAdmission` is a bounded request. The governor
  never admits or refuses a flow;
- **reservations** — the governor reads reservation references and protects
  them. It never creates, sizes or tears down a reservation;
- **pacing execution** — no timers, no token buckets, no transmit scheduling;
- **forwarding** — the runtime never touches a packet, a queue or a port.

## The intent contract

An intent is a statement, not an action:

- it names the exact flow generation, classification identifier, classification
  generation, path generation, capacity generation, policy generation, epoch and
  boot it rests on (the authority vector);
- it carries a *requested* envelope and a *granted* envelope, and the granted
  envelope is never wider than the policy ceiling;
- it has a validity window and expires;
- it is revoked when the classification changes and fenced when a generation,
  epoch or boot moves.

An actuator in an adjacent system is free to honour an intent, to ignore it, or
to be absent entirely. The governor's correctness does not depend on any of those
choices: a decision is complete and explainable the moment it is published.
