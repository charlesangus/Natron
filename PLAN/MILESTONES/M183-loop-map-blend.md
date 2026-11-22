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

- [x] M183.P1.T1 — Add the Map input and Map blend mode to LoopEnd (full render)
  - files: `Engine/Nodes/Loop/LoopEnd.{h,cpp}`, `Engine/Nodes/Loop/LoopMapBlend.{h,cpp}` (new)
  - approach: Declare the Map input with M184's `MapInput` helper. In Map blend mode, request every iteration (no culling yet) and blend per pixel: `k = clamp(floor(m·(n−1)), 0, n−2)`, `w = m·(n−1) − k`, output `lerp(iter_k, iter_{k+1}, w)`; Cubic uses Catmull-Rom weights over iterations k−1..k+2, clamped at the ends; Nearest picks `round`.
  - verify: at the phase build, `ctest -R NativeLoopMapBlend` (T3) passes.
  - size: M

- [x] M183.P1.T2 — Cull iterations per tile with the continuation API
  - files: `Engine/Nodes/Loop/LoopEnd.cpp`, `Engine/Nodes/Loop/LoopMapBlend.cpp`
  - approach: Opt LoopEnd into M179's continuation: the probe renders the Map for the tile, computes min/max, and extends the frame with only iterations `floor(min·(n−1))` … `ceil(max·(n−1))` (widened by one each side for Cubic) for that tile's RoI; the continuation blends. On the legacy path the same probe pulls serially.
  - verify: gtest with a Map that is 0 on the left half and a ramp on the right: the body renders 1 iteration for left tiles and all for right tiles (per-tile counters), and the image equals T1's full render bit for bit.
  - size: L

- [x] M183.P1.T3 — Test Map blend on CPU
  - files: `Tests/Native/NativeLoopMapBlend_Test.cpp`
  - approach: A Grade gain driven 0→1 with Map = constant 0.5 equals gain 0.5 exactly (linear body). A Blur driven 0→20 with a half-black Map leaves the black half bit-identical to the input. Invert and black/white points remap as specified. Missing Map warns and uses 0.
  - verify: at the phase build, `ctest -R NativeLoopMapBlend` passes.
  - size: M

- [x] M183.P1.T4 — Map blend on the GPU
  - files: `Engine/Gpu/Kernels/loopmapblend.slang`, `Engine/Gpu/Kernels/loopmapblend.cmake`, `Engine/Nodes/Loop/LoopMapBlend.cpp`, `Tests/Native/NativeLoopMapBlendGpu_Test.cpp`
  - approach: A point kernel over the two bracketing iterations per pixel; iterations for a tile are bound as an array of resident buffers.
  - verify: at the phase build, `ctest -R NativeLoopMapBlendGpu` passes on lavapipe within 1e-6 relative.
  - size: M

- [x] M183.P1.T5 — Document Map blend
  - files: `Engine/Nodes/Loop/docs/LoopEnd.md`
  - approach: The I-mode recipe, choosing an iteration count, culling behaviour, and when to use the exact native I-nodes instead.
  - verify: reviewed at the gate.
  - size: S

## Decisions

- 2026-10-25 — **Linear, Cubic and Nearest (user).**
- 2026-10-25 — **Even spacing in `t`:** the driver's easing shapes value spacing, so no second spacing control.

