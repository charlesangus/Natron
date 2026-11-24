# M593 - Native EdgePushVector

A native **EdgePushVector** node, part of the edge-extend family (`PLAN/DECISIONS/2026-11-24-edge-extend-node-family.md`). It makes repeated short pushes of colour outward along the matte's normal (VectorExtendEdge-style). The pushes follow curved edges and keep strands and motion-blur streaks. It is built on M238 - EdgePush And Edge Fill Kit's `EdgeFillKit`.

## Scope

**In:**
- **Kit stages:** as for every family node.
- **Per iteration:**
  1. Normal: n = normalise(∇ Gaussian(w, `matteBlur`)), smoothed by `vectorBlur`.
  2. Push: each non-solid pixel averages colour along the segment toward the solid side, `length` px long, as VectorBlur does.
  3. Merge: the pushed result goes under the current solid colour, the weight grows by the push, and the next iteration re-derives the normal from the grown weight.
- **Knobs:** `length` (default 3 px), `iterations` (default 5), `matteBlur` (default 1), `vectorBlur` (default 1). Render-scaled.
- **Output:** `w·src + (1 − w)·fill`, with the source's alpha. Reach is `length × iterations`, and the RoI is local.

**Out:**
- **Fill Extent:** not used. The reach is local, so Fill Extent only bounds the output RoD.
- **Detail restoration:** M595 - Native EdgeDetail.

**Edge cases:**
- **Zero gradient** (thin gaps, concave corners): no push from that pixel in that iteration, never NaN.
- **Nothing solid:** black, no error.
- **Past the reach:** black.

**Assumptions:**
- Unconfirmed: the defaults mirror VectorExtendEdge (length 3, 5 iterations, pre-blur 1).

## Phase 593.1: CPU

- [ ] M593.P1.T1 — EdgePushVector node, normal and push iterations (CPU)
  - files: `Engine/Nodes/Filter/EdgePushVector.{h,cpp,reg.inc}` (new), `Engine/Nodes/Filter/VectorBlur.{h,cpp}` (reuse its line sampler), CMake, `Tests/Native/NativeEdgePushVector_Test.cpp` (new)
  - approach: kit weight, then iterate per the Scope. The RoI grows by `length × iterations + blur radii`.
  - verify: on a disc, colour reaches `length × iterations` and stops there; a red/blue split edge pushes each colour along its own normal; a zero-gradient pixel gives no NaN; a tiled render equals a whole-frame render bit-exact.
  - size: L

## Phase 593.2: GPU and sign-off

- [ ] M593.P2.T1 — EdgePushVector on the GPU
  - files: `Engine/Gpu/Kernels/edgepushvector.{slang,cmake}` (new; imports `edgefillkit`, reuses the `vectorblur.slang` sampler), `Engine/Nodes/Filter/EdgePushVector.cpp`, `Tests/Native/NativeEdgePushVectorGpu_Test.cpp` (new)
  - approach: per-iteration gradient, push and merge passes; resident, no CPU fallback; set gpuWeight.
  - verify: `ctest -R NativeEdgePushVectorGpu` within 1e-4 relative on RADV.
  - size: L
- [ ] M593.P2.T2 — Goldens, docs and GUI check
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/EdgePushVector/`, `Engine/Nodes/Filter/docs/EdgePushVector.md` (new), `Tests/gui/m593_edgepushvector.py` (new)
  - approach: golden cases for a disc, a curved edge and the iteration count (≤256 px crops); a doc naming every knob, with an "extend" search tag; GUI check at 1920x1080 with screenshots.
  - verify: `ctest -R NativeGolden`; the GUI check passes on the remote runner.
  - size: M

**Verification gate:** remote `release --tests` green, including `NativeEdgePushVector*` and `NativeGolden`; GPU parity on RADV; the GUI check passes remotely; screenshots approved; an AppImage for UAT.
