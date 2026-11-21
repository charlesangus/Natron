# M481 - Viewer Polygon Budgets

Houdini-style budgets for the 3D viewer's Storm draw, requested from M336 - Heavy Scene Acceptance's
viewer research (user, 2026-11-17). The viewer has a scene polygon limit. Over it, the farthest
gprims and instances are replaced by stand-ins first. Two distance controls are added: a cull
distance, and a fraction distance beyond which only a stable N% of each instancer's instances and
of each mesh's faces are drawn. The budget is the steady-state aid. Interaction-time stand-ins stay
with M336. Background: `build/m336-heavy-viewport-research.md` (Houdini's Distance-based Packed
Geometry Culling, Scene Polygon Limit, Bounding Boxes and Point Instancing percentage,
`.local/docs/houdini/ref/windows/displayopts_3d.md:1279-1298, 493-499`).

Code base: the F4 - Heavy Scene Viewer stack as merged (`Engine/Nodes/Scene/SceneDrawFilter.h`,
`SceneDrawStates.h`, `Gui/SceneDrawBoxes.h`, `Gui/SceneViewport.cpp`). The line numbers in the briefs
are from `milestone/m336-heavy-scene-acceptance` as of 2026-11-17. Re-check them at claim time.

## Scope

**In:**
- Knobs on the Viewer node, saved with the project, scriptable and undoable, with initial values for new Viewers in Preferences > Viewers:
  - Polygon Budget (on by default)
  - Polygon Limit (default 20M)
  - Count Whole Scene (default off)
  - Cull Distance (default off)
  - Fraction Distance (default off)
  - Fraction Percent
  - Stand-in (Box Wire, Box Shaded, Marker, Off)
- Over the limit, gprims and individual PointInstancer instances are demoted farthest first until
  the counted polygons fit. Demoted items draw as the chosen stand-in, in a budget tint distinct
  from user BBox rows.
- Beyond Cull Distance, gprims and instances are hidden.
- Beyond Fraction Distance:
  - each instancer draws a hashed, stable Fraction Percent of its instances, and the rest use the
    stand-in;
  - each mesh draws a hashed, stable Fraction Percent of its faces, and the rest are omitted.
- A HUD line shows counted, limit, demoted and culled. Stand-ins pick like BBox rows.

**Out:**
- Decimation or generated LOD meshes: none. Faces are only omitted (user).
- The budget in Cycles, MoonRay or Natron Utility viewer renders. Those follow draw states only
  (M335 - Viewer Render Draw States).
- Interaction-time stand-ins (Off / During interaction / Always) and paint-never-waits: M336 - Heavy
  Scene Acceptance.
- Budget badges in the Scene Graph panel's Draw column: none.
- Counting refined triangles or subdivision levels: none.

**Edge cases:**
- A row the user explicitly set to Full may still be demoted. The budget wins, and the tint shows
  why.
- BBox, Hidden and unloaded rows are never promoted by the budget. It only demotes.
- A camera move never changes the drawn set mid-move. The budget re-evaluates once the camera has
  been idle for about 250 ms. Hysteresis stops items near the boundary flipping on small moves.
- If the camera moves again while an evaluation is still running, that result is thrown away.
- A prim with no authored extent gets no box. Under Box stand-ins it draws as a Marker.
- If a single prim exceeds the limit on its own, it is demoted, and the HUD says so.
- With Count Whole Scene on, the frustum is ignored and the demoted set changes only with distance.
- Instance stand-ins are drawn by instancing the prototype's box with the instancer's own
  transforms, with no box stored per instance. Island has about 31M instances.

**Assumptions:**
- The 20M default suits a modest GPU. It is checked at UAT on Island.
- Storm omits a mesh's faces when they are listed in the Hydra mesh topology's `invisibleFaces`,
  without re-gathering batches. P1.T1 verifies this.
- Demoting by holding prims invisible through `SceneDrawFilter` costs less than 100 ms per camera
  stop at 1M prims. P1.T1 measures this; earlier rounds measured up to about 1 s for visibility
  changes.

## Phase 481.1: Apply-cost probe

