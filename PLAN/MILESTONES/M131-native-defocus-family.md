# M131 - Native Defocus Family

New native nodes with no OFX source:
- **A shared bokeh kernel library.** Disc, n-blade (blade count, rotation, roundness) and image-input kernels. It is scene-linear, with an optional highlight boost (threshold and gain).
- **`Defocus`**, at a uniform size.
- ~~**`IBlur`** and **`IDefocus`**~~ moved to E15 - Loops And I-Nodes (user, 2026-10-25): IBlur to M184 - Variable-Size Filter Kit, IDefocus to M186 - Exact IDefocus, which reuses this milestone's bokeh kernel library.
- **`ZDefocus`**, which turns a depth channel into a circle of confusion (CoC). It uses a layered gather to limit edge bleeding and has a focal-plane overlay and output.
  - Its **real-world lens mode** sets the disc size from focal length, f-stop and film back (user).
  - A `depthUnits` knob sets the depth units.

Every node follows the native conventions:
- plain ID at major 1;
- host mask, mix and (un)premult from the base class;
- a CPU kernel, a GPU kernel, a native test, a GPU parity test, an icon and a size-scaled `gpuWeight()` (cost grows with the defocus size);
- registration through M120 - Native Node Port Kit's per-node fragment files, and retirement (none here: these are new nodes) would add `Engine/OfxRetired/<ofx-plugin-id>` markers.

Deps: M120 - Native Node Port Kit.

