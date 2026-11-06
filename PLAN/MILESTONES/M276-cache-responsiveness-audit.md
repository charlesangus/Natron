# M276 - Upstream Reuse On Edits

Measure why a downstream edit re-renders unchanged upstream work, and record the baseline the cache redesign is judged against. The reported cases: a ~24MP camera RAW → raw develop chain → Grade → Viewer, where dragging Grade is far from interactive and unchanged upstream nodes light up as if rendering; and the earlier Raw-correction → HSVKeyer chain. A code read on main (cd383143f) found the likely causes, but none were measured: GPU region outputs reach only the per-frame `FrameStore` (`Engine/GpuRegionTask.cpp` `storeOutputs` ~:2853) and never a cache; `Node::shouldCacheOutput` (`Engine/Node.cpp` ~:8225) skips nodes in a linear chain unless a panel is open; auto-proxy's draft key and mip switch at drag start and release; and `GpuPlacement` `isCached` (~:925) probing only the non-draft key. This milestone instruments, attributes and baselines. The structural fixes are M300 - Unified Tiered Cache and M301 - GPU Cache Tier.

## Scope

**In:**
- Cache hit/miss and executed-device fields in the `NATRON_RENDER_PROFILE` records.
- An edit-latency benchmark: knob edits on the last node of a RAW chain (Grade) and of the Raw → HSVKeyer chain, timed from knob change to the viewer showing the new image, with per-node execution counts.
- An attribution report that classifies every upstream execution.
- The placement `isCached` draft-key bug, because it is independent of the redesign.
- The baseline the later milestones' gates compare against.

**Out:**
- The cache redesign (M300 - Unified Tiered Cache) and device caching (M301 - GPU Cache Tier).
- The viewer stale cue and the node-highlight semantics (M302 - Viewer Render State Cues).
- Draft and auto-proxy behaviour, and the ≤33 ms gate (M303 - Interactive Edit Latency).
- Render-scale support in the raw nodes (no milestone; low priority, user 2026-11-01).

**Edge cases:**
- Below 100% zoom, the raw nodes (`eSupportsNo`) are refused by GPU placement (`GpuPlacement.cpp` ~:659), so the chain runs on the CPU at full resolution. The benchmark covers fit-to-window and 100% so both paths show.
- With the settings panel of the edited node open, today's rule caches its inputs. The benchmark runs with the panel open and with it closed.

**Assumptions:**
- The Canon 5D Mk III fixture (`Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2`, ~22MP) is close enough to the user's 24MP file to reproduce the problem.

## Decisions

- 2026-11-06 — Attribution on the software host (16 configs, GPU off): panel closed, 50 executions per config, `not_admitted`; panel open, 40, the new `behind_hit`: the task graph runs a task's dependencies without asking whether its consumer is cached, so `RawInputColor` hits while the four raw nodes above it run. Hashes never moved. M300 - Unified Tiered Cache's gate needs `behind_hit` gone and none of its tasks covers it: `/cat-plan` should add a pruning task there (recommended). `gpu_region_member`, `placement_iscached_miss`, eviction and the drag's draft switch are unmeasured: no region formed under lavapipe.
- 2026-11-01 — **Broadened from "Cache Responsiveness Audit" (user):** the RAW → Grade report and the GPU-caching question fold in here. The fixes move to M300 - Unified Tiered Cache and M301 - GPU Cache Tier, so this milestone measures and baselines, and fixes only the independent placement-key bug.
- 2026-11-05 — Freshness check: M276.P2.T1 rebriefed; the lambda's location moved and the test needs a real app and cache.
- 2026-11-01 — **The benchmark uses existing raw fixtures (user):** no new camera raws (LFS quota). The latency gate is judged on the user's host with a release AppImage.

## Phase 276.1: Instrument and reproduce

- [x] M276.P1.T1 — Record cache hits and the executing device in the render profile
  - files: `Engine/RenderScheduler.cpp` (profile writer ~:185), `Engine/GpuRegionTask.cpp` (region record), `tools/bench/render-profile-report.py`, `tools/bench/render-profile-sample.jsonl`
  - approach: Add `cache` (`hit` | `partial` | `miss` | `bypass`) and `device` (`cpu` | `gpu`) to each task record. Each region record lists the members that executed, as opposed to those served from a store. Determine the hit state where `renderRoI` finds `rectsToRender` empty or non-empty (`EffectInstanceRenderRoI.cpp` ~:1885). Teach the report script to print per-node counts of hits and executions.
  - verify: A unit or script test renders a two-node graph twice and the second profile shows the upstream task as `hit`. The report script prints per-node hit/execute counts on the updated sample.
  - size: M

- [x] M276.P1.T2 — Build the edit-latency benchmark
  - files: `tools/bench/edit_latency_bench.py` (new), `tools/bench/edit_latency.sh` (new), `tools/bench/README.md`
  - approach: A GUI script run under Xvfb (recipe: `build/deeprepro/run-gui.sh`; pre-seed `checkForUpdates=false`). It builds Read(Canon fixture) → the raw develop chain used by `tools/bench/raw_large_bench.sh` → Grade → Viewer, plus a Raw chain → HSVKeyer variant. It sets the viewer to fit-to-window or 100%, then applies N successive value changes to the last node's knob. Per edit it records the latency from knob set to completion of the viewer's root task (from the profile) and the per-node executions. Switches: `NATRON_GPU` on/off, auto-proxy on/off, the edited node's panel open/closed. Output is JSON lines.
  - verify: A run against a release AppImage writes one JSON line per edit per configuration, with latency and per-node execution counts.
  - size: M

- [x] M276.P1.T3 — Attribute every upstream execution
  - files: `docs/perf/interactive-edit-attribution.md` (new), `tools/bench/edit_latency_bench.py` (classifier)
  - approach: Run the benchmark's configuration matrix. Classify each upstream execution by cause: not admitted (`shouldCacheOutput` false), GPU region member (no cache), mip or draft key change, placement `isCached` miss, eviction, or genuine hash change. Each cause cites the code path. State which later milestone owns each cause.
  - verify: The report accounts for every upstream execution in every configuration, with none left unclassified.
  - size: M

## Phase 276.2: Independent fix and baseline

- [x] M276.P2.T1 — Placement probes the key the render will use
  - files: `Engine/GpuPlacement.cpp` (`isCached` lambda ~:1023-1062), `Tests/Native/ViewerRootOnDevice_Test.cpp`
  - approach: the placement `isCached` lambda probes only the non-draft key, while `renderRoI` (`EffectInstanceRenderRoI.cpp:902-914, 973`) probes the draft key first. Capture the frame's draft mode in the lambda and probe the draft key first when `node->isDraftModeUsed()`, so it matches the lookup `renderRoI` will make.
  - verify: an app-plus-cache test (`GpuPlacement_Test` uses fake inputs, so it can't show this) with a cached draft image keeps the node on the CPU. It fails before the fix and passes after.
  - size: M

- [ ] M276.P2.T2 — Record the edit-latency baseline
  - files: `tools/bench/baselines/edit-latency.jsonl` (new), `tools/bench/compare.py` (edit-latency mode), `tools/bench/README.md`
  - approach: Run the benchmark on the user's host with the release AppImage built from this branch. Commit the results as the baseline. `compare.py` reports per-configuration median and p95 latency and upstream execution counts against it.
  - verify: `compare.py` run on a fresh benchmark run against the committed baseline prints the per-configuration deltas.
  - size: M

**Verification gate:** cache and placement suites pass. The profile carries hit/miss and device fields. The attribution report accounts for every upstream execution. The baseline is recorded from the user's host.