- [x] M481.P1.T1 — Measure what a budget change costs Storm
  - files: `tools/bench/heavy_scene_bench.py`, `tools/bench/README.md`
  - approach: add a `budget_apply_probe` scenario. On the 1M city (loaded), the 1M flat file and Island, time the next presented frame (main-thread stall and render-thread cost) after each of these changes:
    - hold invisible 1%, 10% and 50% of the drawn gprims through `SceneDrawFilter`'s visibility path;
    - mask the same shares of instances through the instance mask (`setInstanceHidden`, SceneDrawFilter.cpp ~560);
    - set `invisibleFaces` to half the faces of 1,000 meshes, through a topology override in the filter's data source (prototype in the bench branch only).
    Run on the GPU host or a cloud VM (cloud-gpu skill), never the dev host. Record the JSON under `## Decisions`. If no path fits the 100 ms stall budget, the PM puts the numbers to the user as `response` before P2.
  - verify: the scenario runs under Xvfb on the city fixture locally (numbers not judged), and its GPU-host or VM JSON is recorded here.
  - size: M

## Phase 481.2: Counts and solver

- [x] M481.P2.T1 — Polygon counts on the box thread
  - files: `Gui/SceneDrawBoxes.h`, `Gui/SceneDrawBoxes.cpp`, `Tests/SceneDrawBoxes_Test.cpp`
  - approach: alongside each `Box`, the box thread publishes a face count:
    - meshes: the length of `faceVertexCounts`;
    - basis curves: segments;
    - points: the point count.
    For a PointInstancer it publishes the prototype's face count and box, plus a reference to the instancer's instance transforms. It does not publish per-instance boxes. Counts are read on the box thread from the stage, never on the main thread, and unloaded payloads count 0. Add a camera input (eye position plus view-projection) next to `setTime`, so later passes can sort by distance.
  - verify: new `SceneDrawBoxes_Test` cases give the face counts of a cube mesh, a curves prim and an instancer with 3 instances of a 6-face prototype, and confirm an unloaded payload counts 0.
  - size: M

- [x] M481.P2.T2 — Budget solver
  - files: `Engine/Nodes/Scene/SceneViewBudget.h`, `Engine/Nodes/Scene/SceneViewBudget.cpp`, `Tests/Scene/SceneViewBudget_Test.cpp`, the CMake source lists for Engine and Tests
  - approach: a pure function.
    - Inputs: units (a gprim path, or an instancer plus instance index), each with its box and face count; the camera (eye position and frustum); the settings (limit, count whole scene, cull distance, fraction distance, fraction percent); and the previous result, used for hysteresis.
    - Output: the demoted units, the culled units, a kept-instance mask per instancer and a kept-face fraction per mesh.
    - Rules: count only units whose box meets the frustum, unless Count Whole Scene is on. Cull everything beyond Cull Distance. Apply fractions beyond Fraction Distance, choosing kept instances and faces by a hash of instance id or face index, so the kept set never changes with the camera. Then demote the farthest units until the count fits the limit. A hysteresis band of 5% of the limit and 5% of distance keeps the previous decision near the boundary.
    - Only units the draw states resolve to Full or Children are eligible. BBox, Hidden and unloaded units never change.
  - verify: `SceneViewBudget_Test` covers:
    - farthest-first demotion;
    - frustum counting, and Count Whole Scene;
    - cull distance;
    - a fraction subset that stays the same across camera positions;
    - hysteresis that keeps the result unchanged on a 1% camera move;
    - a single unit over the limit;
    - BBox and Hidden units left untouched.
  - size: L

## Phase 481.3: Viewer

- [ ] M481.P3.T1 — Budget layer in the draw filter
  - files: `Engine/Nodes/Scene/SceneDrawFilter.h`, `Engine/Nodes/Scene/SceneDrawFilter.cpp`, `Tests/SceneDrawFilter_Test.cpp`
  - approach: add a budget layer, kept apart from the rule states, with a call such as `applyBudget(result)`. It holds demoted and culled gprims invisible on the cheapest path P1.T1 measured. It ORs into the instance mask for demoted, culled and fractioned-out instances. It overrides each fractioned mesh's topology `invisibleFaces` with the hashed faces. Clearing the budget restores exactly what the draw states draw. The layer binds only on the Storm engine's filter; delegate engines (M335 - Viewer Render Draw States) never get it.
  - verify: new `SceneDrawFilter_Test` cases:
    - a demoted Full prim reads invisible;
    - an instance masked by the budget and also by a rule reads hidden once, and is restored by clearing only the budget;
    - a fractioned mesh's `invisibleFaces` holds the expected hashed set;
    - a delegate-engine filter is unaffected.
  - size: L

