# M128 - Native Shape Generators

This milestone rewrites the openfx-misc shape generators as redesigned native nodes:
- **`Ramp`**, **`Radial`** and **`Rectangle`**: three separate **pure generators with no Source input** (user). They share one falloff/ease library, written in C++ with a matching Slang module. Each lerps colour0 → colour1 in scene-linear and has antialiased edges.
- **`ColorWheel`**: linear output only (no gamma), with an edge softness.

Ramp keeps the current OFX overlay: two points, each with a dashed line orthogonal to the line between them (user). It becomes a new host overlay. Radial and Rectangle use the generator extent's rectangle interact.

**ColorBars is not ported and stays on OFX** (user). Noise goes to M129 - Clean-Sheet Noise Node.

Every node:
- has a plain ID at major 1;
- gets the host mask and mix;
- has a GPU kernel on M85 - Native GPU Kernels's generator template;
- keeps no compatibility with the OFX versions.

Registration, kernel lists, icons, docs and OFX retirement go through M120 - Native Node Port Kit's per-node fragment files (`Engine/Nodes/Generator/<Node>.{reg.inc}`, `docs/<Node>.md`, `Engine/Gpu/Kernels/<kernel>.cmake`) and one `Engine/OfxRetired/<ofx-plugin-id>` marker per retired ID.

Deps: M120 - Native Node Port Kit, M85 - Native GPU Kernels (`generators.slang`, `PixelKernelGpu`, `pixelwrap.slang`).

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion). OFX sources are under `build/assets/plugin-src/openfx-misc/`.

**Ramp** (`Ramp/Ramp.cpp`, `net.sf.openfx.Ramp` `:65`)
- Knobs come from `SupportExt/ofxsRamp.cpp:422-531`:
  - `point0` (100,100) and `point1` (100,200), canonical;
  - `color0` (0,0,0,0) and `color1` (1,1,1,1);
  - `type`: Linear default, plus PLinear (t³), EaseIn, EaseOut, Smooth and None;
  - `interactive`.
- `t` is the projection of the pixel onto p0→p1, clamped to [0,1] (`ofxsRamp.h:233`).
- The overlay is `RampOverlayDescriptorOldParams` (`ofxsRamp.h`, set at `Ramp.cpp:784`). It draws two points, each with an orthogonal dashed line.

**Radial** (`Radial/Radial.cpp`)
- The shape is the inscribed ellipse of the generator extent (`btmLeft`/`size`).
- Knobs:
  - `softness` 1 (range 0–1);
  - `plinear`;
  - `color0` and `color1`;
  - `expandRoD`.
- Maths (`:380-450`):
  - `t=(1-r)/softness`.
  - `rampSmooth` is a piecewise-quadratic ease (`:157`).
  - Edge antialiasing comes from the pixel-corner distances.
- The overlay is the generator rectangle (`generatorDescribe`, `:955`).

**Rectangle** (`Rectangle/Rectangle.cpp`)
- The shape is the extent box.
- Knobs:
  - `cornerRadius` 2D;
  - `softness` 0 (pixels, falling off inward);
  - `color0` and `color1`;
  - `expandRoD`.
- Maths: `t = rampSmooth(dx/softness.x)·rampSmooth(dy/softness.y)`.

**ColorWheel** (`ColorWheel/ColorWheel.cpp`)
- Knobs:
  - `centerSaturation` 0 and `edgeSaturation` 1;
  - `centerValue` 1 and `edgeValue` 1;
  - `gamma` 0.45;
  - `rotate` 0.
- The output is an HSV disc: hue from the angle, S and V lerped radially, then `pow(c, 1/gamma)` (`:290-330`).
- The radius is the RoD minus a 1 px black border, with an antialiased alpha edge.

**ColorBars** (`ColorBars/ColorBars.cpp`) is display-referred SMPTE bars. It is not ported.

**Native generator infrastructure:**
- `Engine/Nodes/Image/NativeGenerator.h:96`:
  - `render` is FINAL (`NativeGenerator.cpp:373`).
  - Kernels get `RowIO` with `nSrc=0`, `x0` and `y`.
  - The channel writer is `GeneratorRowChannels` (`.h:44`).