**Verification gate:** the full build and ctest pass, including `NativeLoopMapBlend` and its GPU suite; a `tools/bench` case with a half-black Map shows the culled render taking at most 60% of the unculled one. Under Xvfb an eye-blur (Blur driven by a roto Map) and a Map-driven Grade are rendered; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
- 2026-11-17 — T1, T2, T3, T5 and the bench in b740eb862 (CPU); gate running. The probe is `getRenderContinuation`; iterations are pulled through `pullRenderContinuationRequest`, which also covers the legacy scheduler. M184's MapInput has Invert and Gamma, not black/white points, so the Map uses those.
- 2026-11-17 — **Continuation on GPU (user):** GPU placement kept every node offering a render continuation on the CPU, so T4's kernel could never run. Placement is extended so continuation nodes run on the GPU (probe, then the blend kernel over GPU-resident iteration tiles); this also serves later continuation nodes such as M153's GPU phase.
- 2026-11-17 — T4 in e37a66f01: `supportsRenderContinuationOnGpu()` opts a node in; a continuation task starts its own GPU region and two never merge; the probe runs on the CPU before the region (a GPU-rendered Map is downloaded once); pulled iterations reach `renderGpu` as GPU images. The kernel adds one iteration per dispatch (the fold kernel's pattern; no kernel binds buffer arrays). Pulled iterations still render on the CPU and are uploaded; putting the iteration graphs on the GPU is the next step in this lane after the gate.
- 2026-11-17 — Iteration graphs on the GPU in 5c0d2f0cb: `placeExtensionGraph` places a pulled iteration graph with the normal weight rule and per-node CPU fallback; extension roots read by the waiting region stay on the device; device loss in an extension restarts the frame on the CPU. Task-graph scheduler only: the legacy scheduler has no GPU regions, so its pulls stay on the CPU. Gate test fix c7a246989 (a cubic tap test value sat exactly on an iteration). Gate running.
- 2026-11-20 — The culling bench was polluted: the persistent disk tier served renders across runs, and each process reused the same colour nudges. Fixed in aafa1d3e4 (disk tier off, a fresh base colour per process, a one-frame range). The same commit parallelises `LoopMapBlend::accumulate` over row bands; pixel order is unchanged. Honest ratio at 16 iterations: 0.57 at 1 thread and about 0.63 at 8, held up by a serial Write and the whole-frame iteration 0. 32 iterations gives 0.59. **The bench defaults to 32 iterations (user):** the 60% gate is judged at 32 iterations, at 1 and 8 threads, on a quiet host. Not changed: tools/bench/reformat_filter_bench.py probably has the same disk-tier exposure.
- 2026-11-20 — The m183 GUI failure after aafa1d3e4 was not the threading. The float output is identical, because each pixel is summed by one band in iteration order. The script compared dithered 8-bit grabs: the viewer's error diffusion starts at an unseeded rand(), so a light checker square shows as 187 or 188. Fixed in 20ec02320 with a ±1 tolerance and failing points printed. A new gtest, AccumulateIsTheSameOnOneThreadAndMany, compares 1 and 4 threads at 640x480 bit for bit. Gate re-running.
- 2026-11-20 — Gate green on 20ec02320: m183_map_blend passes with the dither tolerance; the only ctest failure was the SceneViewportStorm flake, which passed on rerun. The 32-iteration culling bench runs locally once the host is quiet (M434's bench is running).
- 2026-11-21 — **Culling bench passes at 32 iterations** on the release AppImage of 20ec02320 (Intel N100): 0.55 at 1 thread (full 3.995 s, half 2.186 s) and 0.54 at 8 threads (2.622 / 1.406). Two earlier runs the same afternoon gave 0.66 at 1 thread while remote-build watchers and a review were loading the host; the slope check settled it: 1 iteration costs 0.15 s either way, and 32→64 iterations adds 4.90 s full vs 2.54 s half (0.52), so culling halves each iteration's work and the fixed cost is negligible. Bench caveats for a later look, not gate blockers: full times climb rep by rep within a process (cache growth from nudged reps), and the timed render includes the serial EXR write.
- 2026-11-21 — Review round (Codex, PR #332): 5 major + 2 minor findings, all real, fixed in 2419bf86d (disjoint per-tile iteration rectangles on the legacy path, abort checks in the Map scan and accumulate, grouped-Map pairing, GPU-continuation CPU fallback under the continuation context, per-request continuation layers, docs iteration rule). Gate on 2419bf86d green (remote run 44949514256: release, both ctest legs, m183_map_blend 42/42). Culling bench on that build, quiet host, 1 thread: 0.66, 0.56, 0.55, 0.55 (bimodal: full 3.4 s vs 4.1 s); 8 threads 0.54. **Gate passes with these numbers (user).** Open for the user: the task-graph scheduler still merges each output's requests into one bounding box (ParallelRenderArgs.cpp:1090), so disjoint culling only helps the legacy path.