- [ ] M481.P3.T2 — Viewer node knobs and Preferences defaults
  - files: `Engine/ViewerInstance.cpp`, `Engine/ViewerInstance.h`, `Engine/Settings.cpp`, `Engine/Settings.h`, `Gui/SceneViewport.cpp` (right-click viewer menu, ~9818-9949), `Gui/SceneViewport.h`, `Tests/ViewerInstance_Test.cpp` (or the existing Viewer knob test)
  - approach: add the knobs to the Viewer node in `ViewerInstance::initializeKnobs` (beside the M164 - Viewer Node Knobs knobs), on a "Polygon Budget" group, so they save in `project.yaml`, script, link and undo like other Viewer knobs. Matching Preferences > Viewers knobs next to `viewer3DGuideSize` give a newly created Viewer node its initial values; loading a project uses the saved values. Knobs:
    - `viewer3DPolygonBudget` (bool, on)
    - `viewer3DPolygonLimit` (int, 20,000,000)
    - `viewer3DCountWholeScene` (bool, off)
    - `viewer3DCullDistance` (double, 0 = off)
    - `viewer3DFractionDistance` (double, 0 = off)
    - `viewer3DFractionPercent` (double, 25)
    - `viewer3DStandIn` (choice: Box Wire, Box Shaded, Marker, Off)

    Viewer knob script names drop the `viewer3D` prefix (`polygonBudget`, `polygonLimit`, …). Labels are readable script names. Tooltips say what each does and never mention other applications. A "Polygon Budget" submenu in the 3D viewer's right-click menu is a view of the Viewer node's knobs (edits go through the knobs, so they undo). The viewport reads the knobs, never a copy.
  - verify: a test creates a Viewer node and reads the Preferences values; sets a knob, saves, reloads and reads it back; and checks that a second Viewer is unaffected. A menu edit changes the knob and undoes.
  - size: M

- [ ] M481.P3.T3 — Camera-idle evaluation, stand-ins and HUD
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneViewport.h`, `Gui/SceneDrawBoxes.cpp`
  - approach:
    - On camera change, cancel any evaluation in flight. After 250 ms idle, hand the camera to the box thread, run `SceneViewBudget` there against the published boxes and counts, and post the result to the filter through the existing draw-delta queue (`postDrawDelta`, ~3169). The main thread does no counting or solving.
    - Demoted items draw through `drawBoxes` (~2728) in a budget tint, as Box Wire, Box Shaded or Marker per the Stand-in setting, and nothing when it is Off. Instance stand-ins are one instanced draw per instancer: the prototype box times the instancer's transforms, restricted to the demoted indices.
    - Stand-ins pick through `SceneDrawBoxes::pick` like BBox rows, and a picked instance selects its instance path.
    - Add a HUD line next to the existing ones (~9106-9126): "Budget: <counted> / <limit> polys, <n> demoted, <m> culled", plus "(<prim> alone exceeds the limit)" when that case applies. The line is hidden when the budget is off.
  - verify: covered by P3.T4's GUI script; `SceneViewport_Test` builds.
  - size: L

- [ ] M481.P3.T4 — GUI test for every control
  - files: `Tests/gui/scene_viewer_budget.py`
  - approach: on a generated scene (a grid of meshes and one instancer, built in the script), check each of these:
    - the limit demotes the farthest first;
    - Count Whole Scene;
    - cull distance;
    - the fraction subset staying the same across two camera positions;
    - each Stand-in value;
    - HUD text;
    - picking a stand-in;
    - an explicit Full row being demoted;
    - no change while the camera is moving, then a change after idle;
    - turning the budget off restoring everything.
    Read state through the viewer's draw stats and the filter's counts, not pixels, except one screenshot per Stand-in value.
  - verify: the script passes under the Xvfb GUI runner.
  - size: M

## Phase 481.4: Bench and UAT

- [ ] M481.P4.T1 — Budget bench scenario
  - files: `tools/bench/heavy_scene_bench.py`, `tools/bench/README.md`
  - approach: add a `budget_orbit` scenario to the long scenario set. On the loaded 1M city and on Island (textures staged) it uses the default settings. It orbits, stops, waits for the budget to apply, and repeats 10 times. For each stop it records the main-thread stall and worst frame, and over the whole run the orbit frame rate. The budgets are a stall of at most 100 ms per stop and an orbit of at least 30 fps.
  - verify: the scenario runs locally under Xvfb on the city (numbers not judged). The GPU-host or VM run passes, and its JSON is recorded under `## Decisions`.
  - size: M

- [ ] M481.P4.T2 — UAT AppImage and steps
  - files: `build/appimages/` (AppImage plus same-named steps `.md`)
  - approach: a stripped release AppImage. The steps walk Island with the default budget: orbit, then stop and check the HUD and the stand-ins. Then they change each control, compare the Stand-in looks, pick a stand-in, and turn the budget off. Ask whether 20M suits the user's GPU.
  - verify: the user's report is recorded here, with any fixes filed as tasks.
  - size: M