- Extent policy: `ExtentKnobs.h:58-70`. The rectangle interact is declared at `ExtentKnobs.cpp:302`.
- Templates:
  - `Generator/Constant.cpp:39-63` (constant kernel);
  - `CheckerBoard.cpp:361-431` (position-dependent kernel using `getExtentRegionOfDefinition` and `mappedScale`).
- Single-point handles: `KnobDouble::setHasHostOverlayHandle(true)` (`Engine/KnobTypes.cpp:247`).
- Host overlay API:
  - `Node::addPositionInteract`, `addRectangleInteract`, `addTransformInteract` and `addCornerPinInteract` (`Engine/Node.h:1421-1460`);
  - implementations in `Engine/NodeOverlay.cpp:296-430`;
  - knob bundles in `Engine/HostOverlaySupport.h`;
  - interacts in `Gui/HostOverlay.cpp:1070-1100`.
- **There is no two-point (ramp) host overlay.**

**GPU (M85 - Native GPU Kernels):**
- `Engine/Gpu/Kernels/generators.slang` defines `struct ConstantOp : IPixelOp { load(params); apply(float4 v, int2 xy) }` and `runWrapped<Op>`.
- `Constant` overrides `isPointOp()`, `gpuKernelId` and `makeGpuKernel` (sharing `readColor` with `makeKernel`), guarded by `NATRON_HAVE_GPU`.
- Parity test: `Tests/Native/NativeConstantGpu_Test.cpp`.
- Position-dependent ops need the extent and the scale in their params.

**References:**
- `Engine/EffectInstance.h:53` defines `PLUGINID_OFX_RADIAL`, which has no users.
- Tests:
  - `Tests/LayerKnobs_Test.cpp:282-289`: Ramp, Radial, Rectangle, ColorBars, ColorWheel, Noise and CImgPlasma.
  - `Tests/GeneratorLayer_Test.cpp:67, 316`: ColorWheel and Ramp.
  - `Tests/Metadata_Test.cpp:89`: ColorWheel.
- Tools:
  - `tools/bench/make_plates.py:133`: Ramp.
  - `tools/bench/comps/{cg_multipass,keying_grade,defocus_retime}.ntp`: Ramp, Rectangle and Radial.
- There are no PyPlug or menu references.

## Scope

**In:**
- Native `Ramp`, `Radial`, `Rectangle` and `ColorWheel` as pure generators (`NativeGenerator`).
- A shared falloff library: a C++ header and a Slang module with identical curves.
- A new host ramp overlay matching the OFX one.
- A GPU kernel and parity test per node.
- Retiring the OFX Ramp, Radial, Rectangle and ColorWheel.

**Out:**
- ColorBars: stays OFX (user). No milestone.
- Noise, Plasma, Rand and CImg Noise → M129 - Clean-Sheet Noise Node.
- Compositing over a Source input: dropped (user). Use a Merge.
- Renaming Constant/CheckerBoard IDs → M111 - Native Node Identity Cleanup.

**Edge cases:**
- Ramp with p0 == p1: the output is colour1 on p1's side, as a step. It is never a divide by zero.
- Radial/Rectangle softness 0: a hard edge, still antialiased over one pixel.
- Rectangle corner radius larger than half the box: clamped to half the shorter side.
- Values outside [0,1] in colour0/colour1: passed through unclamped (scene-linear).
- Render scale below 1: shapes are evaluated in canonical space, so a proxy render matches a downscaled full render.
- ColorWheel alpha outside the disc: 0, with no 1 px black border; the antialiased edge plus softness replaces it.

