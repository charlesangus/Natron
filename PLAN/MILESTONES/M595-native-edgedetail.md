# M595 - Native EdgeDetail

A native **EdgeDetail** node, the edge-extend family's detail restoration (`PLAN/DECISIONS/2026-11-24-edge-extend-node-family.md`). Detail restoration is a separate node, not knobs on the fill nodes (user decision). It adds the original's high frequency back into the extended edge: `out = src + amount × (orig − Gaussian(orig, size)) × mask`, in straight colour.

## Scope

**In:**
- **Inputs:** Source (the extended image), Original (the plate before extension), and an optional Mask.
- **Mask:** a `maskSource` choice:
  - Source Layer (default): reads the layer named by `maskLayer` from Source. `maskLayer` defaults to `edge.mask`, the fill nodes' edge mask.
  - Mask Alpha or Mask Luminance: read from the Mask input.
- **Knobs:** `amount` (default 1, display 0–2), `size` (default 2 px, render-scaled), Source Premultiplied and Original Premultiplied (both default off), Premultiply Output (default off).
- **Output alpha:** Source's.
- Detail is computed on straight Original.

**Out:**
- **Other restore styles:** multiplicative frequency separation, dark/bright-only restore, luma-only restore, and Inpaint-style cloned detail with an offset (the user chose additive only).
- **Regrain:** downstream grain nodes.

**Edge cases:**
- **Original disconnected:** identity.
- **Mask Alpha/Luminance chosen with the Mask input disconnected:** identity.
- **Source Layer chosen but the layer is missing on Source:** identity, no error.
- **`amount` 0:** identity, bit-exact.
- **Original and Source RoDs differ:** detail outside Original's RoD is 0.

**Assumptions:**
- Unconfirmed: `size` defaults to 2 px.

## Phase 595.1: CPU

- [ ] M595.P1.T1 — EdgeDetail node (CPU)
  - files: `Engine/Nodes/Filter/EdgeDetail.{h,cpp,reg.inc}` (new), `Engine/Nodes/Filter/EdgeFillKit.{h,cpp}` (reuse the premultiply helpers and the luminance path), CMake, `Tests/Native/NativeEdgeDetail_Test.cpp` (new)
  - approach: per the Scope. The Gaussian is Blur's FIR Gaussian. The Source-layer read uses the same layer resolution as Shuffle's inputs. The RoI on Original grows by the blur radius.
  - verify: `amount` 0 is identity; on noise inside the mask, the restored band's variance is within 10% of Original's; each disconnected or missing-mask case is identity; the edge.mask layer written by EdgePush is picked up with no extra wiring.
  - size: M

## Phase 595.2: GPU and sign-off

- [ ] M595.P2.T1 — EdgeDetail on the GPU
  - files: `Engine/Gpu/Kernels/edgedetail.{slang,cmake}` (new), `Engine/Nodes/Filter/EdgeDetail.cpp`, `Tests/Native/NativeEdgeDetailGpu_Test.cpp` (new)
  - approach: one blur and one combine pass; resident, no CPU fallback; set gpuWeight.
  - verify: `ctest -R NativeEdgeDetailGpu` within 1e-4 relative on RADV for each `maskSource`.
  - size: M
- [ ] M595.P2.T2 — Goldens, docs and GUI check
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/EdgeDetail/`, `Engine/Nodes/Filter/docs/EdgeDetail.md` (new), `Tests/gui/m595_edgedetail.py` (new)
  - approach: a golden of EdgePush → EdgeDetail on a noisy keyed disc (≤256 px crops); a doc naming every knob, with a "detail" and "extend" search tag; GUI check at 1920x1080: wire EdgePush (edge mask on) into EdgeDetail, with screenshots.
  - verify: `ctest -R NativeGolden`; the GUI check passes on the remote runner.
  - size: M

**Verification gate:** remote `release --tests` green, including `NativeEdgeDetail*` and `NativeGolden`; GPU parity on RADV; the GUI check passes remotely; screenshots approved; an AppImage for UAT.

## Decisions

- 2026-11-24 — Separate node, additive high-pass with Amount and Size (user).
- 2026-11-24 — The mask defaults to the Source layer `edge.mask`, with a Mask input override (user), so Source + Original is the only wiring needed.
