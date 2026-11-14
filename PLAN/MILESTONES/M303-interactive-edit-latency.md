# M303 - Interactive Edit Latency

Make a downstream edit feel immediate: a point-op edit (Grade at the end of a ~24MP raw chain, fit-to-window) reaches the viewer in ≤33 ms on the user's host, with no draft. Draft (auto-proxy) becomes selective, adaptive and cache-friendly. Draft mode is on while scrubbing or dragging; auto-proxy (`autoProxyScrubbing`, default on; level "4", `Engine/Settings.cpp` ~:1749) then raises the mip (`Engine/ViewerInstance.cpp` ~:1172-1184). Draft is part of `ImageKey`, so a drag misses every cached upstream image and the release renders everything again. Separately, GPU placement and regions refuse non-scaling nodes at mip ≠ 0 (`Engine/GpuPlacement.cpp` ~:659, `Engine/GpuRegionTask.cpp` ~:820), so below 100% zoom the raw chain runs on the CPU at full resolution. The viewer picks the next finer power-of-two mip from zoom (`getMipmapLevelWithoutDraft`, ~:4372-4390), so 33% zoom renders at 50%.

## Scope

**In:**
- GPU regions run nodes without render-scale support at mip 0 and downscale on the device to the consumer's mip.
- Draft lookups reuse non-draft cache entries by downscaling them, so drafting never re-runs upstream.
- Point ops never draft: draft applies only when the node being edited isn't a point op.
- Adaptive draft: when measured draft renders predict a full-resolution render at ≤33 ms, render full resolution instead.
- A viewer draft toggle button with a keyboard shortcut.
- A mip-rounding preference: finer (default, today's behaviour) or nearest.
- The ≤33 ms gate on the user's host.

**Out:**
- Render-scale support inside the raw nodes; the mosaic makes it non-trivial, and good caching makes it rarely matter (user, 2026-11-01).
- Arbitrary, non-power-of-two render scales: `RenderScale` is power-of-two by type, and reworking it is a large change for little gain.
- The caching itself (M300 - Unified Tiered Cache, M301 - GPU Cache Tier).

**Edge cases:**
- **OFX nodes:** treated as not point ops unless they declare otherwise, so they still draft.
- **Several nodes edited at once** (a linked knob, an expression): draft applies if any edited node is not a point op.
- **The edited node is the viewer's root:** adaptive draft judges from that node's own measured cost.
- **Zoom above 100%:** auto-proxy's existing reduction per 2× of zoom is kept.

**Assumptions:**
- The ≤33 ms budget is reachable at the viewer's own mip once upstream is cached; if not, the nearest-mip preference or drafts close the gap, and the user decides which.

## Decisions

- 2026-11-01 — **Point ops never draft; draft is for transform-style nodes; adaptive full-res when the draft rate implies ~30 fps at full res; draft reuses the cache (user).**
- 2026-11-01 — **Mip rounding becomes a preference, default finer (user).**
- 2026-11-01 — **Raw render scale out (user);** instead, GPU regions downscale non-scaling nodes on the device (planner), so the raw chain stays on the GPU below 100% zoom.
- 2026-11-01 — **Budget ≤33 ms on the user's host (user).**
- 2026-11-01 — No existing milestone carried a draft-toggle shortcut, so it lands here.

## Phase 303.1: Keep the raw chain on the GPU at any zoom

- [ ] M303.P1.T1 — Run non-scaling nodes at mip 0 in GPU regions and downscale on the device
  - files: `Engine/GpuPlacement.cpp` (~:659), `Engine/GpuRegionTask.cpp` (~:557, ~:820), `Engine/Gpu/` downscale kernel, `Tests/GpuPlacement_Test.cpp`
  - approach: Placement accepts an `eSupportsNo` task at mip ≠ 0. The region renders it at mip 0, and a device box-downscale (matching `Image::downscaleMipmap`) produces the consumer's mip, so the CPU path's semantics (`EffectInstanceRenderRoI.cpp` ~:457) are mirrored. The mip-0 output is cached as its own entry.
  - verify: A GPU test of the raw chain at mip 1 stays on the GPU and matches the CPU result within the existing GPU parity tolerance.
  - size: L

## Phase 303.2: Selective, cache-friendly draft

- [ ] M303.P2.T1 — Serve draft lookups from non-draft entries
  - files: `Engine/ImageKey.cpp`, `Engine/EffectInstance.cpp` (lookup), `Engine/CacheDeviceTier.cpp`, `Tests/CacheStress_Test.cpp`
  - approach: A draft lookup that misses tries the non-draft key at the same or a finer mip and downscales the hit, on the host or the device. A node's draft entry is only written when the node itself rendered in draft.
  - verify: A test caches a non-draft upstream, then renders in draft, and the profile shows the upstream served from cache, not executed.
  - size: M

- [ ] M303.P2.T2 — Declare point ops and exempt them from draft
  - files: `Engine/Nodes/NodeSpec.h`, `Engine/EffectInstance.h`, `Engine/ViewerInstance.cpp` (~:1172), `Engine/Nodes/README.md`
  - approach: Add a `pointOp` trait to `NodeSpec` with an `EffectInstance::isPointOp()` accessor (OFX: false). Draft raises the mip only when a node being edited is not a point op. Set the trait on native point-op nodes (Grade, ColorCorrect, Saturation, Clamp, the colour utilities and the like) in their specs.
  - verify: A unit test dragging Grade renders at the viewer's own mip, while dragging Transform renders at the auto-proxy mip.
  - size: M

- [ ] M303.P2.T3 — Turn draft off when full resolution would keep up
  - files: `Engine/ViewerInstance.cpp`, `Engine/ViewerInstance.h`, `Tests/ViewerStaleState_Test.cpp`
  - approach: Keep a rolling estimate of the draft render time of the edited subgraph. Scale it by the full/draft pixel ratio; when that predicts ≤33 ms, render the next update at full resolution, with hysteresis so it doesn't toggle per frame.
  - verify: A unit test with a fake cost model switches to full resolution under a cheap cost and stays in draft under an expensive one, without oscillating.
  - size: M

- [ ] M303.P2.T4 — Add a viewer draft toggle and shortcut
  - files: `Gui/ViewerTab.cpp`, `Gui/ViewerTab10.cpp`, `Gui/ActionShortcuts.h`, `Gui/ActionShortcuts.cpp`, `Engine/Settings.cpp`
  - approach: A checkable viewer toolbar button bound to `autoProxyScrubbing`, with a registered, rebindable shortcut. The label matches the setting's readable name.
  - verify: A GUI script under Xvfb toggles the button via the shortcut and reads the setting; a screenshot goes to the user.
  - size: M

- [ ] M303.P2.T5 — Add the mip-rounding preference
  - files: `Engine/ViewerInstance.cpp` (`getMipmapLevelWithoutDraft` ~:4372-4390), `Engine/Settings.cpp`, `Engine/Settings.h`
  - approach: Preference `viewerMipRounding`: `finer` (default, `-ceil(log2 zoom)`) or `nearest` (round to nearest power of two).
  - verify: A unit test gives mip 1 at 33% zoom under `finer` and mip 2 under `nearest`.
  - size: M

## Phase 303.3: Gate

- [ ] M303.P3.T1 — Measure the budget on the user's host
  - files: `tools/bench/baselines/edit-latency.jsonl`, `tools/bench/README.md`
  - approach: Run M276 - Upstream Reuse On Edits' benchmark from a release AppImage on the user's host: Grade edits at fit-to-window and 100%, GPU on and off, and a Transform drag for the draft path. Compare against the baseline. If Grade misses ≤33 ms, report which stage dominates and put the nearest-mip default or drafts to the user.
  - verify: The results are recorded and compared against the M276 baseline.
  - size: M

**Verification gate:** cache, GPU and viewer suites pass. On the user's host with a release AppImage, a Grade edit at the end of the Canon raw chain at fit-to-window reaches the viewer in ≤33 ms median with the GPU on, with no upstream execution and no draft. A Transform drag drafts without re-running upstream. The user accepts it at UAT.
- 2026-11-14 — Claimed by bosley-native-nodes (user request).
