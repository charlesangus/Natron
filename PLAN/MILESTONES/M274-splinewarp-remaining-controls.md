# M274 - SplineWarp Remaining Controls

Port the rest of Nuke 17's SplineWarp controls, so every Nuke control is either implemented with Nuke's default, covered by M147–M152, or dropped with the user's sign-off. M147 - SplineWarp Node Core and M148 - SplineWarp Viewer Tools cover the rest. The Nuke reference is `.local/docs/nuke/html/content/reference_guide/transform_nodes/splinewarp.html`.

Rules:
- Work is built on the most complete branch. M151 - SplineWarp Motion Blur carries M147, M149 and M150, and M148 has the viewer tools.
- Knob labels are readable script names.
- No (un)premult-by knob and no backward compatibility.
- Panels fit 1920x1080.
- Anything that changes the resample has CPU/GPU parity in `Engine/Gpu/Kernels/splinewarp.slang`.

Deps: M147, M148, M149, M150, M151. P5.T2 alone also waits on M270 - Lanczos Sinc Resampling Filters.

## Scope

**In:** every row of the audit table marked with a task:
- per-layer and per-link amounts
- honouring Activated
- soft boundaries, the format boundary, and the boundary curve resolution
- the Transform page: per curve and per layer, transform links, and the viewer overlay
- cookie cutters, with cut input and output mask
- crop to format
- preview resolution
- the Lanczos/Sinc filters, through M270

Each task carries its own Python bindings and serialization.