**Assumptions:**
- Unconfirmed: the shared ease vocabulary is Linear, Smooth (smoothstep), EaseIn, EaseOut and Quadratic (Radial's `rampSmooth`), plus a "plinear" cube option dropped as display-era. Each node exposes a `falloff` choice.
- Unconfirmed: Radial's and Rectangle's softness is in canonical pixels for both, not Radial's 0–1 fraction, so the two behave alike.
- Unconfirmed: ColorWheel keeps its centre/edge saturation and value plus rotate, gains `softness`, and outputs linear HSV-derived RGB with no gamma.
- Unconfirmed: `expandRoD` is dropped. The RoD is the extent, or the extent plus softness for Rectangle.

## Phase 127.1: Shared pieces

- [ ] M128.P1.T1 — Write the shared falloff library in C++ and Slang
  - files: `Engine/Nodes/Generator/ShapeFalloff.h` (new), `Engine/Gpu/Kernels/shapefalloff.slang` (new), `Tests/Native/ShapeFalloff_Test.cpp` (new)
  - approach:
    - Pure functions shared by all three nodes:
      - `ease(t, Falloff)` for the curves in Scope's assumption;
      - `rampT(p, p0, p1)`;
      - `ellipseT(p, box, softness)`;
      - `roundedBoxT(p, box, cornerRadius, softness)`;
      - a pixel-footprint antialias helper that integrates coverage over the pixel square, replacing the OFX corner-distance trick.
    - Mirror them line for line in Slang.
  - verify: the test checks curve endpoints, monotonicity and symmetry. It compares each helper against hand values. A small GPU entry checks C++/Slang parity within 1e-6. It runs at the phase build.
  - size: M

- [ ] M128.P1.T2 — Add a host ramp overlay matching the OFX one
  - files: `Engine/HostOverlaySupport.h`, `Engine/NodeOverlay.cpp`, `Engine/Node.h`, `Gui/HostOverlay.cpp`, `Tests/gui/m127_ramp_overlay.py` (new)
  - approach:
    - Add `HostOverlayKnobsRamp { point0, point1, interactive? }` and `Node::addRampInteract(point0, point1)`.
    - Add a `RampInteract` in `Gui/HostOverlay.cpp` that ports the drawing and picking from openfx-misc `SupportExt/ofxsRamp.{h,cpp}`: two draggable points, each with a dashed line orthogonal to p0→p1 through it.
    - Scale by `getScreenPixelRatio()` like the other host overlays.
    - Use the global interactive toggle when `interactive` is null.
  - verify: the Xvfb script creates a Ramp, drags each point and checks the knob values change. A screenshot of the overlay is shared before sign-off.
  - size: L

## Phase 127.2: Nodes

- [ ] M128.P2.T1 — Add the native Ramp
  - files: `Engine/Nodes/Generator/Ramp.{h,cpp}` (new), `Engine/Nodes/Generator/Ramp.reg.inc`, `Tests/Native/NativeRamp_Test.cpp` (new)
  - approach:
    - `Ramp` v1 on `NativeGenerator`.
    - Knobs: `point0` and `point1` (canonical), `color0`, `color1` and `falloff`. The overlay comes from P1.T2.
    - The kernel uses `rampT` and `ease`, then the lerp.
    - The default extent is the project format.
  - verify: the test checks horizontal, vertical and diagonal ramps against hand values, each falloff, p0 == p1, HDR colours passing through, and render-scale invariance. It runs at the phase build.
  - size: M

- [ ] M128.P2.T2 — Add the native Radial and Rectangle
  - files: `Engine/Nodes/Generator/Radial.{h,cpp}` (new), `Engine/Nodes/Generator/Rectangle.{h,cpp}` (new), `Engine/Nodes/Generator/{Radial,Rectangle}.reg.inc`, `Tests/Native/NativeShapes_Test.cpp` (new)
  - approach:
    - Both are v1 on `NativeGenerator`, using the extent box from `ExtentKnobs` with its rectangle interact.
    - Shared knobs: `softness`, `falloff`, `color0` and `color1`. Rectangle also has `cornerRadius`.
    - Kernels use `ellipseT`/`roundedBoxT` plus the antialias helper.
  - verify: the test covers the softness-0 antialiased edge, softness falloff, corner radius clamping, non-square boxes, HDR colours, and render-scale invariance. It runs at the phase build.
  - size: L

- [ ] M128.P2.T3 — Add the native ColorWheel, linear only
  - files: `Engine/Nodes/Generator/ColorWheel.{h,cpp}` (new), `Engine/Nodes/Generator/ColorWheel.reg.inc`, `Tests/Native/NativeColorWheel_Test.cpp` (new)
  - approach:
    - `ColorWheel` v1 on `NativeGenerator`.
    - Knobs: centre/edge saturation and value, `rotate` and `softness`. There is no gamma.
    - The kernel computes HSV → RGB in linear and takes alpha from the antialiased disc edge.
  - verify: the test checks hue at known angles, centre and edge values, rotate, alpha 0 outside the disc, and no gamma applied. It runs at the phase build.
  - size: M

## Phase 127.3: GPU

- [ ] M128.P3.T1 — Run Ramp, Radial, Rectangle and ColorWheel on the GPU
  - files: `Engine/Gpu/Kernels/shapes.slang` (new), `Engine/Gpu/Kernels/shapes.cmake`, `Engine/Nodes/Generator/Ramp.cpp`, `Engine/Nodes/Generator/Radial.cpp`, `Engine/Nodes/Generator/Rectangle.cpp`
  - approach:
    - One Slang file with an `IPixelOp` per node, built on `shapefalloff.slang` and run through `runWrapped`, following M85 - Native GPU Kernels's `generators.slang`.
    - Each node's `makeGpuKernel` packs the extent, scale and knobs into a ≤128 B params block.
    - Add ColorWheel's `makeGpuKernel` in the same pattern; put its file in this task if the count allows, else in P3.T2.
  - verify: P3.T2's parity test.
  - size: L

- [ ] M128.P3.T2 — Add GPU parity tests for the shape generators, and ColorWheel's GPU hook
  - files: `Tests/Native/NativeShapesGpu_Test.cpp` (new), `Engine/Nodes/Generator/ColorWheel.cpp`
  - approach:
    - Use the NativeGpuParity harness over P2's fixtures for all four nodes.
    - The tolerance is 1e-5 absolute (the curves are identical by construction).
  - verify: the test passes at the phase build on RADV.
  - size: M

## Phase 127.4: Retire the OFX generators

- [ ] M128.P4.T1 — Retire the OFX Ramp, Radial, Rectangle and ColorWheel and fix their references
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (four markers), `Engine/EffectInstance.h` (delete the unused `PLUGINID_OFX_RADIAL`), `Tests/LayerKnobs_Test.cpp`, `Tests/GeneratorLayer_Test.cpp`
  - approach:
    - Add an `Engine/OfxRetired/<ofx-plugin-id>` marker for each of the four OFX IDs.
    - Move the test cases to the native IDs. ColorBars cases stay (it remains OFX); Noise and Plasma cases stay until M129 - Clean-Sheet Noise Node retires them.
    - The remaining references (metadata test, bench plates and comps) are P4.T2.
  - verify: a test asserts the retired IDs fail to load and the native ones do. The full ctest passes at the gate.
  - size: M

- [ ] M128.P4.T2 — Move the remaining generator references to the native IDs
  - files: `Tests/Metadata_Test.cpp`, `tools/bench/make_plates.py`, `tools/bench/comps/build_comps.py` (and regenerate `tools/bench/comps/{cg_multipass,keying_grade,defocus_retime}.ntp` with it)
  - approach:
    - Swap the OFX Ramp/Radial/Rectangle/ColorWheel for the native nodes and their knob names.
    - Regenerate the comps with the script rather than hand-editing them.
  - verify: the comps load with no missing-plugin node under `tools/bench/` at the gate.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including the new native and GPU parity tests on RADV.
- An Xvfb GUI check, with screenshots shared before sign-off, covering:
  - the Ramp overlay (two points with orthogonal dashed lines, dragging);
  - Radial and Rectangle with softness;
  - ColorWheel.
- An AppImage built with `package.sh` for UAT.
- The retired OFX IDs no longer load, and ColorBars still does.

## Decisions

- 2026-10-13 — **Three separate pure generators (user, native-node port grill):** Ramp, Radial and Rectangle stay separate nodes with no Source input; users Merge them over a plate.
- 2026-10-13 — **Ramp keeps the current overlay (user):** two points, each with a dashed line orthogonal to the line between them. It becomes a host overlay because no two-point host overlay exists.
- 2026-10-13 — **Only ColorWheel is ported from the test patterns, in linear mode only (user):** ColorBars stays OFX.
- 2026-10-13 — **Noise is its own milestone (user):** M129 - Clean-Sheet Noise Node.
