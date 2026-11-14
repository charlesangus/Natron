# M423 - USDPrune Isolate Mode

USDPrune gains a Prune Unselected checkbox. With it on, the node keeps what Prim Pattern selects and prunes the rest of the scene, by Deactivate or Hide as Mode says. The matches, their ancestors and their descendants survive. So do lights, cameras, materials and render settings, each behind its own Keep checkbox, all on by default. The node authors on the fewest prims: the highest prims that lie off every kept path. In a viewer evaluation an unloaded branch is pruned only when the pattern cannot reach it (`PrimSelector::getReach()`). Renders load everything, so they are exact. Built on the F4 - Heavy Scene Viewer branch, where M331 - Deferred Pattern Matching and M381 - Deferred Matching Coverage rewrote USDPrune's matching.

## Scope

**In:**
- Knobs `pruneUnselected` ("Prune Unselected", default off), `keepLights` ("Keep Lights"), `keepCameras` ("Keep Cameras"), `keepMaterials` ("Keep Materials") and `keepRenderSettings` ("Keep Render Settings"), the four Keep knobs default on and enabled only while Prune Unselected is on.
- Isolate in both Modes, the reach-based handling of unloaded branches, the node doc and tooltips, ctest coverage and one Xvfb GUI script.

**Out:**
- Exclusion rules, prune restrictions and pruning point instances one by one (Solaris Prune extras): no milestone.
- Animating the prune or un-pruning (authoring active = true or visible).
- Changes to the pattern language or `matchPattern()` itself: M331 - Deferred Pattern Matching and M381 - Deferred Matching Coverage.
- Pattern-knob GUI tools: M334 - Knob Prim Tools.

**Edge cases:**
- A match inside an instance proxy: its instance prim is kept whole, since USD can't author inside an instance proxy. Off-path instance prims are pruned as one prim.
- Hide mode and a non-imageable prune root (a Material or a typeless prim): the node descends into it and hides the highest imageable prims below it, since hiding a non-imageable prim has no effect.
- Hide mode keeps every ancestor of a kept prim visible by authoring nothing on it. An ancestor the input already made invisible stays invisible, as in today's Hide mode.
- A pattern that matches nothing with Prune Unselected on: the scene passes through unchanged with the existing no-match warning, rather than pruning everything.
- An empty pattern with Prune Unselected on: passes through without a warning, as today.
- Keep Lights means any `UsdLuxBoundableLightBase` or `UsdLuxNonboundableLightBase` prim, and light filters. Keep Cameras means `UsdGeomCamera`. Keep Materials means `UsdShadeMaterial` and anything below one (shaders, node graphs). Keep Render Settings means `UsdRenderSettings`, `UsdRenderProduct`, `UsdRenderVar` and `UsdRenderPass`. Each kept prim keeps its ancestors.
- Unloaded branches in a viewer evaluation: the Keep types are checked only on loaded prims. An unloaded payload is kept when any prefix of the pattern's reach lies on or below it, or above it, and pruned otherwise. A light or camera inside an unloaded payload the pattern can't reach is pruned in the viewer and kept in a render.
- A load in the viewer changes what isolate prunes, so with Prune Unselected on the node's reach is "/" and every load re-evaluates it.

**Assumptions:**
- `matchPattern()`, `PrimSelector::getReach()` and the evaluation context's reach behave on the F4 branch as M381 - Deferred Matching Coverage's `## Decisions` describe.
- A light filter is a `UsdLuxLightFilter` in the pxr version the image ships (26.08).

## Phase 423.1: Engine

- [ ] M423.P1.T1 — Prune Unselected in both Modes
  - files: `Engine/Nodes/Scene/USDPrune.h`, `Engine/Nodes/Scene/USDPrune.cpp`, `Tests/Scene/USDMergePrune_Test.cpp`
  - approach: add `kUSDPruneParamPruneUnselected` "pruneUnselected", a KnobBool labelled "Prune Unselected", default off, not animated, after Mode. When it is on and the pattern matches, the kept set is the matches. Walk the matched stage (the lease `matchPattern()` borrowed, or borrow one with `args.context`) from the pseudo-root. A prim in or below the kept set is left alone, a prim above a kept prim is descended into, and any other prim is a prune root. Treat an instance prim as a leaf, so a match below it keeps the instance. In Hide mode, replace a non-imageable prune root by the highest imageable prims below it. Author on the prune roots with the existing Deactivate or Hide authoring. The memo facts take the prune roots and the checkbox in place of the matches. Check the render's abort flag during the walk. No match and an empty pattern keep their current pass-through behaviour.
  - verify: new cases in `USDMergePrune_Test.cpp` on a `/World/{A/{a1,a2},B,C}` scene. Isolating `/World/A/a1` deactivates exactly `/World/A/a2`, `/World/B` and `/World/C` and leaves `/World` and `/World/A` active. The same in Hide makes those three invisible and authors nothing on `/World` or `/World/A`. A match inside an instance keeps the instance prim. A pattern that matches nothing passes the scene through with the warning. Turning the checkbox off restores today's results.
  - size: M

