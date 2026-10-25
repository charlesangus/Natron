# M185 - Variable Warps

Exact native I-warps (user, 2026-10-24). Slicing a transform and blending the slices ghosts (a double edge), so geometric I-nodes interpolate the **transform itself** per pixel and resample once. **ITransform**: each output pixel's Map value m gives the transform `T(m)` with translate, rotate, scale, skew and center interpolated from identity at m=0 to the knob values at m=1 (rotation and scale in log space), and samples the source at `T(m)⁻¹(x)`. **ICornerPin**: the four corners interpolate from their "from" to their "to" positions, giving a per-pixel homography. **IDistort and STMap** gain an optional Map input that scales their displacement per pixel. They stay the same nodes, and with the Map unconnected they behave exactly as today (user, 2026-10-25). All go through M113 - Native Warp Nodes' coordinate-map resampler (`Engine/Gpu/Kernels/coordmap.slang`, `Resampler.h` filters).

## Scope

**In:**
- ITransform and ICornerPin as I-variant registrations of Transform and CornerPin's classes, with the M184 - Variable-Size Filter Kit Map convention.
- An optional Map input on IDistort and STMap themselves (no I-variant registrations).
- CPU and GPU paths through the coordinate-map resampler, all base filters, and motion blur where the base node has it.

**Out:**
- Concatenation through an I-warp: it is a per-pixel warp, so it breaks a Transform chain (documented).
- Lens distortion: M132 - Native Lens Distortion.

**Edge cases:**
- Map sampled in output space (the pixel being written), not source space (user); the doc says so.
- A Map edge between 0 and 1 tears the image along it if the transform is large; this is the defined behaviour, and a soft Map is the remedy.
- Filter footprint: the per-pixel Jacobian of `T(m(x))⁻¹` sets the filter footprint, including the Map's gradient term, so steep Maps do not alias.
- RoI: the union of `T(m)⁻¹` of the output RoI over m ∈ [0,1], computed by sampling m at 0, 1 and the interpolation's turning points.
- Motion blur samples interpolate the transform over shutter time as the base node does, then over m.

## Phase 185.1: ITransform and ICornerPin

- [ ] M185.P1.T1 — Write per-pixel transform interpolation
  - files: `Engine/Nodes/Image/TransformMath.h`, `Engine/Nodes/Image/TransformInterp.{h,cpp}` (new), `Tests/Native/TransformInterp_Test.cpp`
  - approach: Engine-free functions: interpolate Transform's parameters (translate linear; rotate linear in angle; scale and skew in log space; center fixed) to a matrix at m, and corner positions to a homography at m; plus the inverse-map Jacobian including the dm/dx term.
  - verify: at the phase build, `ctest -R TransformInterp` passes: m=0 identity, m=1 equals the base matrix, Jacobians match finite differences.
  - size: M

- [ ] M185.P1.T2 — Write ITransform and ICornerPin (CPU)
  - files: `Engine/Nodes/Transform/ITransform.{h,cpp}`, `ITransform.reg.inc`, `Engine/Nodes/Transform/ICornerPin.{h,cpp}`, `ICornerPin.reg.inc`
  - approach: Reuse Transform's and CornerPin's knobs (shared knob builders, `CornerPinKnobs.h`), add `MapInput`. Build a coordinate map per tile from the Map and T1's functions, then resample through the coordinate-map resampler with the chosen filter. Register `NATRON_I_VARIANT` entries.
  - verify: at the phase build, `ctest -R NativeIWarp` (T3) passes.
  - size: L

- [ ] M185.P1.T3 — Test ITransform and ICornerPin
  - files: `Tests/Native/NativeIWarp_Test.cpp`
  - approach: Map 0 is identity; Map 1 matches base Transform/CornerPin within the filter's tolerance; a constant Map 0.5 matches the base node at half parameters; a Map ramp shows no ghosting (single edge, measured by edge count on a line chart). Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeIWarp` passes.
  - size: M

- [ ] M185.P1.T4 — ITransform and ICornerPin on the GPU
  - files: `Engine/Gpu/Kernels/iwarp.slang`, `Engine/Gpu/Kernels/iwarp.cmake`, `Engine/Nodes/Transform/ITransform.cpp`, `Engine/Nodes/Transform/ICornerPin.cpp`, `Tests/Native/NativeIWarpGpu_Test.cpp`
  - approach: A kernel that builds the coordinate map from the Map and pushes it into `coordmap.slang`'s resampler.
  - verify: at the phase build, `ctest -R NativeIWarpGpu` passes on lavapipe within the resampler's existing tolerance.
  - size: M

## Phase 185.2: IDistort and STMap amount

- [ ] M185.P2.T1 — Add an optional Map input to IDistort and STMap
  - files: `Engine/Nodes/Transform/IDistort.{h,cpp}`, `Engine/Nodes/Transform/STMap.{h,cpp}`, `Tests/Native/NativeIDistortAmount_Test.cpp`
  - approach: Append an optional Map input (M184's `MapInput`, after the existing inputs so saved connections keep their indices). When connected, scale the displacement (IDistort) or the offset from identity UV (STMap) by the Map value per pixel, on CPU and GPU (a multiply in the existing coordinate-map build); unconnected, the code path is unchanged.
  - verify: tests: an unconnected Map is bit-identical to before; Map 0 identity, Map 1 equals unmapped, Map 0.5 halves displacement; GPU parity within the existing tolerance.
  - size: M

- [ ] M185.P2.T2 — Icons, cost seeds and docs
  - files: `Gui/Resources/Images/NativeNodes/`, `Engine/Nodes/Transform/*.costseed.inc`, `Engine/Nodes/Transform/docs/{ITransform,ICornerPin}.md`, the Map sections of `docs/IDistort.md` and `docs/STMap.md`
  - approach: Per the native checklist; docs state output-space Map sampling, the tear behaviour, and broken concatenation.
  - verify: `ctest -R NativePluginList` passes; docs reviewed at the gate.
  - size: S

## Decisions

- 2026-10-25 — **Interpolation and Map wording (bosley-native-nodes):** skew blends linearly with the Map amount, matching how Transform's own amount knob blends it, rather than in log space; per-pixel matrices come from exact matrices at 256–512 Map values joined by Hermite interpolation (exact at 0, ½ and 1). The Map knobs' tooltips must not speak of sizes on a warp node; fixed with the docs task.
- 2026-10-25 — **Freshness check (bosley-native-nodes):** references re-verified against the base branch; corrections in `PLAN/DESIGN/2026-10-25-freshness-m122-m192-m202-m178-m185-m20.md` override stale paths, lines and APIs in this file. ITransform/ICornerPin are Custom ops; the branch is rebased onto main before STMap/IDistort GPU work.
- 2026-10-25 — **Interpolate the transform, never blend slices:** slicing a transform ghosts; per-pixel matrices through the coordinate-map resampler are exact.
- 2026-10-25 — **Output-space Map (user):** one lookup, exact, lines up with the result.
- 2026-10-25 — **IDistort and STMap get an optional Map input, not new nodes (user).

**Verification gate:** the full build and ctest pass, including the new suites; GPU parity on lavapipe and RADV. Under Xvfb an ITransform (scale up with a radial Map), an ICornerPin and a Map-scaled IDistort are rendered with Cubic and Impulse filters; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
