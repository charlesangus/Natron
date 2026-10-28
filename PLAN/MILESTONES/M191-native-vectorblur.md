# M191 - Native VectorBlur

A native motion blur from motion vectors (user, 2026-10-25, split from M188 - Variable Median And DirBlur). No VectorBlur exists anywhere in the tree (M131 - Native Defocus Family's scouting). Each output pixel integrates the source along its own motion vector, read from **one** u/v layer (user: two vector passes give kinked blur, so they are not wanted). A continuous `offset` knob places the vector relative to the pixel: 0 starts the streak at the pixel, 1 ends it there, 0.5 centres it. A shutter `scale` (shutter-angle-like) multiplies the vectors. With a depth channel connected, samples are weighted by depth so a sharp foreground is not smeared by the background's vectors (the occlusion setting of RSMB and Nuke's VectorBlur2).

## Scope

**In:**
- `VectorBlur`, plain ID at major 1, Filter grouping. Inputs: Source, optional Mask (host MaskMix).
- Knobs: `uv` layer and u/v channels (any layer, default the first of `motion`, `forward`), `offset` (0..1, default 0.5), `scale` (default 1), `vectorUnits` (Pixels, Normalised to width), `samples` (default 16, adaptive to streak length up to `maxSamples`), `filter` (Box, Triangle), `depth` layer/channel with `depthUnits` (Distance, 1/Distance) and `occlusion` (Off, Depth-aware).
- CPU and GPU kernels, native and GPU parity tests, a size-scaled `gpuWeight()`, an icon and a doc.

**Out:**
- Two-pass (forward + backward) vector modes (user).
- A Map driving per-pixel strength: the universal M183 - Loop Map Blend covers it, since `scale` is drivable.
- Scatter with depth-sorted compositing: no milestone owns it.

**Edge cases:**
- Zero vector: identity at that pixel.
- Non-finite vectors are treated as zero.
- RoI expansion: by the maximum vector length × scale over the requested region; since vectors are unknown in the request pass, the RoI uses a `maxLength` knob (default 128 px) and longer vectors are clamped to it, with a node warning when any are.
- Depth-aware occlusion: a sample contributes only if its depth is not nearer than the output pixel's beyond a tolerance, unless the sample's own vector reaches the output pixel (it is a moving foreground smearing over the background).
- Proxy/render scale: pixel-unit vectors scale with render scale.

## Phase 191.1: VectorBlur

- [ ] M191.P1.T1 — Write the native VectorBlur node (CPU)
  - files: `Engine/Nodes/Filter/VectorBlur.h`, `Engine/Nodes/Filter/VectorBlur.cpp`, `Engine/Nodes/Filter/VectorBlur.reg.inc`
  - approach: Read the u/v channels with `Node::declareLayerKnob` on the Source input (like STMap's `kSTMapParamChannelU`). Per pixel, sample the source along `[−offset, 1−offset] · scale · v` with the chosen filter's weights, sample count adaptive to streak length; bilinear source taps. Depth-aware weighting per `## Scope`. Read `Engine/Nodes/README.md` first.
  - verify: at the phase build, `ctest -R NativeVectorBlur` (T2) passes.
  - size: L

- [ ] M191.P1.T2 — Test VectorBlur on CPU
  - files: `Tests/Native/NativeVectorBlur_Test.cpp`
  - approach: Zero vectors are identity. A constant horizontal vector equals a box DirBlur of the same length within tolerance. Offset 0 and 1 streak to opposite sides of a point. Depth-aware mode keeps a static near square sharp over a moving far background. Clamping past `maxLength` warns. Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeVectorBlur` passes.
  - size: M

- [ ] M191.P1.T3 — VectorBlur on the GPU
  - files: `Engine/Gpu/Kernels/vectorblur.slang`, `Engine/Gpu/Kernels/vectorblur.cmake`, `Engine/Nodes/Filter/VectorBlur.cpp`, `Tests/Native/NativeVectorBlurGpu_Test.cpp`
  - approach: One thread per output pixel, the same sampling; the source tile with a `maxLength` apron.
  - verify: at the phase build, `ctest -R NativeVectorBlurGpu` passes on lavapipe within 1e-4 relative.
  - size: L

- [ ] M191.P1.T4 — Icon, gpuWeight (size-scaled) and doc
  - files: `Gui/Resources/Images/NativeNodes/`, `Engine/Nodes/Filter/VectorBlur.cpp`, `Engine/Nodes/Filter/docs/VectorBlur.md`
  - approach: Per the native checklist; the doc explains `offset`, units, `maxLength` and occlusion.
  - verify: `ctest -R NativePluginList` passes; doc reviewed at the gate.
  - size: S

## Decisions

- 2026-10-28: implemented (build pending; goldens to record). No OFX VectorBlur exists (openfx-misc has VectorToColor only); the milestone's knob list plus Nuke's VectorBlur docs are the spec. Vectors read from a Source layer (Auto: "motion", then forward motion). Samples = min(Max Samples, max(Samples, ceil(length))), midpoint-spaced bilinear taps, black outside; Max Samples knob added (128). Occlusion: depth ≤0/non-finite = infinitely far; samples >1% nearer dropped unless their own streak passes within 1 px; all dropped → unchanged. Clamp warning posted by CPU renders only. GPU tolerance 1e-5 abs + 1e-4 rel.
- 2026-10-25 — **One u/v layer and a continuous offset (user):** two vector passes give kinked blur; the offset knob places the streak anywhere from starting to ending at the pixel.
- 2026-10-25 — **Depth-aware occlusion, optional (user):** plain gather by default.

**Verification gate:** the full build and ctest pass, including both VectorBlur suites (GPU on lavapipe and RADV). Under Xvfb VectorBlur is rendered on a CG fixture with motion vectors and depth, with occlusion off and on and offsets 0, 0.5 and 1; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
