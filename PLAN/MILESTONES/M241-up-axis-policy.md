# M241 - Up Axis And Units Policy

Split out of M226 - Scene Hierarchy From Topology's UAT (user, 2026-10-27: "we'll consider the up axis separately"), and widened to units. M226 shipped the project knobs `upAxis` (Y/Z, default Y) and `metersPerUnit` (default 1.0). It authors both on every layer Natron creates, edit layers included. USDRead carries the file's own metrics up into its layer and warns when they differ from the project's. Nothing is converted.

This milestone settles the policy on the Solaris model. Solaris treats `upAxis`/`metersPerUnit` as metadata and never converts silently. Only the stage's root layer counts (Sublayer/Load Layer can copy a file's metadata to the root), Asset Reference has an opt-in **Align To Up Axis** corrective transform, and USD ROP writes the metrics on every layer it saves (`.local/docs/solaris/docs/houdini/nodes/lop/{configurelayer,sublayer,assetreference,componentoutput}.html`). USD agrees: `UsdGeomGetStageUpAxis`/`UsdGeomGetStageMetersPerUnit` are advisory, and the unauthored fallbacks are Y and 0.01 (`.local/docs/usd/api/group___usd_geom_linear_units__group.html`). In Natron the project plays the root layer's part: the project's metrics win, and a file is converted only when the user asks USDRead to.

USDRead gains two toggles, **Match Project Up Axis** and **Match Project Units** (both off by default), and a **Use File's Metrics For Project** button. Code lives in `Engine/Nodes/Scene/` on M226's branch (`USDRead.cpp`, `SceneOps.cpp`, `SceneNodeAuthoring.cpp`) and in `Engine/Project.cpp`.

## Scope

**In:**
- The policy itself: the project's Up Axis and Meters Per Unit win on every Natron-authored layer (M226's "Units on edit layers" decision stands as written). A file's own metrics count only inside its USDRead's layer, and only while that layer is uncorrected.
- USDRead `matchUpAxis` and `matchUnits` toggles, both off by default. They correct independently. Each authors a corrective transform over the file's topmost xformable prims, in USDRead's own layer, and the file is never edited.
- Once a toggle corrects an aspect, USDRead's layer authors the project's value for it instead of the file's, so the scene reports the project's metrics from that node on.
- Camera properties the corrective scale does not reach (`clippingRange`, `focusDistance`) are rescaled on corrected cameras, time samples included.
- USDRead `useFileMetrics` button: sets the project's Up Axis and Meters Per Unit to the file's, as one undoable edit.
- The warning names only the aspects still uncorrected and points at the toggle for each. A file that authors no metrics is warned about with USD's fallbacks named as such.
- USDWrite keeps writing the metrics on every layer it saves. Its round trip is verified with corrected reads.
- Meters Per Unit's default stays 1.0 (user, 2026-10-27), which settles M226's unconfirmed assumption.

**Out:**
- A standalone axis/units conversion node: none. USDRead's toggles cover file data, and Natron-authored data is already in project metrics.
- Scaling non-geometric quantities that carry length implicitly (light intensities, `UsdPhysics` masses and `kilogramsPerUnit`, shader parameters in scene units): none. Solaris doesn't either, since nothing can tell which attributes are lengths.
- Viewer grid spacing, navigation speed and clipping defaults that follow the project's units belong to M229 - 3D Viewer Guides And Display.
- A live link from the project to a file's metrics: none. The button is one-shot.

**Edge cases:**
- Y-up file in a Z-up project: rotate +90° about X. Z-up file in a Y-up project: rotate −90° about X. With matching axes no rotation op is authored.
- Units correction scales uniformly by `fileMetersPerUnit / projectMetersPerUnit`. No scale op is authored when the factor is 1 within `UsdGeomLinearUnitsAre`'s epsilon.
- A root prim that isn't xformable (a `Scope`, or a typeless `def`): the correction goes on its topmost xformable descendants instead, so every transformed prim gets exactly one corrective op pair.
- A root prim whose `xformOpOrder` is authored in a sublayer or payload of the file: the correction reads the composed order and prepends to it. A root with `!resetXformStack!` keeps it first.
- A file with no authored metrics: treated as USD's fallbacks (Y, 0.01), the same as M226's warning does today. The warning says the values are fallbacks, and the user can leave Match Project Units off when the file is really in metres.
- Reload or a file change re-derives the correction, because the file's metrics and root prims are part of USDRead's layer key.
- Changing the project's metrics with a toggle on re-authors the correction (project metrics are already in every scene node's layer key, from M226).
- A downstream USDEditTransform pattern still matches the file's prim paths, because the correction never reparents anything.

**Assumptions:**
- Hydra and Storm render a camera whose world transform carries a uniform scale correctly once its clipping range is rescaled. M241.P1.T1 proves this before anything builds on it.
- USDWrite's layered mode keeps a corrected USDRead's `over`s in the sidecar it writes for that layer, with the file still referenced by a relative path.

## Phase 241.1: Correction substrate

