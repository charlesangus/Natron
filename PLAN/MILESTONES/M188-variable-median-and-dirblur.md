# M188 - Variable Median And DirBlur

Exact I-variants of M130 - Native Edge-Preserving And Directional Filters' Median and DirBlur (user, 2026-10-24). **IMedian**: per-pixel window radius `Map · size`, with M130's relative threshold. **IDirBlur**: per-pixel blur length `Map · length` in every DirBlur mode — Directional, Radial (spin), Zoom and GodRays — which also gives IRadialBlur and IZoomBlur. A directional blur is a line integral per pixel, so a per-pixel length costs nothing extra.

## Scope

**In:**
- IMedian and IDirBlur as I-variant registrations, with the Map convention from M184 - Variable-Size Filter Kit, CPU and GPU paths, tests, icons and docs.

**Out:**
- Bilateral: Map-driving its sigmas is left to M183 - Loop Map Blend's universal mode.
- An EdgeBlur-style soften: no milestone owns it.
- A two-channel Map setting direction per pixel (a VectorBlur): M191 - Native VectorBlur.

**Edge cases:**
- IMedian at fractional radius: the result blends the floor and ceil window medians.
- IMedian cost is O(r²) per pixel in the worst case (histogram-free); the GPU cap matches Median's and larger sizes fall back to CPU.
- IDirBlur Map 0 is identity at that pixel in every mode.

## Phase 188.1: Nodes

- [ ] M188.P1.T1 — Write IMedian (CPU and GPU)
  - files: `Engine/Nodes/Filter/Median.cpp`, `Engine/Nodes/Filter/IMedian.reg.inc`, `Engine/Gpu/Kernels/median.slang`, `Tests/Native/NativeIMedian_Test.cpp`
  - approach: A mode flag on Median's class reads the per-pixel radius from the Map and selects the window per pixel; fractional radii blend the two window medians.
  - verify: tests: Map 0 identity; Map 1 equals Median at that size; a Map ramp is monotonic in smoothing; GPU parity bit-exact on lavapipe.
  - size: L

- [ ] M188.P1.T2 — Write IDirBlur (CPU and GPU)
  - files: `Engine/Nodes/Filter/DirBlur.cpp`, `Engine/Nodes/Filter/IDirBlur.reg.inc`, `Engine/Gpu/Kernels/dirblur.slang`, `Tests/Native/NativeIDirBlur_Test.cpp`
  - approach: The per-pixel line integral takes its length (and for Radial its angle, for Zoom its scale) from `Map ·` the knob value.
  - verify: tests per mode: Map 0 identity; Map 1 equals DirBlur; GPU parity on lavapipe within DirBlur's tolerance.
  - size: L

- [ ] M188.P1.T3 — Icons, gpuWeight (size-scaled) and docs
  - files: `Gui/Resources/Images/NativeNodes/`, `Engine/Nodes/Filter/IMedian.cpp`, `Engine/Nodes/Filter/IDirBlur.cpp`, `Engine/Nodes/Filter/docs/{IMedian,IDirBlur}.md`
  - approach: Per the native checklist; the IDirBlur doc shows each mode.
  - verify: `ctest -R NativePluginList` passes; docs reviewed at the gate.
  - size: S

## Decisions

- 2026-10-25 — **Radial and zoom come as IDirBlur modes:** M130's DirBlur already has Directional, Radial, Zoom and GodRays modes.
- 2026-10-25 — **VectorBlur is its own milestone (user):** M191 - Native VectorBlur.

**Verification gate:** the full build and ctest pass, including the new suites (GPU on lavapipe and RADV). Under Xvfb IMedian on a noisy plate and IDirBlur in Zoom and Radial modes with a radial Map are rendered; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
