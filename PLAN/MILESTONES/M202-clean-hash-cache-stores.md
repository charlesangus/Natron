# M202 - Clean-Hash Cache Stores

Stop the identity, actions (RoD and others) and image caches storing an entry under a hash computed over a still-dirty input, and stop `NativeEffectBase::getOutputMetadata` caching key/value metadata under a hash that went A→B→A during a derive. M167 - Per-Frame Node Metadata fixed this for the metadata cache only (886c0baf1) and logged the rest as follow-ups. Moved from M31 - Architectural Cleanup (cancelled 2026-10-25). Claimable once M167 is done; no extra go-ahead needed.

## Scope

**In:**
- `getHashValueIfUpToDate` gating (or the equivalent) on every identity, actions and image cache store site.
- A mark-count or write-gate check in `NativeEffectBase::getOutputMetadata`.

**Out:**
- Edits during non-viewer renders, which M177 - Renders Isolated From Edits owns.

## Phase 202.1: Gate cache stores on a clean hash

- [ ] M202.P1.T1 — Stop the identity, actions and image caches storing under a hash computed over a dirty input
  - files: `Engine/Node.cpp` (`refreshDirtyHashes`, `getHashValueIfUpToDate` ~:817), the identity, actions (RoD etc.) and image cache store sites, `Engine/Nodes/NativeEffectBase.cpp` (`getOutputMetadata`)
  - approach: `Node::refreshDirtyHashes` can return a hash computed over a still-dirty input. Since 886c0baf1 such a node stays dirty, but only the metadata cache checks `getHashValueIfUpToDate` before storing. Gate the identity, actions and image cache stores the same way. Give `NativeEffectBase::getOutputMetadata` (key/value metadata) the mark-count or write-gate check, so a hash that goes A→B→A during a derive cannot leave a stale entry.
  - verify: a test edits an upstream knob concurrently with renders and asserts that no cache holds an entry under a hash whose node was dirty at store time; an A→B→A edit during a metadata derive leaves no stale key/value entry.
  - size: L

**Verification gate:** M202.P1.T1's verify holds with `--gtest_repeat=200` on the new test and `ConcurrentQueriesCacheOnlyConsistentAnswers`; whole ctest green in the PR CI debug build (asserts live).

## Decisions

- 2026-10-25 — **Freshness check (bosley-native-nodes):** references re-verified against the base branch; corrections in `PLAN/DESIGN/2026-10-25-freshness-m122-m192-m202-m178-m185-m20.md` override stale paths, lines and APIs in this file. M202 lands before M178, which rebases onto it.
- 2026-10-25 — **Own milestone after M167 rather than a change-request to it:** this keeps M167's diff bounded (user, at M31 triage).
