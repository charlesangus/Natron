# M129 - Clean-Sheet Noise Node

A clean-sheet `Noise` generator replaces three OFX nodes: openfx-misc Noise (Rand), CImg Noise and CImg Plasma.

- **No Source input:** it is a pure generator (user). CImg Noise's "add noise to an input" job is retired, and users Merge (plus) instead.
- **Noise types:**
  - gradient (Perlin and/or simplex);
  - value;
  - cellular/Voronoi (F1, F2, F2−F1, cell value, distance to edge, with Euclidean, Manhattan and Chebyshev metrics);
  - white/random (replaces Rand).
- **Fractal combiners:** fBm, turbulence/billow, ridged and hybrid.
- **Domain warp.**
- **4D is required (user):** x, y, z plus an evolution axis that animates over time.
- **Tile- and scale-independent:** output is position-hashed in canonical space.
- **Every type runs on the GPU** with CPU/GPU parity.

**Phase 1 is a spike (user).** It compares porting Blender Cycles' noise kernels against OpenSimplex2 plus our own cellular, value and fractals, and ends with a decision and a user checkpoint. The later phases are written library-agnostic. The spike fills in which files are vendored.

Plain ID `Noise`, major 1. Registration, kernel lists, cost seeds, icons, docs and OFX retirement go through M120 - Native Node Port Kit's per-node fragment files (`Engine/Nodes/Generator/Noise.{reg.inc,costseed.inc}`, `docs/Noise.md`, `Engine/Gpu/Kernels/noise.cmake`) and one `Engine/OfxRetired/<ofx-plugin-id>` marker per retired ID.

Deps: M120 - Native Node Port Kit, M85 - Native GPU Kernels (`PixelKernelGpu`, `generators.slang`, `pixelwrap.slang`).

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):

**Rand** (openfx-misc `Rand/Rand.cpp`, `net.sf.openfx.Noise`):
- Knobs: `noise` 1, `density` 1, `seed` 2000, `staticSeed` off.
- Maths (`:130-190`): `hash(hash(hash(seed^x)^y)^c)/2³² - 0.5`, then `mean + noise·r`. Pixels with `rand > density` are 0. The seed hashes in the frame unless `staticSeed` is on.
- Noise level and density scale with renderScale, so output depends on render scale.

**CImg Noise** (`CImg/Noise/CImgNoise.cpp`, `net.sf.cimg.CImgNoise`):
- A filter that adds noise to its input.
- Knobs: `sigma` 0.01, `type` (Gaussian, Uniform, Salt&Pepper, Poisson, Rice), `seed`, `staticSeed`.
- Output is clamped to the type's range.

**CImg Plasma** (`CImg/Plasma/CImgPlasma.cpp`, `net.sf.cimg.CImgPlasma`):
- Midpoint displacement with wraparound.
- Knobs: `alpha` 0.002, `beta` 0, `scale` 8, `offset`, `seed`.
- `kSupportsTiles 0`, so it depends on image size and render scale.

**Native generator template:**
- `Engine/Nodes/Image/NativeGenerator.h:96`, with a FINAL `render` that runs `makeKernel` in row bands.
- Position-dependent kernel pattern: `CheckerBoard.cpp:361-431`.
- GPU template: M85 - Native GPU Kernels's `Engine/Gpu/Kernels/generators.slang` (`IPixelOp`, `runWrapped`) plus `makeGpuKernel` and `NativeConstantGpu_Test.cpp`.
- Push constants are at most 128 B, and resources are storage buffers in set 0 (`Engine/Gpu/GpuKernel.h:16-19`). Kernels are offline-compiled SPIR-V.

**Candidate sources** (research, 2026-10-13):
- **FastNoiseLite** is ruled out: it is 2D/3D only.
- **Blender Cycles** `intern/cycles/kernel/svm/{noise,voronoi,fractal_noise}.h` and `kernel/util/hash.h`:
  - Licence: Apache-2.0.
  - Coverage: Perlin and Voronoi in 1–4D; fBm, multifractal, hybrid, ridged and hetero-terrain; Voronoi F1, F2, smooth F1, distance to edge and n-sphere radius.
  - Uses an integer hash, with no tables.
  - Written in a portable C++ subset that Cycles compiles for several GPU backends.
- **OpenSimplex2 (KdotJPG):**
  - Licence: CC0.
  - 4D OpenSimplex2F/S, with Java, C#, Rust, GLSL and HLSL ports.
  - Gradient only: no cellular, no value noise.
- **Licensing:** Natron is GPL-2.0-or-later (`LICENSE.txt`, file headers). Apache-2.0 is GPLv3-compatible only, so vendoring Cycles code makes the distributed binaries effectively GPLv3. OIIO 3.x is already Apache-2.0.

**References to the retired IDs:**
- `Tests/LayerKnobs_Test.cpp:282-289` (Noise, CImgPlasma).
- `tools/ci/smoke_test.py:305` (CImgPlasma as a bundle check).
- `tools/bench/make_plates.py:75` (Noise).
- `Tests/BaseTest.cpp:79` (`PLUGINID_OFX_SENOISE`) is SeExpr noise. It is unrelated and stays.

