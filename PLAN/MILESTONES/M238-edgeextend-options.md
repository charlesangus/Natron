# M238 - EdgeExtend Options

Follow-up the user asked for when approving M123 - Native Sharpen And Glow's UAT (2026-10-26): improve the native EdgeExtend's options. Today it has Source Premultiplied, Threshold (0.5), Size, Slices and Premultiply Output (M123 round 1).

Reference: Nuke 17's EdgeExtend (`.local/docs/nuke/html/content/reference_guide/filter_nodes/edgeextend.html`): a **Matte** input and `matte` choice (None, Source Alpha, Source Inverted Alpha, Matte Luminance, Matte Inverted Luminance, Matte Alpha, Matte Inverted Alpha); **Erode** (negative dilates) on the matte before extending; **Detail Amount** (default 1) recovering high-frequency detail after the erode/dilate; **Output Edge Mask** to a chosen channel; a fill independent of the fill region's size (ours is slice-count/size bound).

Acceptance sketch:
- Each chosen option has CPU and GPU kernels with parity, native tests and goldens.
- The panel fits the 1920x1080 default layout; screenshots shared before sign-off.

## Scope

**In (pending Q1):** a `Matte` input and `matte` choice, `erode`, `detailAmount`, `edgeMaskChannel`, and a push-pull fill independent of the area it fills. Each with CPU and GPU kernels, parity and goldens. Panel order: Matte, Source Premultiplied, Threshold, Erode, Detail Amount, Edge Mask Channel, Premultiply Output.

**Out:** `size` and `slices` (replaced by push-pull, no compatibility; pending Q2). Writing the edge mask to another layer (LayerPolicy can't; would be an E14 stub). No (un)premult-by knob (filter).

**Edge cases:**
- `matte` = None: every pixel solid; colour passes through apart from the premultiply round trip.
- Matte input disconnected with a Matte option chosen: empty matte, identity, no error (EdgeBlur's rule).
- Erode past the whole matte: nothing solid, black (not NaN).
- Push-pull needs the whole frame: RoI = source RoD; pyramid ≈1.33× frame per channel; tiles bit-exact against a whole-frame render.
- Luminance uses the working-space luma coefficients of the native colour nodes.

**Assumptions:** Detail Amount adds back `src − Gaussian(src)` (Gaussian size max(|erode|, 1) px) only where colour was replaced; Nuke doesn't document the maths.

## Phase 238.1: Matte, erode and fill

- [ ] M238.P1.T1 — Matte input and `matte` choice (CPU)
  - files: `Engine/Nodes/Filter/EdgeExtend.{h,cpp}`, `Tests/Native/NativeEdgeExtend_Test.cpp`
  - approach: optional Aux "Matte" input read as EdgeBlur's (`EdgeBlur.h:52`, `Engine/Nodes/Filter/MapInput.h`); `matte` KnobChoice (IDs none, sourceAlpha (default), sourceInvertedAlpha, matteLuminance, matteInvertedLuminance, matteAlpha, matteInvertedAlpha) replaces source alpha in the threshold test; output alpha stays the source's.
  - verify: gtests for each option on a synthetic matte, disconnected-Matte identity, None identity.
  - size: M
- [ ] M238.P1.T2 — Erode (CPU)
  - files: `Engine/Nodes/Filter/EdgeExtend.{h,cpp}`, `Tests/Native/NativeEdgeExtend_Test.cpp`
  - approach: spatial `erode` (default 0; negative dilates) on the chosen matte before the threshold, IErodeDilate's fractional kernel, render-scaled; RoI grows by |erode|.
  - verify: erode shrinks, dilate grows, past the matte gives black, 0 is identity.
  - size: M
- [ ] M238.P1.T3 — Push-pull fill replaces the slices (CPU)
  - files: `Engine/Nodes/Filter/EdgeExtend.{h,cpp}`, `Engine/Nodes/Filter/BloomKernels.h`, `Tests/Native/NativeEdgeExtend_Test.cpp`
  - approach: remove size/slices/sliceSigmas/kMaxSlices; weighted pyramid of (colour·w, w) with [1 3 3 1] decimation (Bloom's helpers) down until no zero-weight pixel or 1 px; up with bicubic, merge under finer (`a + b(1 − αa)`), normalise; RoI = source RoD.
  - verify: solid pixels unchanged; a pixel 500 px from the matte gets the nearest edge colour; tiled == whole frame bit-exact.
  - size: L
- [ ] M238.P1.T4 — GPU path for matte, erode and push-pull
  - files: `Engine/Gpu/Kernels/edgeextend.{slang,cmake}`, `Engine/Nodes/Filter/EdgeExtend.cpp`, `Tests/Native/NativeEdgeExtendGpu_Test.cpp`
  - approach: matte-select, pyramid-down, pyramid-up/merge entries; erode via `ierodedilate.slang`; pyramid resident, no CPU fallback; update gpuWeight.
  - verify: `ctest -R NativeEdgeExtendGpu` within 1e-4 relative on RADV for every matte option and erode ±.
  - size: L

## Phase 238.2: Detail, edge mask and sign-off

- [ ] M238.P2.T1 — Detail Amount (CPU and GPU)
  - files: EdgeExtend sources, `edgeextend.slang`, both tests
  - approach: `detailAmount` (default 1, display 0–2) adds amount × (straight src − Gaussian(src)) where colour was replaced; record final maths in Decisions.
  - verify: 0 is a smooth fill; 1 restores noise variance in the replaced band within 10%; GPU parity 1e-4.
  - size: L
- [ ] M238.P2.T2 — Edge mask output (CPU and GPU)
  - files: EdgeExtend sources, `edgeextend.slang`, both tests
  - approach: `edgeMaskChannel` (none (default), red, green, blue, alpha; pending Q3) writes clamp(chosen matte − solid matte, 0, 1) into that output channel.
  - verify: mask on a known disc per channel; None unchanged; GPU parity.
  - size: M
- [ ] M238.P2.T3 — Goldens, description and docs
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/EdgeExtend/`, `EdgeExtend.h` description, `Engine/Nodes/Filter/docs/EdgeExtend.md`
  - approach: replace oneslice/wide cases with matte, erode, detail, edgemask; re-record (≤256 px crops); rewrite doc/description.
  - verify: `ctest -R NativeGolden`; every knob named in the doc.
  - size: M
- [ ] M238.P2.T4 — Xvfb GUI check
  - files: `Tests/gui/m238_edgeextend.py`
  - approach: create EdgeExtend at 1920x1080, connect a Matte, cycle options, erode ±, detail 0/1, edge mask on alpha; panel fits without scrolling; screenshots.
  - verify: passes on the remote GUI runner.
  - size: M

**Verification gate:** remote `release --tests` green incl. `NativeEdgeExtend*` and `NativeGolden`; GPU parity on RADV; GUI check passes remotely; screenshots approved; AppImage for UAT.

## Open questions (for the user)
- Q1 Which options do you want? Recommendation: all five.
- Q2 Should push-pull replace Size and Slices outright? Recommendation: yes, no compatibility (no reach limit).
- Q3 Where should the edge mask go? Recommendation: an R/G/B/A channel of the output now; a separate layer needs an E14 stub.

## Decisions

- 2026-10-28: elaborated (consultant); blocked on Q1–Q3 before implementation.
