# M594 - Native EdgeDistort

A native **EdgeDistort** node, part of the edge-extend family (`PLAN/DECISIONS/2026-11-24-edge-extend-node-family.md`). It applies a single displacement warp that pulls pixels inward along the gradient of a blurred matte, a PxF_Distort-style "inflate". It is built on M238 - EdgePush And Edge Fill Kit's `EdgeFillKit`.

## Scope

**In:**
- **Kit stages:** Matte input and `matte`, Source Premultiplied and Premultiply Output (both default off), Erode, Edge Mask Layer. Threshold and Softness set the edge mask's `w`.
- **Displacement:** d = `amount` × ∇ Gaussian(matte, `blur`), pointing toward the solid side. Output p samples the straight source at p + d over the **whole image** (user decision), with IDistort's sampler and filter.
- **Knobs:** `amount` (default 10 px), `blur` (default 10 px), `filter`. Render-scaled.
- **Optional Vector input:** when connected, its UV (IDistort's channel convention, in pixels) replaces the gradient, scaled by `amount`.
- **Output alpha:** the source's.

**Out:**
- **Fill Extent:** not used; the RoI is local, the output RoD grown by `amount`.
- **Noise-driven distortion:** wire a noise into the Vector input instead.
- **Detail restoration:** M595 - Native EdgeDetail.

**Edge cases:**
- **Flat matte** (zero gradient): zero displacement, so identity. Deep in the core and far outside it.
- **Vector input disconnected:** use the gradient.
- **`amount` 0:** identity, bit-exact.

## Phase 594.1: CPU

- [ ] M594.P1.T1 — EdgeDistort node, gradient displacement and optional Vector input (CPU)
  - files: `Engine/Nodes/Filter/EdgeDistort.{h,cpp,reg.inc}` (new), `Engine/Nodes/Transform/IDistort.{h,cpp}` (reuse the sampler), CMake, `Tests/Native/NativeEdgeDistort_Test.cpp` (new)
  - approach: per the Scope. The RoI grows by `amount + blur radius`.
  - verify: `amount` 0 is identity; on a disc, pixels just outside sample from inside, displaced by the expected amount; Vector input overrides the gradient; flat regions are unchanged; a tiled render equals a whole-frame render bit-exact.
  - size: L

## Phase 594.2: GPU and sign-off

- [ ] M594.P2.T1 — EdgeDistort on the GPU
  - files: `Engine/Gpu/Kernels/edgedistort.{slang,cmake}` (new; imports `edgefillkit`, reuses the `idistort` sampler if a kernel exists, else the `stmap.slang` one), `Engine/Nodes/Filter/EdgeDistort.cpp`, `Tests/Native/NativeEdgeDistortGpu_Test.cpp` (new)
  - approach: blur, gradient and warp passes; resident, no CPU fallback; set gpuWeight.
  - verify: `ctest -R NativeEdgeDistortGpu` within 1e-4 relative on RADV, with and without the Vector input.
  - size: L
- [ ] M594.P2.T2 — Goldens, docs and GUI check
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/EdgeDistort/`, `Engine/Nodes/Filter/docs/EdgeDistort.md` (new), `Tests/gui/m594_edgedistort.py` (new)
  - approach: golden cases for a disc and the Vector input (≤256 px crops); a doc naming every knob, with an "extend" search tag; GUI check at 1920x1080 with screenshots.
  - verify: `ctest -R NativeGolden`; the GUI check passes on the remote runner.
  - size: M

**Verification gate:** remote `release --tests` green, including `NativeEdgeDistort*` and `NativeGolden`; GPU parity on RADV; the GUI check passes remotely; screenshots approved; an AppImage for UAT.

## Decisions

- 2026-11-24 — The warp applies to the whole image, not only the fill band (user): a PxF-style inflate, where the core's edge warps too.
