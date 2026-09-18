# Milestone 51: Deep filtering nodes (OpenDCX integration)

Integrate OpenDCX (DreamWorks' open-source deep compositing extensions
library) to add production-grade filtering, transformation, and manipulation
nodes for deep pixel data. Extends M18's core deep compositing foundation
with filtering operations on multi-sample deep pixels, subpixel masking,
and efficient deep-data transformations.

## Phase 51.1: OpenDCX integration and DeepBlur

- [ ] M51.P1.T1 — Vendorize OpenDCX and integrate as a CMake dependency
  - files: `tools/cmake/FindOpenDCX.cmake`, `CMakeLists.txt`, build config
  - approach: Add OpenDCX (github.com/dreamworksanimation/opendcx) as a
    pinned vendored dependency (fetch on build). Ensure it links cleanly
    against the existing OpenEXR and Python bindings. Target glibc 2.34+
    compliance on EL9.
  - verify: Build succeeds with OpenDCX statically linked; a minimal test
    that calls OpenDCX's deep API without errors completes.
  - size: M
- [ ] M51.P1.T2 — Design DeepBlur node: blur on deep pixel data
  - files: `Engine/Nodes/Deep/DeepBlur.cpp`, `.h`, Python bindings
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
- [ ] M51.P1.T3 — Add DeepBlur to the node registry and OFX plugin set
  - files: `HostModel/EffectPluginLoader.cpp`, deep node registry
  - approach: Register DeepBlur as a native node, add category tagging for
    deep nodes, and vendor it in the release AppImage.
  - verify: DeepBlur appears in the Create Node menu under Deep category.
    A project using it loads and renders without errors.
  - size: S

## Phase 51.2: DeepTransform (affine transforms on deep pixels)

- [ ] M51.P2.T1 — Design DeepTransform: scale, rotate, skew deep pixels
  - files: `Engine/Nodes/Deep/DeepTransform.cpp`, `.h`
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
- [ ] M51.P2.T2 — Ship DeepTransform in the deep node registry
  - files: same registry as M51.P1.T3
  - approach: As M51.P1.T3 — register and vendor.
  - verify: Menu presence, load/render verification as above.
  - size: S

## Phase 51.3: Deep utility nodes (QA and Polish)

- [ ] M51.P3.T1 — Add DeepSamples and DeepInfo inspection nodes
  - files: `Engine/Nodes/Deep/DeepInfo.cpp`, `.h`
  - approach: Read-only nodes that output deep-pixel statistics/visualization.
    DeepInfo outputs depth range, sample count per-pixel, coverage, and
    channel list as readable metadata overlays or log output. No knobs,
    pure inspection.
  - verify: Xvfb GUI: connect DeepInfo to a deep image, viewer shows per-pixel
    statistics correctly. CLI: log output matches expected format.
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