- [ ] M241.P1.T1 — Probe the corrective transform on file roots
  - files: `build/m241-probe/` (throwaway), `Engine/Nodes/Scene/SceneOps.cpp` read only
  - approach: Using test files that cover a Z-up cm file (Xform root, Scope root holding meshes, a camera with clippingRange [1, 100000] and a focusDistance, a root whose xformOpOrder is authored in a sublayer), author the planned correction in a stronger layer: `over`s on the topmost xformable prims, prepending `xformOp:rotateX:upAxisCorrection` and `xformOp:scale:unitsCorrection` to the composed order. Compose the stage, then check world bounds, the camera's world transform, and the Storm render of the camera view against the same scene converted by hand. Also measure the cost of the composed-order read (a `SceneQueryPool` lease versus a `LoadNone` stage on the file layer) on a large file from `build/assets`. Record the chosen read path and any Hydra camera-scale problem in `## Decisions`.
  - verify: probe script output shows matching bounds and a pixel-matching camera render (within ulps) for both directions and both unit cases; the decision is recorded.
  - size: L

- [ ] M241.P1.T2 — `SceneOps::authorMetricsCorrection`
  - files: `Engine/Nodes/Scene/SceneOps.h`, `Engine/Nodes/Scene/SceneOps.cpp`, `Tests/Scene/SceneMetricsCorrection_Test.cpp`, `Tests/CMakeLists.txt` (or the Scene test list it uses)
  - approach: A function taking the target layer, the composed source (the read path chosen in P1.T1), the file's and the project's up axis and meters per unit, and two booleans (correct axis, correct units). It finds the topmost xformable prims under each root, authors `over`s prepending the rotate and/or scale ops (the Edge cases above give the rules), and rescales `clippingRange`/`focusDistance` on every `UsdGeomCamera` under a scaled prim, default and time samples alike. Corrects nothing when both booleans are off or the metrics already match.
  - verify: gtest — Y→Z and Z→Y rotation give the expected world transform on an Xform root and on a mesh under a Scope root; cm→m scales the bounds by 0.01; a sublayer-authored xformOpOrder is preserved after the prepended ops; `!resetXformStack!` stays first; a camera's clippingRange and focusDistance samples are rescaled; matching metrics author nothing.
  - size: L

## Phase 241.2: USDRead controls

- [ ] M241.P2.T1 — Match Project Up Axis and Match Project Units toggles
  - files: `Engine/Nodes/Scene/USDRead.cpp`, `Engine/Nodes/Scene/USDRead.h`, `Engine/Nodes/Scene/docs/USDRead.md`, `Tests/Scene/SceneUpAxis_Test.cpp`
  - approach: Two `KnobBool`s, `matchUpAxis` ("Match Project Up Axis") and `matchUnits` ("Match Project Units"), both default off and not animatable, after Frame Offset. Both go into the layer key. `authorLayer` calls `authorMetricsCorrection` after the sublayer is inserted. For each corrected aspect it writes the project's value into the layer's root metadata instead of copying the file's, so the composed metadata matches the project. The warning in `renderScene` lists only the aspects still differing, names the toggle that would fix each, and says "(unauthored, USD fallback)" for a metric the file doesn't author. Document both toggles in `USDRead.md`.
  - verify: gtest — a Z-up cm file in a Y-up m project: both off warns about both and authors no ops; Match Project Up Axis alone warns about units only and composes upAxis Y; both on clears the warning, composes the project's metrics and scales bounds; toggling off restores the uncorrected layer; switching the project's axis with a toggle on re-authors the correction; a file with no metrics warns with the fallback wording.
  - size: M

- [ ] M241.P2.T2 — Use File's Metrics For Project button
  - files: `Engine/Nodes/Scene/USDRead.cpp`, `Engine/Nodes/Scene/USDRead.h`, `Engine/Project.h`, `Engine/Project.cpp`, `Engine/Nodes/Scene/docs/USDRead.md`
  - approach: A `KnobButton` `useFileMetrics` ("Use File's Metrics For Project") beside the toggles. On press it sets the project's `upAxis` and `metersPerUnit` knobs to the file's values (USD fallbacks where unauthored) through the knobs' user-edit path, so it is one undo step. Find how other buttons make undoable knob edits (grep `KnobButton` handlers that set another node's knobs) and follow that. It is disabled with no file loaded. Add a small `Project` setter only if no existing path lets a node set project knobs undoably.
  - verify: gtest — pressing it on a Z-up 0.01 file sets the project to Z / 0.01, the warning clears, and undo restores Y / 1.0.
  - size: M

## Phase 241.3: Export

- [ ] M241.P3.T1 — USDWrite round trip of corrected reads
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: Verify, and fix if needed, that both modes export a corrected USDRead correctly. Flattened mode bakes the corrective ops into the prims, and the output root carries the project's metrics. Layered mode writes USDRead's sidecar with the `over`s and the project's metrics, and keeps referencing the file by relative path. An uncorrected read's sidecar keeps the file's metrics, as today.
  - verify: round-trip gtest — Z-up cm file read with both toggles on → USDWrite (both modes) → USDRead composes the same world bounds and metadata as the source graph (`ExportToString` diff of the composed stage); in layered mode the file is still referenced by a relative path.
  - size: M

