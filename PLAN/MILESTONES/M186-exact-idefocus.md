# M186 - Exact IDefocus

Exact per-pixel defocus (user, 2026-10-24: IBlur and IDefocus moved out of M131 - Native Defocus Family into E15 - Loops And I-Nodes). Map blend would blend two bokeh sizes into a double ring on highlights, so IDefocus gathers with the true per-pixel bokeh. Each output pixel takes contributions from neighbours whose own radius (`lerp(sizeAtBlack, size, Map)`) reaches it, normalised by the accumulated weight (scatter-as-gather), using M131's bokeh kernel library (disc, n-blade, image kernels, highlight boost). ZDefocus in M131 is depth-driven and stays there.

## Scope

**In:**
- IDefocus as an I-variant of Defocus's class, with the M184 - Variable-Size Filter Kit Map convention and Defocus's bokeh and highlight knobs.
- CPU and GPU scatter-as-gather; tests; docs.

**Out:**
- ZDefocus and its layered gather: M131 - Native Defocus Family.
- Lens aberrations: M133 - ZDefocus Lens Aberrations.

**Edge cases:**
- Map 0 at a pixel with no neighbour reaching it: the input exactly.
- A sharp pixel beside a large-radius neighbour receives that neighbour's spread (scatter semantics), which is what makes highlights bloom over sharp areas; the doc contrasts this with IBlur's gather.
- Highlight boost is applied before the gather and undone after, as in Defocus.
- Size above the GPU cap falls back to CPU, as in Defocus.

## Phase 186.1: IDefocus

- [ ] M186.P1.T1 — Write the native IDefocus node (CPU)
  - files: `Engine/Nodes/Filter/IDefocus.h`, `Engine/Nodes/Filter/IDefocus.cpp`, `Engine/Nodes/Filter/IDefocus.reg.inc`
  - approach: Reuse Defocus's knobs and `BokehKernel`. Per tile, read the Map with an apron of the maximum size; for each output pixel, gather over the apron the bokeh-weighted contributions of neighbours whose radius reaches it, normalised by summed weight. Register `NATRON_I_VARIANT(Defocus, IDefocus, …)`.
  - verify: at the phase build, `ctest -R NativeIDefocus` (T2) passes.
  - size: L

- [ ] M186.P1.T2 — Test IDefocus on CPU
  - files: `Tests/Native/NativeIDefocus_Test.cpp`
  - approach: A constant Map equals Defocus at that size within tolerance; a zero Map is identity; a half-and-half Map with a highlight on the defocused side spreads no further than the stated radius; a constant input stays constant. Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeIDefocus` passes.
  - size: M

- [ ] M186.P1.T3 — IDefocus on the GPU
  - files: `Engine/Gpu/Kernels/idefocus.slang`, `Engine/Gpu/Kernels/idefocus.cmake`, `Engine/Nodes/Filter/IDefocus.cpp`, `Tests/Native/NativeIDefocusGpu_Test.cpp`
  - approach: The same scatter-as-gather over a tile, apron sized by the Map's maximum in the tile (`MapInput::tileRange`), kernel weights in a storage buffer.
  - verify: at the phase build, `ctest -R NativeIDefocusGpu` passes on RADV within 1e-4 relative.
  - size: L

- [ ] M186.P1.T4 — Icon, gpuWeight (size-scaled) and doc
  - files: `Gui/Resources/Images/NativeNodes/`, `Engine/Nodes/Filter/IDefocus.cpp`, `Engine/Nodes/Filter/docs/IDefocus.md`
  - approach: Per the native checklist.
  - verify: `ctest -R NativePluginList` passes; doc reviewed at the gate.
  - size: S

## Decisions

- 2026-10-25 — **Moved from M131 - Native Defocus Family (user):** IBlur went to M184 - Variable-Size Filter Kit and IDefocus here, so all I-nodes share one Map convention.

**Verification gate:** the full build and ctest pass, including both IDefocus suites (GPU on lavapipe and RADV). Under Xvfb IDefocus with disc, 6-blade and image kernels and a roto Map is rendered on a highlight-rich fixture; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
