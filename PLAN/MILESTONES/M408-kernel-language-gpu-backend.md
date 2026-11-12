# M408 - Kernel Language GPU Backend

Lower M360 - Geometry Kernel Language's IR to Slang so wrangles run on the GPU through the existing Slang compute stack (E5 - GPU Compute, `Engine/Gpu/`). It exists so M409 - Particle GPU Path can keep particles resident while `PopWrangle` runs (user, 2026-11-12: Slang codegen). `GeoWrangle` gains the same GPU path. A snippet using a builtin with no GPU form fails to compile for the GPU and falls back to the CPU interpreter, with a warning naming the builtin.

Member of F19 - Particles: branches off and merges into `feature/f19-particles` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- A Slang code generator from `KernelProgram` (`Engine/Kernel/KernelTypes.h`) for the arithmetic, control flow, vector/matrix types, `@attr` reads and writes, `ch()` values as uniforms, `@ptnum`/`@numpt`/time values, and the noise builtins.
- Runtime compilation through the runtime shader compiler (`PLAN/DESIGN/2026-10-13-runtime-shader-compiler.md`), cached by snippet hash.
- A GPU runner binding attribute buffers (struct-of-arrays, dense in point-number order) and dispatching one thread per element.
- Placement: wrangles go to the GPU through the weight-based placement (M239), not a timing seed.
- CPU fallback per snippet when any builtin lacks a GPU form (topology edits, `nearpoint(s)`, string attributes, second-input reads in this milestone).

**Out:**
- GPU topology edits, `nearpoint(s)` and second-input reads (none; CPU fallback).
- GPU forms of M415 - Geometry Attribute Ops' surface-query builtins (none).
- Volume wrangles on the GPU (none; M419 - VDB SDF Ops And Wrangle is CPU).

**Edge cases:**
- CPU vs GPU float results differ by ulps: tests compare with a tolerance, never bit-exact (memory: ulp diffs never reopen the architecture).
- A snippet the GPU compiler rejects for a reason the CPU front end accepted: falls back to CPU with the compiler message in the node's info, not an error.
- Huge details exceeding a GPU buffer limit: dispatch in page-aligned chunks.
- Device lost mid-run: the existing GPU error path reruns on the CPU.

**Assumptions:**
- Unconfirmed: the IR is low-level enough that codegen is a straight walk; if not, a small SSA lowering is added in P1.T1.

## Phase 408.1: Codegen

- [ ] M408.P1.T1 — Slang code generator for kernel programs
  - files: `Engine/Kernel/KernelSlangCodegen.{h,cpp}`, `Engine/Kernel/KernelTypes.h`, `Tests/KernelSlangCodegen_Test.cpp`
  - approach: Walk the compiled `KernelProgram` and emit a Slang compute entry point with one buffer per bound attribute and a uniform block for `ch()` values and time. Each builtin maps to a Slang function in a shared prelude, or marks the program CPU-only with the builtin's name. Keep the generator free of `GeoDetail` types, as the front end is.
  - verify: `KernelSlangCodegen_Test` generates Slang for a set of reference snippets, compiles each with slangc in-process, and checks that unsupported builtins report CPU-only with the right name.
  - size: L

## Phase 408.2: Runner and wrangle

- [ ] M408.P2.T1 — GPU runner and `GeoWrangle` GPU placement
  - files: `Engine/Geo/GeoKernelGpuRunner.{h,cpp}`, `Engine/Nodes/Geometry/GeoWrangle.cpp`, `Tests/Gpu/GeoWrangleGpu_Test.cpp`
  - approach: Upload bound attributes densely, dispatch, read back only written attributes into new pages. Ask placement for the device like image kernels do; fall back to the CPU interpreter on CPU-only programs or GPU errors, with an info message. Tests run under lavapipe in CI as other GPU tests do.
  - verify: `GeoWrangleGpu_Test`: a dozen reference snippets give CPU and GPU results equal within 1e-5 relative; a snippet using `addpoint` runs on the CPU with the fallback message.
  - size: L

**Verification gate:** full ctest green including `KernelSlangCodegen_Test` and `GeoWrangleGpu_Test` under lavapipe; the 10M-point `@P.y += 1` run timed CPU vs GPU on the GPU host and recorded in `## Decisions` (report, no threshold).

## Decisions

- 2026-11-12 — **Slang codegen for the kernel language (user, /cat-plan grill),** so `PopWrangle` runs on resident GPU particles; split out of M409 - Particle GPU Path on sizing.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