## Phase 241.4: GUI check

- [ ] M241.P4.T1 — Viewer check of corrected reads under Xvfb
  - files: `Tests/gui/m241_up_axis_policy.py`
  - approach: Follow `Tests/gui/m226_scene_hierarchy.py`'s setup and the Xvfb runner at 1920x1080. Use a Y-up project, read a Z-up cm fixture (generated under `build/`), and screenshot the Viewer's 3D view with the toggles off and with both on. Assert the warning text in each state, check that the corrected model stands upright against the grid at metre scale (bounds via the scene API), and check that Use File's Metrics For Project flips the grid to Z.
  - verify: the script passes under the Xvfb runner; screenshots are saved for the user.
  - size: M

**Verification gate:** release gate green; `SceneMetricsCorrection_Test`, `SceneUpAxis_Test` and `USDWrite_Test` pass; `m241_up_axis_policy.py`, `scene_viewer_flip.py` and `m226_scene_hierarchy.py` pass under Xvfb; screenshots of a Z-up cm file read into a Y-up m project, uncorrected and corrected, shared with the user before sign-off.

## Decisions

- 2026-10-28: GUI checks pass on Actions (M269 remote runs); AppImage and UAT doc in build/appimages/, contact sheet build/uat-shots/; awaiting user UAT.
- 2026-10-27 — **Solaris model (user):** the project's metrics win on every Natron-authored layer, and a file is converted only by USDRead's opt-in toggles, off by default, one for axis and one for units. A one-shot button adopts the file's metrics for the project. M226's "Units on edit layers" decision stands unchanged. Project-wide record: `PLAN/DECISIONS/2026-10-27-up-axis-and-units-policy.md`.
- 2026-10-27 — **Meters Per Unit default stays 1.0 (user):** the Nuke/Houdini metre convention rather than USD's 0.01 fallback. It settles M226's unconfirmed assumption.
- 2026-10-27 — **P1.T1 probe: the correction is sound, read through a SceneQueryPool lease (implementer):** `build/m241-probe/probe.py` (run under Xvfb with `render.sh`) authored the correction in a stronger layer over Z-up cm and Y-up m files (Xform root, Scope root, typeless root, a root whose `xformOpOrder` lives in a sublayer, `!resetXformStack!` roots and a resetting child, a camera with clipping range and focus distance samples) for axis, units and both, in both directions, and compared with the same scenes converted by hand. World bounds match to 4e-8 relative; the camera frusta match exactly (view matrices within 5e-13, projections and near/far identical, all sample times). `GfCamera::GetFrustum()` orthonormalizes the camera transform, so a camera under the corrective scale sees world units: rescaling `clippingRange`/`focusDistance` is right, and Natron's viewer and USDRender both go through `GetFrustum()`. Storm renders through the camera match except for ≤27 silhouette-edge pixels per 256 px image differing by ≤6/255 (vertex rounding of baked versus transformed points); interiors are within 1/255. **Read path:** a `LoadNone` stage is wrong: it does not compose payloads, so a root whose `xformOpOrder` is authored in a payload reads no order (checked). The composed order is read from a `SceneQueryPool` lease on a one-layer payload that sublayers the file (with the file's time codes per second, so sample times are not rate-converted), kept per file layer so toggle flips reuse the pooled stage. On 2,000 roots × 50 meshes (Python) a fresh LoadAll stage costs ~2 s to open, the read ~4 s (mostly the camera traversal), the leased stage only the read. Authoring runs only on a layer-key change. **Two additions to the rules:** a descendant with `!resetXformStack!` escapes its ancestor's correction, so it gets its own op pair; and correction op names take a free `_<n>` suffix when the prim already has one (a file exported from a corrected read).
- 2026-10-27 — **Implementation notes (implementer):** (1) Match Project Units also rescales `focalLength`, `horizontalAperture`/`verticalAperture` and their offsets, and the plane distances in `clippingPlanes`, besides `clippingRange`/`focusDistance`: Natron authors the lens in tenths of a scene unit, so without them a corrected camera's field of view would change by the units factor. (2) The correction targets every xformable with no xformable ancestor, plus any descendant whose own stack resets; every camera is xformable, so each sits under (or is) a target and is rescaled. (3) Use File's Metrics For Project needs no `Project` setter: the button opens an `UndoStepGuard` (joining the one `KnobGui` opens for a button press) and records the project's `upAxis`/`metersPerUnit` knobs with `recordPythonKnobEdit` after checking `IsolatedJob::refusesWrite`, so it undoes as one step through the window's history, the same path Python edits of project knobs take. (4) USDWrite needed no change: layered mode flattens USDRead's layer, corrective overs included, into the written file over the file it still sublayers, and both modes author the scene's (the project's, once corrected) metrics; there is no separate sidecar for USDRead's layer, since sidecars are only written for referenced anonymous stacks.
