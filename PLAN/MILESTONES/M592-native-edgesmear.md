# M592 - Native EdgeSmear

A native **EdgeSmear** node, part of the edge-extend family (`PLAN/DECISIONS/2026-11-24-edge-extend-node-family.md`). It push-pulls an identity UV map instead of colour, then resamples the source through the result. Replaced pixels become real moved pixels, so grain and texture survive (jkUVEdgeExtend-style). It is built on M238 - EdgePush And Edge Fill Kit's `EdgeFillKit` and EdgePush's pyramid.

## Scope

**In:**
- **Kit stages:** as for every family node.
- **UV fill:** EdgePush's weighted pyramid over (x·w, y·w, w), which gives a smooth source-coordinate field.
- **Resample:** EdgePush's colour fill is resampled at that field with STMap's filter, so a coordinate that lands on a non-solid pixel reads the smooth fill instead of contaminated colour.
- **`filter`:** STMap's filter choice and default.
- **Output:** `w·src + (1 − w)·smear`, with the source's alpha.

**Out:**
- **Detail restoration:** M595 - Native EdgeDetail. Smear keeps grain anyway.
- **Separate X and Y reach:** a jkUVEdgeExtend control, not taken.

**Edge cases:**
- **Nothing solid:** black, no error.
- **Coordinates outside the source RoD:** clamped to the fill extent before sampling.
- **RoI:** the whole fill extent.

**Assumptions:**
- Unconfirmed: reusing STMap's filter choice and its default is enough.

## Phase 592.1: CPU

- [ ] M592.P1.T1 — EdgeSmear node, UV push-pull and resample (CPU)
  - files: `Engine/Nodes/Filter/EdgeSmear.{h,cpp,reg.inc}` (new), `Engine/Nodes/Filter/EdgePush.{h,cpp}` (factor the pyramid into a function over N weighted channels if T4 of M238 left it colour-only), `Engine/Nodes/Transform/STMap.{h,cpp}` (reuse the sampler), CMake, `Tests/Native/NativeEdgeSmear_Test.cpp` (new)
  - approach: kit weight, then the pyramid over (x, y) weighted by w, then sample the EdgePush colour fill at the normalised (x, y) with STMap's filter.
  - verify: solid pixels unchanged; a noise texture inside a disc keeps its variance in the smeared band within 10% (a colour push-pull loses it); nothing solid gives black; a tiled render equals a whole-frame render bit-exact.
  - size: L

## Phase 592.2: GPU and sign-off

- [ ] M592.P2.T1 — EdgeSmear on the GPU
  - files: `Engine/Gpu/Kernels/edgesmear.{slang,cmake}` (new; imports `edgefillkit` and reuses `edgepush` entries), `Engine/Gpu/Kernels/stmap.slang` (sampler reuse), `Engine/Nodes/Filter/EdgeSmear.cpp`, `Tests/Native/NativeEdgeSmearGpu_Test.cpp` (new)
  - approach: run the pyramid on the coordinates, then one resample pass; resident, no CPU fallback; set gpuWeight.
  - verify: `ctest -R NativeEdgeSmearGpu` within 1e-4 relative on RADV for each filter.
  - size: L
- [ ] M592.P2.T2 — Goldens, docs and GUI check
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/EdgeSmear/`, `Engine/Nodes/Filter/docs/EdgeSmear.md` (new), `Tests/gui/m592_edgesmear.py` (new)
  - approach: golden cases for a textured disc and erode (≤256 px crops); a doc naming every knob, with an "extend" search tag; GUI check at 1920x1080 with screenshots.
  - verify: `ctest -R NativeGolden`; the GUI check passes on the remote runner.
  - size: M

**Verification gate:** remote `release --tests` green, including `NativeEdgeSmear*` and `NativeGolden`; GPU parity on RADV; the GUI check passes remotely; screenshots approved; an AppImage for UAT.