- [ ] M423.P1.T2 — Keep Lights, Cameras, Materials and Render Settings
  - files: `Engine/Nodes/Scene/USDPrune.h`, `Engine/Nodes/Scene/USDPrune.cpp`, `Tests/Scene/USDMergePrune_Test.cpp`
  - approach: add KnobBools `keepLights` "Keep Lights", `keepCameras` "Keep Cameras", `keepMaterials` "Keep Materials" and `keepRenderSettings` "Keep Render Settings", default on, not animated, enabled only while Prune Unselected is on (follow how another native node enables knobs from a checkbox in `knobChanged`). During T1's walk, a loaded prim of a checked type joins the kept set, with the types the Scope edge cases list. A Material keeps its whole subtree. The checked set goes into the memo facts.
  - verify: on a scene with `/World/Geo/{a,b}`, `/World/Lights/key`, `/World/Cam`, `/World/Looks/mat` (bound to `a`) and `/Render/settings`, isolating `/World/Geo/a` with defaults keeps the light, the camera, the material and the render settings, and `a` still resolves its binding. Clearing each Keep in turn prunes exactly that branch. The four knobs are disabled while Prune Unselected is off.
  - size: M

- [ ] M423.P1.T3 — Isolate on a deferred load set
  - files: `Engine/Nodes/Scene/USDPrune.cpp`, `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: in an evaluation whose context leaves payloads unloaded, T1's walk reaches unloaded payload prims without their children. Keep such a prim when a prefix from `PrimSelector::getReach()` is the prim, an ancestor of it, or below it (a reach of "/" keeps all of them), and treat it as a prune root otherwise. The Keep types are not looked for inside it. With Prune Unselected on, set the context's reach to "/" so any load re-evaluates the node, the way M381 - Deferred Matching Coverage's borrowers add theirs.
  - verify: on the CarCity probe, isolating `/World/Block_0//car*` with nothing loaded opens 0 buildings, keeps `Block_0`'s unloaded payloads and prunes every other block's. Loading `Block_0` re-evaluates the node, and the result matches the fully loaded composition (`expectParity`). A pattern `//car*` keeps every unloaded payload. A render (everything loaded) gives the same prims as the parity reference.
  - size: M

- [ ] M423.P1.T4 — Node doc and tooltips
  - files: `Engine/Nodes/Scene/docs/USDPrune.md`, `Engine/Nodes/Scene/USDPrune.h` (`kNodeSpec.description`), `Engine/Nodes/Scene/USDPrune.cpp` (tooltips)
  - approach: document Prune Unselected and the four Keep knobs: what survives, the highest-prim authoring, Hide's handling of ancestors and non-imageable prims, instances, and that the viewer prunes an unloaded branch only where the pattern cannot reach while renders are exact. Each knob's tooltip says the same in one or two sentences. Update the node description to mention isolating. No mention of other applications.
  - verify: the doc and tooltips match the behaviour T1–T3 tested; each label is the readable script name.
  - size: M

## Phase 423.2: GUI check

- [ ] M423.P2.T1 — Xvfb script for toggling isolate in the viewer
  - files: new `Tests/gui/scene_prune_isolate.py`
  - approach: model it on an existing `Tests/gui/scene_*.py` script. Build a scene with three prims into USDPrune viewed in a 3D viewer, set the pattern to one prim, toggle Prune Unselected on and off, then switch Mode to Hide. Read what the viewer draws after each knob change with no manual refresh call, and take a screenshot of the properties panel with Prune Unselected on.
  - verify: passes under Xvfb (`build/deeprepro/run-gui.sh`, or remotely with `--gui`): the viewer draws one prim with the checkbox on and three with it off, after each toggle alone; the Keep knobs show enabled only with it on; the screenshot is shared with the user.
  - size: M

**Verification gate:** release gate green with `--gui` including `scene_prune_isolate.py`; `USDMergePrune_Test` and `SceneLoadSetCoverage_Test` pass; PR CI into `feature/f4-heavy-scene-viewer` green or the PM's gate standing in; the panel screenshot shared and OK'd by the user before sign-off. User acceptance happens with F4 - Heavy Scene Viewer's UAT.

## Decisions

- 2026-11-13 — Shape (user): a Prune Unselected checkbox beside Mode, so isolating works with Deactivate and Hide. Rejected: an Operation choice; extra Mode entries.
- 2026-11-13 — What survives (user): matches, their ancestors and descendants, plus Keep Lights, Keep Cameras, Keep Materials and Keep Render Settings checkboxes, all on by default. Rejected: strict pruning; always keeping those types with no controls.
- 2026-11-13 — Unloaded branches (user): in a viewer evaluation an unloaded branch is kept when the pattern's reach can touch it and pruned otherwise. Rejected: pruning all of them (matches inside never show in the viewer); keeping all of them (heavy scenes keep every bounding box).
- 2026-11-13 — Shipping (user): a member of F4 - Heavy Scene Viewer, since USDPrune's matching was rewritten there, and it shares F4's UAT.
- 2026-11-13 — Planner: the Keep types are checked only on loaded prims. Otherwise the default-on Keep Lights would make every unloaded branch reachable and the viewer would never isolate a heavy scene.
