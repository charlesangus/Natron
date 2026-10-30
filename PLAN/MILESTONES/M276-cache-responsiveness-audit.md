# M276 - Cache Responsiveness Audit

Determine why an expensive Raw-correction chain feeding HSVKeyer appears to recompute upstream work and deliver slow interactive feedback even after M260 - Cache Invalidation Follow-ups, then fix the measured cache or scheduling cause without weakening correctness.

## Scope

**In:**
- A reproducible Raw-correction → HSVKeyer interactive benchmark.
- Cache hit/miss, hash-change and task-scheduling instrumentation sufficient to attribute repeated work.
- A targeted fix and regression budget for the confirmed cause.

**Out:**
- New cache architecture or eviction policy without evidence from the reproduction.

## Phase 276.1: Reproduce and attribute

- [ ] M276.P1.T1 — Build the interactive keyer cache benchmark
  - files: `tools/bench/`, synthetic Raw/keyer fixture, benchmark docs
  - approach: Reproduce the reported chain with a small deterministic source; measure first render, repeat render and HSVKeyer-only adjustments under both schedulers while recording upstream executions.
  - verify: Repeated runs emit timing and per-node execution counts in a machine-readable result.
  - size: M

- [ ] M276.P1.T2 — Trace unexpected invalidation or cache misses
  - files: cache/hash diagnostic hooks, benchmark analyser, focused tests
  - approach: Attribute every repeated upstream render to a hash change, missing region, eviction, action invalidation or scheduler dependency and record the smallest confirmed cause.
  - verify: The report accounts for every upstream execution in the reproduction and distinguishes expected from redundant work.
  - size: M

## Phase 276.2: Fix and guard

- [ ] M276.P2.T1 — Remove the confirmed redundant upstream work
  - files: exact cache/hash/scheduler area identified by Phase 276.1, focused regression test
  - approach: Apply the narrow fix supported by the trace while retaining M260's file-pinning and mixed-state protections.
  - verify: Correctness suites pass and the focused test fails before the fix and passes after it.
  - size: L

- [ ] M276.P2.T2 — Set the interactive repeat-render budget
  - files: cache benchmark baseline and comparison script
  - approach: Record the fixed host result and fail comparison when HSVKeyer-only edits re-execute the unchanged expensive upstream chain or exceed the accepted repeat-render budget.
  - verify: The benchmark passes on the target host and reports near-immediate repeat feedback with upstream cache hits.
  - size: M

**Verification gate:** cache correctness and stress suites pass; the new benchmark attributes no redundant upstream execution and meets the user-approved interactive latency budget on the target host.