**Out:**
- **Morph:** dropped by the user's M147 decision ("Morphing, dissolving and a second input → none (user: out of scope)"). That covers output modes other than Source/Source Warped, the morph `mix`, input B, the A/B/AB Type column and "Duplicate in B".
- **Point-level track links and Copy/Paste between nodes:** these go to M152 - Curve Track Links And Roto Copy.
- **Duplicate, Link (Nuke's Join) and the role columns:** these go to M148 - SplineWarp Viewer Tools.
- **Nuke's OpenGL "persistent preview":** M148's draft field is its analogue.

**Edge cases:**
- A cookie cutter must be a closed curve, and it may also be linked. Opening the curve clears the flag.
- `outputMask` and `cookieCutInput` are disabled while no curve is a cookie cutter, as in Nuke.
- Deactivating a layer deactivates everything inside it. A deactivated curve's link, boundary and cookie role are ignored, but not deleted.
- `layerAmount` multiplies down nested layers. It also multiplies with `amount` and with the link's own amount.
- `boundaryBBox` with a hard boundary: the format rectangle is a soft boundary, so the hard-boundary masking is unchanged.

**Assumptions:**
- Per-item transforms already apply in sampling (`SplineWarpCurve.cpp:101-122`, `getTransformAtTime`). Only the knobs, layers, links and overlay are missing.
- The geometry curve context creates no Transform page. Only `RotoContextPrivate::createPaintKnobs` does (`RotoContextPrivate.h:1658-1666`).
- The resampler has no Lanczos or Sinc filter (`Resampler.h:88-97`). SplineWarp fills its filter menu from `Resampler::filterOptions()` (`SplineWarp.cpp:267-279`), so it picks up M270's filters automatically.
- The host's `mix` is the mask mix (`StagedImageEffect.cpp:423`).

## Params audit (Nuke 17 → Natron)

| Nuke control (script) | Tab | Nuke default | Natron knob / status | Planned |
|---|---|---|---|---|
| channels | SplineWarp | all | host layer selector (`LayerPolicy::all`) | covered M147 |
| premultiply input (premultiply) | SplineWarp | off | missing → `cookieCutInput` "Cookie Cut Input" | P3.T2 |
| output mask (outputMask) | SplineWarp | mask_splinwarp.a | missing → `outputMask` "Output Mask" | P3.T3 |
| crop to format (crop_to_format) | SplineWarp | on | missing → `cropToFormat` "Crop To Format" | P3.T4 |
| bbox boundary curve (boundary_bbox) | SplineWarp | on | missing → `boundaryBBox` "Boundary BBox" | P2.T4 |
| output | SplineWarp | source warped | M148 `outputWarped` (Source / Source Warped); the B outputs are dropped (user, M147) | covered M148 / dropped |
| mix (morph dissolve) | SplineWarp | 0 | dropped (user, M147: no morph) | dropped |
| root warp (root_warp) | SplineWarp | 1 | `amount` | covered M147 |
| layer warp (layer_warp) | SplineWarp | 1 | missing → `layerAmount` "Layer Amount" | P2.T1 |
| pair warp (pair_warp) | SplineWarp | 1 | link amount in the model and Python only → `linkAmount` "Link Amount" | P2.T2 |
| spline key (key) | SplineWarp | 0, 0 | RotoPanel keyframe controls | covered M146 |
| curves: Name | SplineWarp | — | Label column | covered M146 |
| curves: Pairing | SplineWarp | — | Linked-to column | covered M148 |
| curves: Type (A/B/AB) | SplineWarp | — | Role column; A/B is morph-only, dropped (user, M147) | covered M148 / dropped |
| curves: transform linked | SplineWarp | off | missing → Transform Link column + action | P4.T2 |
| curves: cookie cutter | SplineWarp | off | missing → Cookie Cutter column + action | P3.T1 |
| curves: visible | SplineWarp | on | Activated column, but the warp ignores it | P2.T3 |
| curves: lock | SplineWarp | off | Locked column | covered M146 |
| curves: colour | SplineWarp | — | Overlay column (M148 role colours override it; open question in M148) | covered M146 |
| curves: hard boundary | SplineWarp | off | boundary role | covered M147 |
| curves: boundary (soft) | SplineWarp | off | missing → Soft Boundary role | P2.T3 |
| curves: context menu (add layer, copy, paste, duplicate) | SplineWarp | — | M146 / M148 / M152 | covered |
| translate | Transform | 0, 0 | missing → `translate` | P4.T1 |
| rotate | Transform | 0 | missing → `rotate` | P4.T1 |
| scale | Transform | 1 (w, h) | missing → `scale` (2D) | P4.T1 |
| skew X (skewX) | Transform | 0 | missing → `skewx` (Roto's name) | P4.T1 |
| skew Y (skewY) | Transform | 0 | missing → `skewy` (Roto's name) | P4.T1 |
| skew order (skew_order) | Transform | XY | missing → `skewOrder` | P4.T1 |
| center xy (center) | Transform | from the source input | missing → `center`, default the Source format centre | P4.T1 |
| extra matrix (transform_matrix) | Transform | identity | missing → `extraMatrix` | P4.T1 |
| curve resolution (curverez) | Render | 3 | `curveResolution`, 4 px spacing (different units) | covered M147, sign-off Q2 |
| boundary curve resolution (boundary_curverez) | Render | 3 | missing → `boundaryCurveResolution`, 4 px | P2.T4, sign-off Q2 |
| preview resolution (previewrez) | Render | 100 | missing → `previewResolution` (%) | P5.T1, Q2 |
| Classic warping (rbf) | Render | off (local function) | `solver`: TPS ≈ classic, Mesh ≈ local; default TPS | covered M147/M149, sign-off Q2 |
| filter | Render | cubic | `filter` (10 filters) | covered M147 |
| filter: Lanczos4, Lanczos6, Sinc4 | Render | — | via M270 (Lanczos4, Sinc with Lobes = 4); Lanczos6 has no entry | P5.T2, Q3 |
| input A | — | — | `Source` | covered M147 |
| input B | — | — | dropped (user, M147: no second input) | dropped |
| input mask | — | — | Mask input, `maskInvert`, `mix` | covered M147 |

Natron extras with no Nuke control, kept: `invert`, `smoothness`, `fieldResolution`, `clamp`, `blackOutside`, `bbox`, `motionBlur`, `shutter`, `shutterOffset`, `shutterCustomOffset`.

## Phase 274.1: Audit and page layout

- [ ] M274.P1.T1 — Confirm the params audit against the code
  - files: this table (report corrections to the PM, who edits the plan)
  - approach: re-read `SplineWarp.{h,cpp}`, `SplineWarpInteract.h` (m148) and the merged M146 RotoPanel columns on a geometry host. Correct any row that is wrong. Every row ends up as a task, "covered Mxxx", or "dropped (user …)".
  - verify: the corrections report lists every row as confirmed or changed.
  - size: S
- [ ] M274.P1.T2 — Controls, Transform and Render pages
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`
  - approach:
    - **Controls page:** amount, invert, the Phase 2/3 knobs, and the stage knobs.
    - **Transform page:** filled in Phase 274.4.
    - **Render page:** solver, smoothness, curveResolution, fieldResolution, filter/clamp/blackOutside, bbox, and the motion-blur group.
    - Move the existing knobs onto these pages, with no compatibility shims.
  - verify: `Tests/Native/NativeSplineWarp_Test.cpp` checks each knob's page.
  - size: S

## Phase 274.2: Amounts, activation and boundaries (field only; GPU unchanged)

- [ ] M274.P2.T1 — Per-layer amount (`layerAmount`)
  - files: `SplineWarpContext.{h,cpp}` (per-layer animatable amount, serialization, warp hash), `SplineWarpSampling.cpp` (multiplied into each link's lerp), `SplineWarp.cpp`, `PySplineWarp.{h,cpp}`
  - approach:
    - A node knob tied to the selected layers, default 1, range 0–1.
    - It is enabled only when only layers are selected, and it writes to each selected layer.
    - Nested layers multiply.
  - verify: `Tests/Native/SplineWarpModel_Test.cpp`: a layer at 0.5 halves the destination lerp, nesting multiplies, and keys survive save/load. `SplineWarpPython_Test.cpp`.
  - size: M
- [ ] M274.P2.T2 — Link amount knob (`linkAmount`)
  - files: `SplineWarp.cpp`, `SplineWarpContext.cpp` (`setLinkAmount`/`setLinkAmountAtTime` exist)
  - approach:
    - A node knob tied to the selection, default 1.
    - It is enabled only when every selected curve is linked, and it edits those links' amounts with keys and undo.
    - It reloads when the selection changes.
  - verify: `SplineWarpModel_Test.cpp`: the selection drives the value, and an edit keys the link.
  - size: M
- [ ] M274.P2.T3 — Honour Activated; Soft Boundary role
  - files: `SplineWarpContext.{h,cpp}`, `SplineWarpSampling.cpp`, `SplineWarpMesh.cpp`, `Gui/SplineWarpPanelHook.cpp`, `SplineWarpInteract.cpp` (the Toggle Boundary action splits into Hard and Soft), bindings
  - approach:
    - Inactive curves and layers are skipped when building samples and boundaries.
    - Soft Boundary role: any curve (open or closed, never linked) adds zero-displacement constraints along the curve and does no masking.
    - The Role column shows "Hard Boundary" or "Soft Boundary".
  - verify:
    - `SplineWarpSampling_Test.cpp`: a deactivated link gives identity; on a soft-boundary curve the field is zero while the far side still moves.
    - `SplineWarpMesh_Test.cpp` checks the same.
  - size: L
- [ ] M274.P2.T4 — `boundaryBBox` and `boundaryCurveResolution`
  - files: `SplineWarpSampling.{h,cpp}`, `SplineWarp.cpp`, `SplineWarpCache.h` (key)
  - approach:
    - `boundaryBBox` (default on) adds the Source format rectangle as a soft boundary.
    - `boundaryCurveResolution` (default 4 px, Render page) sets the sample spacing on every boundary, including the format rectangle.
    - M147 tests that assume no pinning set `boundaryBBox` off.
  - verify: `SplineWarpSampling_Test.cpp`: displacement at the format corners is 0 when on and non-zero when off, and the boundary sample count follows the spacing.
  - size: M

## Phase 274.3: Cookie cutters, output mask, crop to format (CPU + GPU)

- [ ] M274.P3.T1 — Cookie Cutter flag
  - files: `SplineWarpContext.{h,cpp}`, `Gui/SplineWarpPanelHook.cpp` (column and action), bindings
  - approach: a per-curve flag, for closed curves only. It is serialized, part of the warp hash, and undoable.
  - verify: `SplineWarpModel_Test.cpp`: an open curve is refused, opening a curve clears the flag, and the flag round-trips through save/load.
  - size: M
- [ ] M274.P3.T2 — Cookie mask and `cookieCutInput`
  - files: `SplineWarp.cpp`, `SplineWarpCookie.{h,cpp}` (new), `Engine/RotoContext.cpp` (reuse `renderBezier`/`renderFeather`), `splinewarp.slang`, `SplineWarpGpu.h`
  - approach:
    - Rasterise the union of the cookie curves, antialiased with feather and fall-off, at their source positions in the Source's RoI. Cache the mask per window by curve hash.
    - `cookieCutInput` (default off) multiplies the processed Source channels by the mask before the resample.
    - On the GPU, a pre-pass multiplies a Source copy.
  - verify:
    - `NativeSplineWarp_Test.cpp`: with zero warp, the output equals Source × mask.
    - `NativeSplineWarpGpu_Test.cpp`: GPU and CPU agree within the existing tolerance.
  - size: L
- [ ] M274.P3.T3 — `outputMask`
  - files: `SplineWarp.{h,cpp}` (`getComponentsNeededAndProduced`, following `Nodes/Channel/AddLayers.h`), `splinewarp.slang`
  - approach:
    - A channel choice, default `SplineWarpMask.A`, disabled while there are no cookie cutters.
    - It writes the cookie mask warped through the same field, filter and motion-blur sub-frames.
  - verify: CPU and GPU tests check that the produced layer exists, matches the warped mask, and is absent when there are no cookie cutters.
  - size: L
- [ ] M274.P3.T4 — `cropToFormat`
  - files: `SplineWarp.cpp` (RoD/RoI, after `LensDistortion.cpp`), `splinewarp.slang` (format rect and flag)
  - approach:
    - Default on.
    - On: sample the Source only inside its format (outside counts as black), and limit the output RoD to the format.
    - Off: warp the full Source RoD.
  - verify:
    - CPU and GPU tests use a synthetic Source with pixels outside its format: on gives black at the edge, off pulls those pixels in.
    - RoD tests cover both settings.
  - size: M

## Phase 274.4: Transform page

- [ ] M274.P4.T1 — Transform knobs on the selection
  - files: `Engine/RotoContextPrivate.h` (move the transform-page block of `createPaintKnobs` into a base helper any host can opt into), `Engine/RotoCurveContext.{h,cpp}` (register for `selectInternal` tying), `SplineWarpContext.cpp`, `SplineWarp.cpp`
  - approach:
    - Add `translate`, `rotate`, `scale` (2D, with `uniform`), `skewx`, `skewy`, `skewOrder` (XY), `center` and `extraMatrix`, tied to the selected curves.
    - `center` defaults to the Source format centre when an item is created.
  - verify: `Tests/Native/SplineWarpTransform_Test.cpp` (new):
    - translating a destination curve moves the field by that amount;
    - a change to the item transform changes the field cache key;
    - undo restores the transform.
  - size: L
- [ ] M274.P4.T2 — Layer transforms and transform links
  - files: `SplineWarpContext.{h,cpp}`, `SplineWarpCurve.cpp` (compose layer→curve), `Gui/SplineWarpPanelHook.cpp` (Transform Link column, "Link Transform To…"/"Unlink Transform"), bindings
  - approach:
    - A layer's transform composes with its children's.
    - A transform link ties the item's transform knobs to the target's.
    - Links that would form a cycle are refused.
  - verify: `SplineWarpTransform_Test.cpp`: a layer rotate moves all its curves, a linked curve follows its source, and a cycle is refused.
  - size: L
- [ ] M274.P4.T3 — Viewer transform overlay
  - files: `SplineWarp.cpp` (`addTransformInteract`, as Transform does), `SplineWarpInteract.cpp`
  - approach:
    - Show the overlay when a single curve or layer is selected and the select tool is active.
    - Dragging it keys the knobs, respecting auto-key.
  - verify: the gate GUI check drags the overlay.
  - size: M

## Phase 274.5: Preview, filters, audit test, GUI, docs

- [ ] M274.P5.T1 — `previewResolution`
  - files: `SplineWarp.cpp`, `SplineWarpField.cpp`
  - approach:
    - A percentage, 1–100, on the Render page.
    - Draft field step = fieldResolution × 100 / previewResolution, and draft curve detail scales the same way.
    - Default per Q2.
  - verify: the M148 draft test is parameterised by this knob.
  - size: M
- [ ] M274.P5.T2 — Lanczos/Sinc filters through M270 (waits on M270)
  - files: `Tests/Native/NativeSplineWarpGpu_Test.cpp` (`EachFilterMatchesTheCpu` covers every `filterOptions()` entry)
  - approach:
    - No SplineWarp code change: the filter menu and kernel are shared.
    - Confirm the new entries appear in the menu.
  - verify: the parity test passes for every filter.
  - size: S
- [ ] M274.P5.T3 — Knob audit test
  - files: `Tests/Native/SplineWarpKnobs_Test.cpp` (new)
  - approach: one row per knob in the audit table, checking script name, label (= readable script name), default and page.
  - verify: the test itself passes.
  - size: S
- [ ] M274.P5.T4 — GUI check
  - files: `Tests/gui/m274_splinewarp_controls.py` (new)
  - approach: at 1920x1080, take screenshots of:
    - the three pages, which must fit;
    - a soft boundary;
    - the transform overlay drag;
    - a cookie mask in `outputMask`;
    - crop on and off.
  - verify: the script passes on the remote GUI runner, and the screenshots are shared.
  - size: M
- [ ] M274.P5.T5 — Docs
  - files: `Engine/Nodes/Transform/docs/SplineWarp.md`
  - approach: document every new knob and role, the page layout, and the differences from Nuke: curveResolution units, the solver default, Lanczos6, and the dropped morph controls.
  - verify: `grep` finds each knob's script name in the doc.
  - size: S

**Verification gate:**
- Each phase builds with `ctest -R SplineWarp` green.
- The final remote `release --tests` is green, including the GPU parity tests and `SplineWarpKnobs_Test`.
- The GUI check passes remotely, and its screenshots are shared before sign-off.
- Fixtures are synthetic, and any golden is a ≤256 px half-float crop.
- An AppImage for UAT.

## Open questions (for the user)

1. **Soft Boundary role.** M147 says unlinked curves are not pins. A soft boundary is an explicit role, so that rule is unchanged. Recommendation: build it; Nuke has it.
2. **Defaults that differ from Nuke.**
   - `solver`: TPS is the default; Nuke defaults to its local function, which is closer to Mesh. Recommendation: keep TPS.
   - Curve and boundary resolution are in pixels, not Nuke's levels. Recommendation: keep 4 px.
   - `previewResolution`: Nuke's default of 100 would make the draft full quality. Recommendation: 25, which matches M148's ×4 draft.
   - `boundaryBBox`: on, as in Nuke. Recommendation: yes.
3. **Lanczos6.** M270 has no Lanczos6 entry. Recommendation: Sinc with Lobes = 6 stands in, or M270 adds Lanczos6.
4. **Names.**
   - `amount`/`layerAmount`/`linkAmount` replace Nuke's root/layer/pair warp. Recommendation: yes.
   - `cookieCutInput` replaces Nuke's `premultiply`, so it isn't mistaken for a premult knob. Recommendation: yes.
   - Roto's `skewx`/`skewy` names are reused.

## Decisions

- 2026-10-28: elaborated (consultant). PM call: morph (Destination input, output modes, morph mix, A/B Type, Duplicate in B) stays dropped under the user's M147 decision (out of scope). The consultant's morph phase was struck, and the rows are marked "dropped (user, M147)". Deps widened to M149–M151, the branch this builds on. The Lanczos/Sinc task waits on M270.
