# M591 - Native EdgeClamp

A native **EdgeClamp** node, part of the edge-extend family (`PLAN/DECISIONS/2026-11-24-edge-extend-node-family.md`). Each replaced pixel takes the colour of the nearest solid pixel, a hard clamp-to-edge fill. The user judged this close enough to the slEdgeHammer gizmo. It is built on M238 - EdgePush And Edge Fill Kit's `EdgeFillKit`.

## Scope

**In:**
- **Kit stages:** Matte input and `matte`, Source Premultiplied and Premultiply Output (both default off), Threshold and Softness, Erode, Fill Extent, Edge Mask Layer.
- **Nearest-seed fill:** 1+JFA (jump flooding, Rong & Tan 2006) over the pixels with `w > 0`, taking the seed's straight colour. The same algorithm runs on CPU and GPU, so the two match.
- **`smoothing`** (px, default 0): a distance-scaled blur over the fill. At distance d it uses radius `min(smoothing, d)/√2`, which is Blender Inpaint's seam hiding.
- **Output:** `w·src + (1 − w)·fill`, with the source's alpha.

**Out:**
- **Exact distance transform:** CPU and GPU would differ where seeds tie, so it is out.
- **Detail restoration:** M595 - Native EdgeDetail.

**Edge cases:**
- **Nothing solid:** black, no error (the kit rule).
- **Ties between equidistant seeds:** decided the same way on CPU and GPU (lowest index), so parity holds.
- **RoI:** the whole fill extent; jump flooding is global.

## Phase 591.1: CPU

- [ ] M591.P1.T1 — EdgeClamp node with a jump-flood nearest fill (CPU)
  - files: `Engine/Nodes/Filter/EdgeClamp.{h,cpp,reg.inc}` (new), `Engine/Nodes/Filter/JumpFlood.{h,cpp}` (new, reusable), CMake, `Tests/Native/NativeEdgeClamp_Test.cpp` (new)
  - approach: kit knobs and weight, then a 1+JFA seed propagation (one pass at step 1, then steps N/2 … 1), copying the seed colour. Panel order: the kit order, with Smoothing after Erode.
  - verify: each replaced pixel gets the colour of its true nearest seed on synthetic seeds, within the 1+JFA error tested against brute force on small images; solid pixels unchanged; nothing solid gives black.
  - size: L
- [ ] M591.P1.T2 — Smoothing (CPU)
  - files: `Engine/Nodes/Filter/EdgeClamp.{h,cpp}`, `Tests/Native/NativeEdgeClamp_Test.cpp`
  - approach: a separable variable-radius Gaussian over the fill, with σ from `min(smoothing, d)/√2`, where d comes from the jump flood. 0 is skipped.
  - verify: 0 is bit-identical to no smoothing; a raised value removes the seam step between two seeds of different colour.
  - size: M

## Phase 591.2: GPU and sign-off

- [ ] M591.P2.T1 — EdgeClamp on the GPU
  - files: `Engine/Gpu/Kernels/edgeclamp.{slang,cmake}` (new; imports `edgefillkit`), `Engine/Nodes/Filter/EdgeClamp.cpp`, `Tests/Native/NativeEdgeClampGpu_Test.cpp` (new)
  - approach: jump-flood passes and the variable blur, all resident, no CPU fallback; set gpuWeight.
  - verify: `ctest -R NativeEdgeClampGpu` within 1e-4 relative on RADV, smoothing 0 and >0.
  - size: L
- [ ] M591.P2.T2 — Goldens, docs and GUI check
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/EdgeClamp/`, `Engine/Nodes/Filter/docs/EdgeClamp.md` (new), `Tests/gui/m591_edgeclamp.py` (new)
  - approach: golden cases for a hard clamp, smoothing, and erode (≤256 px crops); a doc naming every knob, with an "extend" search tag. GUI check at 1920x1080: the panel fits, with screenshots.
  - verify: `ctest -R NativeGolden`; the GUI check passes on the remote runner.
  - size: M

**Verification gate:** remote `release --tests` green, including `NativeEdgeClamp*` and `NativeGolden`; GPU parity on RADV; the GUI check passes remotely; screenshots approved; an AppImage for UAT.

## Decisions

- 2026-11-24 — Smoothing defaults to 0 (user): the default is a pure hard clamp, the slEdgeHammer look. Raising it gives Blender-style seam hiding.
