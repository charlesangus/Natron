# M51 - Deep Filtering OpenDCX

Full title: Deep filtering nodes (OpenDCX integration)

Integrate OpenDCX (DreamWorks' open-source deep compositing extensions
library) to add production-grade filtering, transformation, and manipulation
nodes for deep pixel data. Extends M18's core deep compositing foundation
with filtering operations on multi-sample deep pixels, subpixel masking,
and efficient deep-data transformations.

## Phase 51.1: OpenDCX integration and DeepBlur

- [ ] M51.P1.T4 — Check that OpenDCX builds and runs against our OpenEXR 3 (do this first)
  - files: a throwaway build in the natron-dev container; findings in this file's `## Decisions`
  - approach: OpenDCX was written against OpenEXR 2.x and the old IlmBase/Imath split; our baseline is OpenEXR 3 with standalone Imath (CY2027 direction). Build its library alone against the container's OpenEXR 3 and Imath, list every API break, and run its deep-pixel tests. Record one of: builds as is; builds with a small patch set (list it, to carry as a pinned fork patch); or the port is large, in which case stop and bring the user the choice between porting it and writing DeepBlur and DeepTransform natively on `renderDeepRoI`.
  - verify: the decision line names the outcome, the OpenEXR/Imath versions built against, and the patch list or break count.
  - size: M
- [ ] M51.P1.T1 — Vendorize OpenDCX and integrate as a CMake dependency (after M51.P1.T4)
  - files: `tools/cmake/FindOpenDCX.cmake`, `CMakeLists.txt`, build config, plus M51.P1.T4's patch set if any
  - approach: Add OpenDCX (github.com/dreamworksanimation/opendcx) as a
    pinned vendored dependency (fetch on build). Ensure it links cleanly
    against the existing OpenEXR and Python bindings. Target glibc 2.34+
    compliance on EL9.
  - verify: Build succeeds with OpenDCX statically linked; a minimal test
    that calls OpenDCX's deep API without errors completes.
  - size: M
- [ ] M51.P1.T2 — Design DeepBlur node: blur on deep pixel data
  - files: `Engine/Nodes/Deep/DeepBlur.{h,cpp}`, `Engine/Nodes/Deep/docs/DeepBlur.md`, `Tests/` deep render test
  - approach: Blur (box, Gaussian, or both) applied per-sample on deep
    pixels, preserving sample depth and coverage. Each sample at each pixel
    location is blurred independently. Knobs: blur method (box/Gaussian),
    blur size (XY radii), edge handling (pad/wrap/clamp). Start with a
    single blur method; add others if time/QA headroom permits.
  - verify: GUI test — blur radius knob changes output interactively. CLI
    render: deep blur on a deep image with varying depth/coverage produces
    smooth inter-sample blurring and maintains coverage correctness. Compare
    against OpenDCX reference behavior where applicable.
  - size: L
- [ ] M51.P1.T3 — Register DeepBlur as a native node
  - files: `Engine/Nodes/Deep/DeepBlur.reg.inc`, `Tests/Native/NativePluginList_Test.cpp`
  - approach: add `NATRON_NATIVE_NODE(DeepBlur, "DeepBlur.h")` in a `.reg.inc` beside the node, as `DeepCrop.reg.inc` does; `Engine/CMakeLists.txt` globs `Nodes/*.reg.inc`, so no CMake or loader edit. Add it to the native plugin list test. It lands in the Deep menu category like the other deep nodes.
  - verify: DeepBlur appears in the Create Node menu under Deep category.
    A project using it loads and renders without errors.
  - size: M

## Phase 51.2: DeepTransform (affine transforms on deep pixels)

- [ ] M51.P2.T1 — Design DeepTransform: scale, rotate, skew deep pixels
  - files: `Engine/Nodes/Deep/DeepTransform.{h,cpp}`, `Engine/Nodes/Deep/docs/DeepTransform.md`, `Tests/` deep render test
  - approach: Affine transformation (scale, rotation, skew) applied per-sample
    on deep pixels, preserving sample depth and coverage. Resampling strategy:
    decide between bilinear, bicubic, or OpenDCX's built-in resampling. Knobs:
    translate (XY), scale (XY or uniform), rotate (degrees), and optional
    skew (shear). Reference OpenDCX's DeepTransform class for efficient
    subpixel-accurate implementation.
  - verify: GUI interactive transform. CLI render: a rotated deep image shows
    correct depth ordering and no sample loss at boundaries. Verify subpixel
    accuracy via a test with fractional transforms.
  - size: L
- [ ] M51.P2.T2 — Register DeepTransform as a native node
  - files: `Engine/Nodes/Deep/DeepTransform.reg.inc`, `Tests/Native/NativePluginList_Test.cpp`
  - approach: as M51.P1.T3 — a `.reg.inc` beside the node and a native plugin list test entry.
  - verify: Menu presence, load/render verification as above.
  - size: M

## Phase 51.3: Deep utility nodes (QA and Polish)

- [ ] ~~M51.P3.T1 — Add DeepSamples and DeepInfo inspection nodes~~ (cancelled 2026-11-14: per-pixel inspection is M32 - Deep Sample Node's; replaced by M51.P3.T3)
- [ ] M51.P3.T3 — DeepInfo: whole-image deep statistics
  - files: `Engine/Nodes/Deep/DeepInfo.{h,cpp,reg.inc}`, `Engine/Nodes/Deep/docs/DeepInfo.md`, `Tests/` deep test, `Tests/Native/NativePluginList_Test.cpp`
  - approach: a deep pass-through that reports whole-image statistics for the current frame in read-only knobs: depth range (min Z, max ZBack), total and maximum samples per pixel, and the deep layers and channels present. Nothing per pixel: inspecting one pixel's samples is M32 - Deep Sample Node. Registered through a `.reg.inc`.
  - verify: a gtest on a fixture deep image checks each reported value; downstream renders are unchanged (pass-through hash).
  - size: M
- [ ] M51.P3.T2 — Polish deep node UI and docs
  - files: doc strings, Help menu entries, example projects
  - approach: Write brief help text for DeepBlur and DeepTransform. Add
    example .ntp projects demonstrating each node. Ensure knob tooltips
    explain subpixel precision, sample depth semantics, and coverage
    preservation.
  - verify: Help text is searchable and explains each knob. Example projects
    load and render without errors.
  - size: S

**Verification gate:** DeepBlur and DeepTransform nodes ship, load existing
deep projects without errors, and render deep images with correct per-sample
filtering/transformation. Example projects demonstrating both exist. OpenDCX
library is vendored and statically linked; no external runtime dependency.

## Decisions

- 2026-09-18 — **OpenDCX chosen**: DreamWorks' production-grade deep
  compositing library. Rationale: widely used in film VFX, C++ API integrates
  cleanly with OpenEXR, provides battle-tested filtering and transform
  algorithms on deep pixels. Alternative (DIY implementation) ruled out due to
  algorithmic complexity and risk of sample-loss bugs.
- 2026-09-18 — **Phase sequencing**: DeepBlur first (simpler, high immediate
  value), DeepTransform second (more complex, useful for deep manipulation
  workflows), then utility/polish. Blocks on M18 shipping; pairs naturally
  with M21 (deep tier-2 nodes).
- 2026-11-14 — **OpenDCX build check first, DeepInfo narrowed** (user accepted /cat-plan proposals): M51.P1.T4 checks OpenDCX against OpenEXR 3 before anything builds on it; registry tasks now use `.reg.inc` files (`HostModel/EffectPluginLoader.cpp` does not exist); per-pixel inspection belongs to M32 - Deep Sample Node, so DeepInfo reports whole-image statistics only.
