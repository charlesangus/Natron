# M183 - Loop Map Blend

The universal I-mode (user, 2026-10-24): LoopEnd's **Map blend** mode. LoopEnd gains a Map input; each output pixel takes its Map value m, finds the two iterations whose `loop.t` bracket m, and blends them linearly. With knobs driven from `loop.t`, Map black gives the start values and white the end values, so any subgraph — a Blur, a Grade, an OFX plug-in — becomes an I-node. Rendering is data-dependent from the start (user): LoopEnd's probe task renders the Map tile, finds its min and max, and requests only the iterations that range spans, through M179 - Render Graph Continuation. A tile whose Map is all 0 renders one iteration; a uniform-gradient tile renders all of them. The Map input follows the shared convention from M184 - Variable-Size Filter Kit.

## Scope

**In:**
- `mode = Map blend` on LoopEnd with a Map input (M184's Map convention: layer/channel, invert, black and white points).
- Per-tile culling through the continuation API, plus the serial-pull fallback on the legacy path.
- An `interpolation` knob: Linear (default, two nearest iterations), Cubic (four nearest, hides banding on non-linear bodies) and Nearest (for debugging banding).
- CPU and GPU blend kernels.

**Out:**
- The exact per-pixel paths for nodes where blending ghosts (Transform, CornerPin) or rings (Defocus): the native I-nodes in M185 - Variable Warps and M186 - Exact IDefocus. Map blend does not detect matrix-exporting nodes itself; M189 - Make I-Node Action steers users to the exact variants.
- The Make I-Node action: M189 - Make I-Node Action.

**Edge cases:**
- Map values outside [0,1] after the black/white remap are clamped.
- A missing Map input: LoopEnd uses Map = 0 everywhere and warns.
- The Map's RoD is smaller than the output: outside it, Map = 0.
- Iterations 1: Map blend is the single iteration everywhere.
- Iterations are spaced evenly in `t`; a driver's easing curve shapes the value spacing, so there is no spacing knob.
- Banding: with too few iterations, a non-linear body (blur) shows steps between levels; the doc recommends a count and the node shows the level spacing in its panel.

## Phase 183.1: Map blend

- [ ] M183.P1.T1 — Add the Map input and Map blend mode to LoopEnd (full render)
  - files: `Engine/Nodes/Loop/LoopEnd.{h,cpp}`, `Engine/Nodes/Loop/LoopMapBlend.{h,cpp}` (new)
  - approach: Declare the Map input with M184's `MapInput` helper. In Map blend mode, request every iteration (no culling yet) and blend per pixel: `k = clamp(floor(m·(n−1)), 0, n−2)`, `w = m·(n−1) − k`, output `lerp(iter_k, iter_{k+1}, w)`; Cubic uses Catmull-Rom weights over iterations k−1..k+2, clamped at the ends; Nearest picks `round`.
  - verify: at the phase build, `ctest -R NativeLoopMapBlend` (T3) passes.
  - size: M

- [ ] M183.P1.T2 — Cull iterations per tile with the continuation API
  - files: `Engine/Nodes/Loop/LoopEnd.cpp`, `Engine/Nodes/Loop/LoopMapBlend.cpp`
  - approach: Opt LoopEnd into M179's continuation: the probe renders the Map for the tile, computes min/max, and extends the frame with only iterations `floor(min·(n−1))` … `ceil(max·(n−1))` (widened by one each side for Cubic) for that tile's RoI; the continuation blends. On the legacy path the same probe pulls serially.
  - verify: gtest with a Map that is 0 on the left half and a ramp on the right: the body renders 1 iteration for left tiles and all for right tiles (per-tile counters), and the image equals T1's full render bit for bit.
  - size: L

- [ ] M183.P1.T3 — Test Map blend on CPU
  - files: `Tests/Native/NativeLoopMapBlend_Test.cpp`
  - approach: A Grade gain driven 0→1 with Map = constant 0.5 equals gain 0.5 exactly (linear body). A Blur driven 0→20 with a half-black Map leaves the black half bit-identical to the input. Invert and black/white points remap as specified. Missing Map warns and uses 0.
  - verify: at the phase build, `ctest -R NativeLoopMapBlend` passes.
  - size: M

- [ ] M183.P1.T4 — Map blend on the GPU
  - files: `Engine/Gpu/Kernels/loopmapblend.slang`, `Engine/Gpu/Kernels/loopmapblend.cmake`, `Engine/Nodes/Loop/LoopMapBlend.cpp`, `Tests/Native/NativeLoopMapBlendGpu_Test.cpp`
  - approach: A point kernel over the two bracketing iterations per pixel; iterations for a tile are bound as an array of resident buffers.
  - verify: at the phase build, `ctest -R NativeLoopMapBlendGpu` passes on lavapipe within 1e-6 relative.
  - size: M

- [ ] M183.P1.T5 — Document Map blend
  - files: `Engine/Nodes/Loop/docs/LoopEnd.md`
  - approach: The I-mode recipe, choosing an iteration count, culling behaviour, and when to use the exact native I-nodes instead.
  - verify: reviewed at the gate.
  - size: S

## Decisions

- 2026-10-25 — **Linear, Cubic and Nearest (user).**
- 2026-10-25 — **Even spacing in `t`:** the driver's easing shapes value spacing, so no second spacing control.

**Verification gate:** the full build and ctest pass, including `NativeLoopMapBlend` and its GPU suite; a `tools/bench` case with a half-black Map shows the culled render taking at most 60% of the unculled one. Under Xvfb an eye-blur (Blur driven by a roto Map) and a Map-driven Grade are rendered; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