**Verification gate:** ctest (SceneViewBudget, SceneDrawFilter, SceneDrawBoxes) and `Tests/gui/scene_viewer_budget.py` plus the existing `scene_draw_*`/`scene_viewer_*` GUI scripts pass. `budget_orbit` passes on the GPU host or a cloud VM. The user accepts the behaviour on Island.

## Decisions

- 2026-11-17 — Re-evaluate on camera idle with hysteresis, never per frame (user): changing what Storm draws costs up to about 1 s at 1M prims (M336 rounds 3-5). M336's interaction stand-in covers the tumble.
- 2026-11-17 — The budget may demote an explicit Full row (user): the limit is a hard viewport ceiling, as in Houdini, and the budget tint shows why.
- 2026-11-17 — The unit is the gprim, with each PointInstancer instance counted separately (user): finest control, matching Houdini's packed-primitive behaviour. The existing instance mask carries it.
- 2026-11-17 — Instance stand-ins are a setting (user). One Stand-in setting covers gprims and instances: Box Wire, Box Shaded, Marker, Off. Default Box Wire, in a budget tint (user).
- 2026-11-17 — Settings are knobs on the Viewer node and travel with the project (user, revised the same day): scriptable, linkable and undoable like the M164 - Viewer Node Knobs knobs. Preferences > Viewers give a new Viewer's initial values.
- 2026-11-17 — On by default with a generous limit (user): 20M polygons, with distance cull and fraction off.
- 2026-11-17 — Count authored faces in the frustum, times instances, with a Count Whole Scene option (user).
- 2026-11-17 — Fractions apply beyond a distance (user): a hashed, stable subset of instances, and of each mesh's faces. Faces are omitted, never decimated into a new low-polygon mesh.
- 2026-11-17 — Storm only (user): delegate viewer renders follow draw states and ignore the budget.
- 2026-11-17 — Standalone, after F4 - Heavy Scene Viewer lands (user). Deps M336 - Heavy Scene Acceptance.
- 2026-11-17 — A HUD budget line, and stand-ins that can be picked (user). No Scene Graph badges.
- 2026-11-17 — Gate (user): a `budget_orbit` bench on the 1M city and Island, with a stall of at most 100 ms per stop and an orbit of at least 30 fps, then UAT on Island.
- 2026-11-18 — **Possible follow-up (user, from M336 - Heavy Scene Acceptance):** after a scene has fully filled, while the camera moves on a scene whose full draw is over budget, draw only the stand-in boxes (no Storm pass), then one full draw once idle — the During interaction stand-in rule applied to every move.
- 2026-11-20 — P1.T1 in 598827bc9: the env-gated (`NATRON_BUDGET_PROBE=1`) SceneBudgetProbe scene index and the budget_apply_probe bench. Faces are hidden through a child geomSubset (visibility false) plus a topology dirty, because USD 0.26.8's HdMeshTopologySchema has no invisibleFaces; P3.T1's wording follows this. The probe sits just after SceneDrawFilter and sends the same Hydra changes, so the filter's own bookkeeping isn't measured. **PM call:** the probe stays on the milestone branch behind the env gate and is removed when P3 lands. Build running; the GPU-host or VM run follows.
- 2026-11-20 — **Based on F4's current branch (user):** "Pick up m481 based on the current state of f4, accept some rebasing." The phase-1 commit is being rebased from M336's branch onto feature/f4-heavy-scene-viewer (bd9fae37b), and the remaining phases build on F4. M481 rebases again once M336 merges into F4. F4's branch lacks main's AppStream fix, so packaging may fail there until F4 ships; ctest still runs.
- 2026-11-20 — Phase 481.1 rebased onto F4 (f6e7113f6). It adapts to F4's draw-change delivery and settled check, and records its own engine id.
- 2026-11-20 — Phase 481.2 committed: unit counts on the box worker (off by default, `setCounting`; a PointInstancer is one unit; built-in shape face counts follow USD's tessellation) and SceneViewBudget (cull → fraction by hashed path and index → count → demote farthest first; >limit units always go and are named). Unit types live in Engine. **PM calls:**
  - Keep the 5% hysteresis band. The drawn count may sit up to 5% over until a fresh solve, which avoids boundary flicker.
  - Phase 3 must split a huge instancer's world-bound read into chunks so writers aren't stalled (UI responsiveness rule).
  - Phase 3 must extend the draw filter's per-instance mask to PointInstancer instances; today it covers native instances only.

  Noted: the solver keeps about 5 B per instance (about 155 MB at 31M), and the fraction hash uses the instance index, so reordering instances changes the subset. Gate running.