## Scope

**In:**
- The library spike and its decision.
- The vendored or written noise core in C++ and Slang.
- The native `Noise` generator with every type, fractal and warp listed above, in 4D.
- CPU and GPU parity tests.
- Retiring Rand, CImg Noise and CImg Plasma.

**Out:**
- Adding noise to an input image (CImg Noise's mode): retired, use Merge plus. No milestone.
- A scene-linear film Grain node: future work. No milestone yet.
- SeExpr noise: stays OFX. No milestone.
- Noise as a displacement or warp of another image: use M113 - Native Warp Nodes' IDistort with Noise as the UV input.

**Edge cases:**
- Tiling: any tile split, strip height or GPU region renders the same pixels as a whole-frame render. The tests assert bit-equality on CPU.
- Render scale below 1: the kernel evaluates at canonical coordinates, so it matches a downscaled full render (statistically for white noise, exactly per sample point otherwise).
- Large coordinates (|x| > 1e5): no precision collapse within the documented range. Use integer cell + fractional offset decomposition. The spike measures the float32 GPU limit.
- Evolution animated through time: continuous in 4D, with no per-frame reseed unless the white type's "reseed per frame" is on.
- Octaves: capped (Unconfirmed: 16) so a large octave count can't stall a render.

**Assumptions:**
- Unconfirmed: knobs are `type`, `fractal`, `octaves`, `lacunarity`, `gain`, `size` (2D), `zOffset`, `evolution` (animatable, the 4th axis), a transform (translate/rotate/scale/centre), `seed`, `warpType`, `warpAmount`, cellular `metric`/`output`/`jitter`, `color0`/`color1` and output `amplitude`/`offset`.
- Unconfirmed: output is a single channel replicated to the selected output channels, with colour0 → colour1 mapping. Per-channel decorrelated noise (a different seed offset per channel) is a toggle.
- Unconfirmed: CPU/GPU parity tolerance is 1e-5 absolute for gradient, value and fractals. Cellular F-distances are exact up to float ties, and hash-based white noise is bit-exact.
- The spike can build Slang prototypes through the existing offline `slangc` path without new build infrastructure.

## Phase 128.1: Library spike

- [ ] M129.P1.T1 — Prototype Cycles-derived 4D Perlin and 4D Voronoi F1 in C++ and Slang
  - files: `tools/spikes/noise/cycles/` (new: `noise4d.h`, `voronoi4d.h`, `noise4d.slang`, `NOTICE`), `tools/spikes/noise/bench.cpp` (new)
  - approach:
    - Port Cycles' hash, 4D Perlin and 4D Voronoi F1 (Euclidean) to a self-contained C++ header and a Slang module, keeping the Apache-2.0 headers.
    - Compile with strict float settings (no fast-math; `-ffp-contract=off` on CPU; the Slang equivalent) so the CPU and GPU differ only by intrinsic accuracy.
    - Use a small harness that renders 2K tiles of each on CPU and GPU through the existing Vulkan test fixture.
  - verify: the harness reports CPU ms/Mpx, GPU ms/Mpx, max abs CPU/GPU difference, and writes EXR samples to `build/spikes/noise/`. It runs at the spike build; spike code is not linked into Natron.
  - size: L

- [ ] M129.P1.T2 — Prototype OpenSimplex2S 4D plus our own 4D Voronoi F1 in C++ and Slang
  - files: `tools/spikes/noise/opensimplex/` (new: `os2s4d.h`, `voronoi4d.h`, `os2s4d.slang`, `LICENSE`), `tools/spikes/noise/bench.cpp`
  - approach:
    - Port OpenSimplex2S 4D (CC0) from its HLSL/C# sources to C++ and Slang.
    - Write a hash-based 4D Voronoi F1 with the same hash on both sides.
    - Use P1.T1's strict float settings and harness.
  - verify: same report as P1.T1.
  - size: L

- [ ] M129.P1.T3 — Compare, decide, and checkpoint with the user
  - files: `PLAN/DESIGN/<date of the spike>-noise-library-spike.md` (new, plan branch, dated the day the spike runs), this milestone file (`## Decisions`)
  - approach:
    - Tabulate quality (visual artefacts: axis alignment, grid patterns; EXR crops attached), CPU and GPU speed, parity error, 4D coverage per type, porting effort for the remaining types, and licence consequences.
    - Recommend one, and present it to the user via the PM's open question. **The licence choice is the user's.**
    - Record the decision under `## Decisions` and fill in P2.T1's vendored-file list.
  - verify: the design note exists, and a dated decision naming the chosen source and licence is recorded after the user answers.
  - size: M

## Phase 128.2: Noise core

- [ ] M129.P2.T1 — Vendor or write the chosen 4D noise core in C++
  - files: `libs/noise/` (new: the chosen core headers, `LICENSE`/`NOTICE`, `README.md` naming the upstream revision), `Engine/Nodes/Generator/NoiseCore.h` (new: the engine-facing API), `Tests/Native/NoiseCore_Test.cpp` (new)
  - approach:
    - One API with `float noise4(Type, float4 p, const NoiseParams&)` covering gradient, value, cellular (F1, F2, F2−F1, cell value, distance to edge; the metric is a parameter) and white. White is hash(cell, seed) per canonical pixel centre.
    - Integer cell + fractional decomposition for precision.
    - The C-like subset (POD params, no virtuals in the inner loop) so P2.T2 mirrors it line for line.
    - Third-party notices are updated (`docs/` or the packaging licence list) per the spike's licence decision.
  - verify: the test covers determinism, seed sensitivity, output range bounds per type, continuity across cell boundaries (gradient/value), and the 4th axis changing the result. It runs at the phase build.
  - size: L

- [ ] M129.P2.T2 — Add fractal combiners and domain warp, and mirror the core in Slang
  - files: `Engine/Nodes/Generator/NoiseCore.h`, `Engine/Gpu/Kernels/noisecore.slang` (new), `Engine/Gpu/Kernels/noisecore.cmake`, `Tests/Native/NoiseCoreGpu_Test.cpp` (new)
  - approach:
    - Add fBm, turbulence/billow, ridged and hybrid over any base type, with an octave cap, plus domain warp (offset p by a lower-frequency vector noise).
    - Port the whole core to Slang line for line.
    - Add a test-only compute entry that samples a coordinate buffer.
  - verify: CPU/GPU parity per type × fractal × warp on random 4D points within the stated tolerances. It runs at the phase build on RADV.
  - size: L

## Phase 128.3: Node

- [ ] M129.P3.T1 — Add the native Noise generator
  - files: `Engine/Nodes/Generator/Noise.{h,cpp}` (new), `Engine/Nodes/Generator/Noise.reg.inc`, `Tests/Native/NativeNoise_Test.cpp` (new)
  - approach:
    - `Noise` v1 on `NativeGenerator`, with the knobs in Scope's assumption.
    - The kernel maps the canonical pixel centre through the inverse transform to noise space (x, y, `zOffset`, `evolution`) and evaluates `NoiseCore`.
    - Map colour0 → colour1, apply amplitude/offset, and write the selected channels.
    - The default extent is the project format.
  - verify: the test covers tile invariance (a 4-strip render bit-equals a whole render), render-scale invariance at sample points, the transform rotating the pattern, evolution animating, per-channel decorrelation, and the octave cap. It runs at the phase build.
  - size: L

- [ ] M129.P3.T2 — Run Noise on the GPU
  - files: `Engine/Gpu/Kernels/noise.slang` (new), `Engine/Nodes/Generator/Noise.cpp`, `Engine/Gpu/Kernels/noise.cmake`, `Tests/Native/NativeNoiseGpu_Test.cpp` (new)
  - approach:
    - An `IPixelOp` over `noisecore.slang` run through `runWrapped`.
    - Pack the params into ≤128 B of push constants, or use a small storage buffer if the transform plus warp exceed it.
    - Add the cost seed.
  - verify: parity over P3.T1's fixtures within the stated tolerances. It runs at the phase build on RADV.
  - size: L

## Phase 128.4: Retire the OFX noise nodes

- [ ] M129.P4.T1 — Retire Rand, CImg Noise and CImg Plasma, and fix their references
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (three markers), `Tests/LayerKnobs_Test.cpp`, `tools/ci/smoke_test.py` (pick another CImg plugin as the bundle check), `tools/bench/make_plates.py`
  - approach:
    - Add `Engine/OfxRetired/net.sf.openfx.Noise`, `Engine/OfxRetired/net.sf.cimg.CImgNoise` and `Engine/OfxRetired/net.sf.cimg.CImgPlasma` markers.
    - Move the test and bench cases to native `Noise`.
  - verify: a test asserts the three IDs no longer load and `Noise` does. The full ctest and the smoke test pass at the gate.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including core, node and GPU parity tests on RADV.
- The spike decision is recorded with the user's licence choice.
- An Xvfb GUI check with screenshots of each type and fractal, shared before sign-off.
- An animated evolution clip renders without popping.
- An AppImage built with `package.sh` for UAT.
- The retired IDs no longer load.

## Decisions

- 2026-10-13 — **A clean-sheet Noise node in its own milestone (user, native-node port grill):** it has the usual types (fBm, turbulence, Voronoi, cellular and more) and Rand becomes one of them. It was split out of M128 - Native Shape Generators as a larger task.
- 2026-10-13 — **4D noise is required (user):** x, y, z plus evolution. FastNoiseLite is ruled out because it is 2D/3D only.
- 2026-10-13 — **Spike before choosing the library (user):** Cycles kernels (Apache-2.0, binaries effectively GPLv3) against OpenSimplex2 (CC0) plus our own cellular/value/fractals. The licence choice stays with the user at P1.T3.
- 2026-10-13 — **A pure generator (user):** no Source input, and CImg Noise's add-to-input mode is retired.
