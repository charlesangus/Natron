# M444 - Cycles MaterialX Procedurals

Match MaterialX's procedurals exactly in Cycles by adding new SVM nodes to our Cycles fork (`charlesangus/cycles` `natron/deep-5.2`), ported from MaterialX's reference implementations (Apache-2.0, fine under GPLv3-or-later). Cycles' own Noise, Voronoi and fractal nodes use different algorithms, so mapping onto them would render differently from MaterialX and from MoonRay's `materialx_shaders` (user, 2026-11-15). The new nodes are registered in Cycles' `NodeType` registry, so hdCycles creates them from `cycles_<type>` ids with no further delegate change. M443 - Cycles MaterialX Backend then maps them and drops them from its known-unsupported list.

## Scope

**In:**
- SVM nodes for `noise2d`, `noise3d`, `fractal2d`, `fractal3d`, `cellnoise2d`, `cellnoise3d`, `worleynoise2d`, `worleynoise3d`, `unifiednoise2d`, `unifiednoise3d`, `hextiledimage` and `hextilednormalmap`, covering every output type MaterialX defines for each.
- One shared port of MaterialX's noise library (`mx_noise` and its hash functions) in the fork's kernel headers.
- Backend mappings for every type variant, with parity against the reference evaluator.

**Out:**
- GPU kernels: the bundled Cycles is CPU-only. The kernel code stays in Cycles' device-portable style, so a later GPU build compiles it unchanged.
- Changing Cycles' existing Noise or Voronoi nodes: none, they stay as they are.

**Edge cases:**
- A node that MaterialX defines as a nodegraph over these atomics (e.g. a fractal built from noise): left to the core's inlining once the atomics exist.
- Float precision: the port follows MaterialX's float arithmetic so parity holds to the stated tolerance, not bit-exact.
- `hextiledimage` needs an image lookup inside the node: it samples through the same image path as `image_texture`, so `natron://` textures work.

**Assumptions:**
- MaterialX's GLSL and OSL implementations of these nodes agree with each other closely enough to serve as one reference. P1.T1 checks two nodes against both before porting the rest.

## Phase 444.1: Kernels

- [ ] M444.P1.T1 — Port MaterialX's noise library
  - files: the Cycles fork's `src/kernel/svm/mx_noise.h`, `src/kernel/CMakeLists.txt`
  - approach: port MaterialX's `mx_noise` library (Perlin gradient noise, cell noise, worley, fractal sum and their hash functions) from its GLSL and OSL sources into one kernel header in Cycles' device-portable style. Record the MaterialX version and source files in the header's licence block.
  - verify: a fork unit test evaluates 2D and 3D Perlin noise and cell noise at 64 fixed points and matches values precomputed with M439 - MaterialX Translation Spike's reference evaluator within 1e-5.
  - size: L

- [ ] M444.P1.T2 — Add noise, fractal and cell noise nodes
  - files: the Cycles fork's `src/scene/shader_nodes.{h,cpp}`, `src/kernel/svm/mx_procedural.h`, `src/kernel/svm/svm.h`, `src/kernel/svm/types.h`
  - approach: SVM nodes for `noise2d/3d` (amplitude, pivot), `fractal2d/3d` (octaves, lacunarity, diminish) and `cellnoise2d/3d`, each with float, colour and vector outputs as MaterialX defines them, built on P1.T1's header. Register each as a `NodeType` with socket names matching MaterialX inputs.
  - verify: fork unit test — each node evaluated through the SVM at fixed points matches the reference within 1e-4 for every output type.
  - size: L

- [ ] M444.P1.T3 — Add worley, unified noise and hex tiling nodes
  - files: the Cycles fork's `src/scene/shader_nodes.{h,cpp}`, `src/kernel/svm/mx_procedural.h`, `src/kernel/svm/svm.h`, `src/kernel/svm/types.h`
  - approach: `worleynoise2d/3d` (jitter, style, all output types), `unifiednoise2d/3d` (type switch over the others, frequency, offset, jitter, outmin/outmax, clamp, octaves) and `hextiledimage`/`hextilednormalmap` sampling through the image path `image_texture` uses.
  - verify: fork unit test — worley and unified noise match the reference within 1e-4 at fixed points; a hex-tiled checker texture matches a reference crop.
  - size: L

- [ ] M444.P1.T4 — Bump the Cycles pin
  - files: `tools/ci/local/Dockerfile`
  - approach: push the fork branch, set `NATRON_CYCLES_COMMIT` and its `NATRON_CYCLES_SHA256` to the new commit, and rebuild and publish the dev image.
  - verify: the image builds; a script in the image lists the new node types from Cycles' `NodeType` registry.
  - size: M

## Phase 444.2: Mapping

- [ ] M444.P2.T1 — Map the procedurals and prove parity
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesProcedurals.cpp`, `Tests/Scene/data/mtlx/cycles-unsupported.json`, `Tests/Scene/MtlxParityCycles_Test.cpp`, `Engine/Nodes/Scene/docs/MaterialX.md`
  - approach: map every type variant of each node in Scope **In** onto its new `cycles_<type>` id, remove them from the known-unsupported list, and add them to the parity test. Update the doc's Cycles coverage section.
  - verify: `MtlxCoverage_Test` passes with the shortened list; `MtlxParityCycles_Test` passes for every procedural variant; a noise3d material renders the same pattern in Cycles and MoonRay within the parity tolerance.
  - size: M

**Verification gate:** release gate green per `.local/build-mode` on the rebuilt image; the fork's procedural unit tests pass; `MtlxParityCycles_Test` and `MtlxCoverage_Test` pass. The user's sign-off is collected at the UAT of F26 - MaterialX Rendering.