Scouted facts (scouted 2026-10-13 on `main` `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **No defocus code exists.** No Defocus, ZDefocus or VectorBlur exists anywhere in the tree or the OFX bundle. `defocus_retime.ntp` names a node "Defocus", but it is a Blur.
- **Native spatial infrastructure:** `Engine/Nodes/Filter/SpatialFilterSupport.h` (`PlaneJob` `:125`, `MaskInput` `:150`, `WindowLayout` `:228`, `writeWindow` `~:250`) and `BlurKernels.h` (line filters, `Filter` enum `:59`).
- **Blur's GPU path:**
  - It is at `Blur.cpp:894-990`, with the helpers in `Engine/Gpu/GpuBlur.h` (`kGpuBlurMaxRadius` 512).
  - `blur.slang` stages a shared-memory tile (`blurPass` at `:115`).
  - The mask and mix refusal at `Blur.cpp:897` must not be copied. Use M85 - Native GPU Kernels's `pixelwrap.slang` finish pass.
- **Overlays:** `Node::addPositionInteract` (`Engine/Node.h:1421-1460`) and `KnobDouble::setHasHostOverlayHandle` (`Engine/KnobTypes.cpp:247`) give a point handle, which the focal-point picker uses.
- **Layers:** depth comes from any layer through `Node::declareLayerKnob(knob, inputNb, role)` (`Engine/Node.h:1626`), the same mechanism Merge uses for input-bound layer selects.
- **GPU interface:**
  - `GpuRenderArgs::inputs` is indexed by input number (`Engine/Nodes/Image/GpuRender.h`), so a second input (the kernel image, or the size map) works at the interface.
  - Kernels are Slang compiled offline. Storage buffers go in set 0, and push constants are at most 128 B (`Engine/Gpu/GpuKernel.h:16-19`).

## Scope

**In:**
- The bokeh kernel library.
- `Defocus` and `ZDefocus`, including ZDefocus's real-world lens mode.
- CPU and GPU kernels and parity tests for both.

**Out:**
- Deep input to ZDefocus: E8 - Deep Compositing.
- Cat-eye or optical vignetting, chromatic aberration, anamorphic squeeze, and textured kernels: M133 - ZDefocus Lens Aberrations.
- Motion-vector blur. No milestone owns it.
- Sharpen and Glow: M123 - Native Sharpen And Glow.
- IBlur and IDefocus: M184 - Variable-Size Filter Kit and M186 - Exact IDefocus.
- Median, Bilateral and DirBlur: M130 - Native Edge-Preserving And Directional Filters.

**Edge cases:**
- **Depth convention:** `depthUnits` chooses Distance or 1/Distance (Nuke's default). Depth ≤ 0 or non-finite is treated as infinitely far.
- **Size 0 or CoC below half a pixel:** identity for that pixel. A node whose maximum size is 0 is identity as a whole.
- **Highlight boost:** applied before the gather and undone after it, so out-of-focus highlights bloom into bokeh shapes without changing in-focus pixels.
- **Edge bleeding:** ZDefocus splits the CoC range into depth layers and gathers front to back with occlusion. Sharp foreground over a blurred background must not halo.
- **Lens mode:** CoC (mm on the film back) is `f² / (N · (S − f)) · |D − S| / D`, converted to pixels via film-back width ÷ image width. S is focus distance, D is depth, f is focal length, N is f-stop. A depth closer than the focal length clamps to the maximum size.
- **Image kernel:** normalised to unit sum. An empty or black kernel image is identity, with a node warning.

**Assumptions:**
- Unconfirmed: the default bokeh is a disc with 6 blades available, roundness 0 and rotation 0.
- Unconfirmed: the maximum size knob is capped at 500 px. The GPU path handles up to that with a tiled gather, and larger sizes fall back to the CPU.
- Unconfirmed: the focal-plane output is a separate "focal plane" view mode (in focus, front and back colour-coded), not an extra layer.

## Phase 130.1: Bokeh kernel library and Defocus

- [ ] M131.P1.T1 — Write the bokeh kernel library
  - files: `Engine/Nodes/Filter/BokehKernel.h`, `Engine/Nodes/Filter/BokehKernel.cpp`, `Tests/Native/BokehKernel_Test.cpp`
  - approach:
    - Generate a normalised 2-D kernel for a given radius from {disc, n-blade (blades, rotation, roundness), image}, with sub-pixel anti-aliased edges.
    - Provide a highlight boost and unboost pair (`threshold`, `gain`) as point functions.
    - Keep it engine-free so the GPU host side can upload the same weights.
    - Tests:
      - Each kernel sums to 1.
      - An n-blade at roundness 1 matches the disc.
      - The boost is undone exactly by the unboost.
  - verify: at the phase build, `ctest -R BokehKernel` passes.
  - size: M

- [ ] M131.P1.T2 — Write the native Defocus node (CPU)
  - files: `Engine/Nodes/Filter/Defocus.h`, `Engine/Nodes/Filter/Defocus.cpp`, `Engine/Nodes/Filter/Defocus.reg.inc`
  - approach:
    - Inputs: Source and an optional Kernel image.
    - Knobs: `size`, `kernelType`, blade knobs, `highlightThreshold`, `highlightGain`.
    - Boost, then a direct 2-D gather with the bokeh kernel over plane jobs, then unboost. Expand the RoI by `size`.
  - verify: at the phase build, `ctest -R NativeDefocus` (T3) passes.
  - size: M

- [ ] M131.P1.T3 — Test Defocus on CPU
  - files: `Tests/Native/NativeDefocus_Test.cpp`
  - approach:
    - Size 0 is identity.
    - A point source reproduces the kernel's shape.
    - A constant is preserved.
    - A bright highlight with boost on grows a visible disc.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeDefocus` passes.
  - size: M

- [ ] M131.P1.T4 — Defocus on the GPU
  - files: `Engine/Gpu/Kernels/defocus.slang`, `Engine/Gpu/Kernels/defocus.cmake`, `Engine/Nodes/Filter/Defocus.cpp`, `Tests/Native/NativeDefocusGpu_Test.cpp`
  - approach: run a tiled 2-D gather with kernel weights in a storage buffer, plus the boost and unboost point ops. Refuse above the size cap.
  - verify: at the phase build, `ctest -R NativeDefocusGpu` passes on RADV within 1e-4 relative.
  - size: L

## Phase 130.2: Variable-size IBlur and IDefocus

- [ ] ~~M131.P2.T1 — Write the native IBlur and IDefocus nodes (CPU)~~ — cancelled 2026-10-25, moved to E15 - Loops And I-Nodes (M184 - Variable-Size Filter Kit, M186 - Exact IDefocus).
  - files: `Engine/Nodes/Filter/VariableDefocus.h`, `Engine/Nodes/Filter/VariableDefocus.cpp`, `Engine/Nodes/Filter/VariableDefocus.reg.inc`
  - approach:
    - One class, two registrations (`IBlur` with a Gaussian kernel, `IDefocus` with the bokeh kernel). Input 1 is a size map, read through a layer/channel knob.
    - The per-pixel size is `map × size`. Gather as scatter-as-gather: each output pixel takes contributions from neighbours whose own radius reaches it, normalised by the accumulated weight. A large blur therefore doesn't leak into sharp neighbours.
  - verify: at the phase build, `ctest -R NativeVariableDefocus` (T2) passes.
  - size: L

- [ ] ~~M131.P2.T2 — Test IBlur and IDefocus on CPU~~ — cancelled 2026-10-25, moved to E15 - Loops And I-Nodes (M184 - Variable-Size Filter Kit, M186 - Exact IDefocus).
  - files: `Tests/Native/NativeVariableDefocus_Test.cpp`
  - approach:
    - A constant map equals uniform Blur or Defocus at that size, within tolerance.
    - A zero map is identity.
    - A half-and-half map blurs only one side, with no leak across the boundary beyond the stated radius.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeVariableDefocus` passes.
  - size: M

- [ ] ~~M131.P2.T3 — IBlur and IDefocus on the GPU~~ — cancelled 2026-10-25, moved to E15 - Loops And I-Nodes (M184 - Variable-Size Filter Kit, M186 - Exact IDefocus).
  - files: `Engine/Gpu/Kernels/variabledefocus.slang`, `Engine/Gpu/Kernels/variabledefocus.cmake`, `Engine/Nodes/Filter/VariableDefocus.cpp`, `Tests/Native/NativeVariableDefocusGpu_Test.cpp`
  - approach: implement the same scatter-as-gather over a tile with an apron sized by the map's maximum in the tile. The map is GPU input 1.
  - verify: at the phase build, `ctest -R NativeVariableDefocusGpu` passes on RADV within 1e-4 relative.
  - size: L

## Phase 130.3: ZDefocus

- [ ] M131.P3.T1 — Write the depth-to-CoC model, with the manual and real-world lens modes
  - files: `Engine/Nodes/Filter/DepthOfField.h`, `Engine/Nodes/Filter/DepthOfField.cpp`, `Tests/Native/DepthOfField_Test.cpp`
  - approach:
    - Engine-free functions. Manual mode: `focalPlane`, `depthOfField` and `size`, giving a pixel radius. Lens mode: focal length (mm), f-stop, film-back width (mm), focus distance, image width, using the formula in `## Scope`.
    - Handle both depth conventions and the invalid-depth rule.
    - Tests:
      - Manual mode gives zero inside the depth of field.
      - Lens mode matches hand-computed CoCs for a 50 mm f/2 lens on a 36 mm back at 2 m focus.
      - Doubling the f-stop halves the CoC.
  - verify: at the phase build, `ctest -R DepthOfField` passes.
  - size: M

- [ ] M131.P3.T2 — Write the native ZDefocus node with a layered gather (CPU)
  - files: `Engine/Nodes/Filter/ZDefocus.h`, `Engine/Nodes/Filter/ZDefocus.cpp`, `Engine/Nodes/Filter/ZDefocus.reg.inc`
  - approach:
    - Inputs: Source and an optional Kernel image.
    - Knobs:
      - `depth` layer/channel (input-bound layer knob);
      - `depthUnits`;
      - `mode` (Manual, Lens) with each mode's knobs;
      - `focalPoint`, a position overlay that samples depth to set the focus distance;
      - `layers` (depth slices, default 16);
      - the bokeh and highlight knobs;
      - `output` (Result, Focal plane).
    - Slice the CoC range into layers. Gather each slice with its own kernel radius, back to front, compositing with occlusion so in-focus foreground is not haloed.
  - verify: at the phase build, `ctest -R NativeZDefocus` (T3) passes.
  - size: L

- [ ] M131.P3.T3 — Test ZDefocus on CPU
  - files: `Tests/Native/NativeZDefocus_Test.cpp`
  - approach:
    - Depth all at the focal plane gives identity.
    - Constant depth equals Defocus at the matching size.
    - A sharp foreground over a far background shows no halo beyond one pixel at the edge.
    - Focal-plane output marks the right regions.
    - Lens mode at f/16 is sharper than at f/2.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeZDefocus` passes.
  - size: M

- [ ] M131.P3.T4 — ZDefocus on the GPU
  - files: `Engine/Gpu/Kernels/zdefocus.slang`, `Engine/Gpu/Kernels/zdefocus.cmake`, `Engine/Nodes/Filter/ZDefocus.cpp`, `Tests/Native/NativeZDefocusGpu_Test.cpp`
  - approach:
    - Run the per-slice gather plus an occlusion-composite kernel. The slices are resident buffers from M84 - GPU Placement And Residency's pools.
    - Reuse Defocus's tiled gather.
  - verify: at the phase build, `ctest -R NativeZDefocusGpu` passes on RADV within 1e-3 relative. The slice compositing order makes summation order matter, so state that.
  - size: L

## Phase 130.4: Documentation

- [ ] M131.P4.T1 — Document the nodes
  - files: `Engine/Nodes/Filter/docs/{Defocus,ZDefocus}.md`
  - approach: one doc file per node (M120 - Native Node Port Kit's scheme), the kernel library described in `Defocus.md`: knobs, the lens formula, the depth conventions, GPU caps and tolerances.
  - verify: reviewed at the gate against the node code.
  - size: S

## Decisions

- 2026-10-13 — **Scope of v1 (user, /cat-plan grill):** a solid core with the bokeh kernels (disc, n-blade, image), highlight boost, Defocus, IBlur, IDefocus and ZDefocus with a layered gather and a focal-plane output, GPU for all, and no deep input.
- 2026-10-13 — **ZDefocus real-world lens mode (user):** the disc size can be set from focal length, f-stop and film back.
- 2026-10-13 — **Aberrations split out (user):** cat-eye, chromatic aberration and anamorphic go to a follow-up, M133 - ZDefocus Lens Aberrations.
- 2026-10-13 — **IBlur and IDefocus (user):** filter size is driven by a mask input.
- 2026-10-25 — **IBlur and IDefocus moved out (user):** to E15 - Loops And I-Nodes, so every I-node shares one Map convention; Phase 130.2 is cancelled. Keep `BokehKernel` engine-free and reusable, since M186 - Exact IDefocus builds on it.

**Verification gate:** the full build and `ctest` pass. Every `Native*Gpu` parity test in this milestone passes on RADV. Under Xvfb, Defocus (disc, 6-blade and image kernels) and ZDefocus (Manual and Lens modes, plus the focal-plane output) are rendered on a CG fixture with a depth pass. The ZDefocus focal-point overlay is dragged. Screenshots are shared with the user before sign-off. An AppImage built with `package.sh` is handed over for UAT.
