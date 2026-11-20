# M278 - Translucent Selection Highlight

Under Storm, the 3D viewport highlights selected surfaces inside Storm's own passes (`params.highlight = true`, Gui/SceneViewport.cpp:1067; colour (1, 0.8, 0.1, 1) at :1173). The translucent (OIT) pass gets no selection buffer, so selected projected cards and blended materials show no highlight.

This milestone turns Storm's in-pass highlight off and replaces it with a Natron selection pass: a small Storm render index on the engine's Hgi that draws only the selected prims, with their own materials and blending off, into colour and depth AOVs. A depth-tested composite after `drawGrade()` then tints them, so opaque and translucent surfaces highlight the same way.

## Scope

**In:**
- Selection highlight for every surface type in the Storm viewport (opaque, masked, additive, translucent, projected cards), in Shaded, Unshaded and Wireframe.
- Storm's in-pass highlight turned off in the viewport.

**Out:**
- USDRender primId/picking misses on projected cards (PLAN.md # Open questions, "Storm primId on projected cards"). A follow-up may reuse this pass's technique.
- Highlight for the non-Storm viewport path (`renderProgressive`, Gui/SceneViewport.cpp:2888), which has none today.
- Instance-index and subprim (face/edge/point) selection, an outline style, and a selection-colour preference.

**Edge cases:**
- A selected prim behind an opaque unselected prim fails the depth test, so it gets no tint.
- A selected prim behind an unselected translucent prim is tinted, because translucent surfaces write no depth.
- When an ancestor (an Xform) is selected, all its descendant geometry tints, as Storm's SetSelected does.
- Projection Crop/Outside transparent, and alpha cutouts: materials run in the pass, so discarded fragments stay untinted.
- Half-transparent fragments: the tint strength is the selection alpha times the pass's colour alpha, so a 50% card tints at 50%.
- Invisible prims and purposes the viewport hides: the pass uses the viewport's purposes, so they stay untinted.
- Camera and light prims have no surface; their guides already show their selection.
- An empty selection skips the pass at no cost.
- Engine failure or renderer switch: the pass is destroyed before the engine, because its delegate needs the engine's Hgi (Tests/SceneViewport_Test.cpp:1730-1735).

**Assumptions:**
- From memory of pxr, not yet verified against the 26.08 source: HdxOitRenderTask swaps in its own OIT render-pass shader after HdxRenderTask binds `hdxSelectionBuffer` to the setup task's shader. renderPassOitShader.glslfx imports Selection.ComputeColor, but `HD_HAS_hdxSelectionBuffer` is never defined there.
- A non-OIT HdxRenderTask on the translucent material tag draws those prims opaque and writes the bound AOVs, as HdxPickTask's id pass does.
- Storm render delegates on one Hgi share a resource registry (tested at Tests/SceneViewport_Test.cpp:1719), so plate textures are not uploaded twice.
- The pass depth and the beauty depth of the same opaque prim agree within a small bias.

## Phase 278.1: Hydra API check

- [x] M278.P1.T1 — Confirm the selection-pass API against the 26.08 headers in the published image
  - files: none (read-only); the result goes in this file's ## Decisions
  - approach: In ghcr.io/charlesangus/natron-dev, read these under /usr/local/include/pxr:
    - usdImagingGL/engine.h (the protected `_GetRenderIndex`/`_Execute` and terminal scene index accessors);
    - usdImaging/sceneIndices.h (`UsdImagingCreateSceneIndices`);
    - hd/sceneIndexPluginRegistry.h, hd/renderIndex.h, hdSt/renderDelegate.h;
    - hdx/renderSetupTask.h, renderTask.h, oitRenderTask.h, taskController.h;
    - the task-params plumbing (legacy task scene delegate vs task scene index in 26.08).

    Settle three things:
    1. Whether any supported selection binding exists for OIT. Expected: no.
    2. The host: the engine's terminal scene index, filtered, if it is reachable; otherwise a second stage scene index on the same stage.
    3. The per-material-tag task list.

    Record each as a dated decision, and re-plan any P2 task that no longer fits (PLAN-FORMAT §5a).
  - verify: The decisions name the headers and signatures used. The P2.T1/T2 briefs still hold, or are rewritten.
  - size: L

## Phase 278.2: Selection pass

- [ ] M278.P2.T1 — Scene index filter keeping only the selected prims' geometry
  - files: Gui/SceneSelectionFilter.{h,cpp} (new), Gui/CMakeLists.txt, Tests/SceneSelectionFilter_Test.cpp (new), Tests/CMakeLists.txt
  - approach: An HdSingleInputFilteringSceneIndexBase that hides rprim-typed prims (mesh, basisCurves, points, volume, instancer) not at or under a selected path. Ancestors, materials and their networks, and cameras pass through, so bindings and projector references resolve. `setSelection(paths)` sends PrimsRemoved/PrimsAdded for the difference only. No GL.
  - verify: A gtest on an in-memory stage through UsdImagingCreateSceneIndices: only the selected meshes keep their type, materials stay, and a selection change notifies only the changed prims.
  - size: M

- [ ] M278.P2.T2 — SceneSelectionPass: a Storm render index drawing the filtered prims opaque into colour and depth
  - files: Gui/SceneSelectionPass.{h,cpp} (new), Gui/CMakeLists.txt, Tests/SceneViewport_Test.cpp
  - approach: Build it on the P1 host:
    - render index: an HdStRenderDelegate on the engine's Hgi (`HdDriver{renderDriver, VtValue(hgi)}`), with the Natron renderer scene-index plugins appended as the engine does (LiveTextureSceneIndex for plates), then the P2.T1 filter;
    - tasks: one render-setup task, then one HdxRenderTask per material tag (default, masked, additive, translucent), all with blending off, depth LESS, color+depth AOVs and the same viewport size;
    - API: `render(view, projection, viewport, time, drawMode, purposes, clipPlanes)`, `colorTexture()`, `depthTexture()`, `release()`;
    - repr follows the draw mode, so Wireframe draws lines;
    - it must run inside the caller's StormGLSection, and is destroyed before the Hgi's owner.
  - verify: A new SceneViewportStorm test renders the pass alone on projectedCardScene with /World/Card selected and reads the AOVs: alpha ≈ 0.5 at (97,128), and no coverage at (159,128) (the cropped half) or on the backdrop.
  - size: L

## Phase 278.3: Viewport highlight

- [ ] M278.P3.T1 — Composite the pass in the Storm viewport and drop Storm's in-pass highlight
  - files: Gui/SceneViewport.cpp, Gui/SceneViewport.h
  - approach:
    1. Under Storm, set `renderParams().highlight = false` and stop calling `engine->SetSelected`; `applySelection` feeds the filter instead.
    2. Build the pass in `ensureEngine`, after the engine. Destroy it before the engine in `discardFailedEngine`, `engineFailedToStart`, the renderer switch and `releaseGL`.
    3. In `renderLocked` (:2931-2954), after `drawGrade` and before the grid, guides and handles, skip the pass when nothing is selected. Otherwise render it with the same frustum and params, then draw a full-screen triangle:
       - `gl_FragDepth` = the pass depth minus a small bias;
       - `GL_LEQUAL` against the fbo depth `drawGrade` wrote (:236), with depth writes off;
       - colour = mix(dst, selColour.rgb, selColour.a * passAlpha), using the existing selection colour.
    4. Restore GL state as `drawGrade` does.
  - verify: The existing highlight tests pass unchanged (Tests/SceneViewport_Test.cpp:535, :907, :1353, and :1134 RestoresTheGLStateItChanges). `getEngineCount()` is still 1.
  - size: L

- [ ] M278.P3.T2 — GL tests: a half-transparent projected card highlights like an opaque prim
  - files: Tests/SceneViewport_Test.cpp
  - approach: Rewrite PicksAndHighlightsAProjectedCard (:566) so that, with /World/Card selected, (97,128) moves toward yellow by at least the opaque box's margin scaled by 0.5, while (159,128) and the backdrop stay within 6. Then add tests for:
    - an opaque occluder quad at z=+0.5 covering part of the card: the covered part is untinted and the uncovered part tinted;
    - the selected opaque /World/Backdrop tinting through the card's transparent half;
    - restoring the colours when the selection is cleared;
    - Wireframe mode.

    Use the existing helpers (`isYellowish` :1207, `isBluish` :302).
  - verify: A remote gate with `--gui`: SceneViewportStorm.* passes under Xvfb/llvmpipe in the published image.
  - size: M

**Verification gate:**
- A remote build, with `SceneViewportStorm.*` and `SceneSelectionFilter*` green under Xvfb/llvmpipe.
- PR CI (debug asserts) green.
- In the GUI under Xvfb, an AppImage screenshot of a selected half-transparent projected card next to a selected opaque box, shared with the user and OK'd before sign-off.

## Decisions

- 2026-11-20 — Elaborated from the stub after the claim; its blocker, M234 - Camera Projection Nodes, is done.
- 2026-11-20 — No pxr patch: USD comes prebuilt from aswf/ci-vfxall (cmake/NatronUsd.cmake:3-6), and no public switch binds selection into the OIT pass.
- 2026-11-20 — The brief's "existing prim-ID pass" doesn't exist. Viewport picking is an on-demand `TestIntersection` (Gui/SceneViewport.cpp:3170-3173, :3229-3232), and the beauty renders only colour and depth AOVs (:1171). A per-frame full-resolution pick readback would stall a tumble, so the selection pass is its own pass.
- 2026-11-20 — No second UsdImagingGLEngine with scene materials off. Its fallback material drops the projection's discard, so the card's transparent half would tint; the assertion at Tests/SceneViewport_Test.cpp:594-599 must keep holding. It would also have no alpha to weight a translucent tint, and it would change `getEngineCount()` (:1362, :1396).
- 2026-11-20 — The tint is weighted by the pass's alpha, so a 50% card tints at 50% rather than turning solid yellow. Risks: the first selection compiles the pass's material shader variants (a possible hitch on llvmpipe), and selecting the root of a huge scene doubles its geometry in GPU memory (textures are shared through the common registry).
- 2026-11-20 — P1.T1, checked against the 26.08 headers in ghcr.io/charlesangus/natron-dev:2c1cbed46e197e27 and the v26.08 sources of hdx/usdImagingGL:
  - **OIT:** no supported selection binding. `HdxOitRenderTask`'s translucent sub-pass runs its private `_translucentPassShader` without selection (oitRenderTask.cpp:165-298; selection is bound only in renderTask.cpp:193-235). A translucent material's alpha≥1 fragments do highlight today, through the opaque sub-pass.
  - **Terminal scene index:** private, so it can't be reached (engine.h:844-878). Reachable as protected: `_Execute`, `_renderer`, `_taskControllerSceneIndex`, `_renderCollection` and `_ComputeRenderTags` (:717-836).
  - **Plates:** LiveTextureSceneIndex registers only for Cycles and MoonRay; Storm reads `natron://` through the Ar resolver.
  - **Tasks:** the stock task controller can't be reused, because it forces each tag's blend and depth state and sends translucent prims to OIT. Natron authors four HdxRenderTasks (default, masked, additive, translucent), with blend off, depth mask on, depth LESS and alpha-to-coverage off. Volume is left out. Only the first task clears the AOVs.
  - **Instances:** a whole native instance isn't tinted, because its prototype rprims don't live under the instance path. Out of scope, with instance selection.
- 2026-11-20 — **The host is an engine subclass plus a Storm scene-index plugin (user)**, replacing the planned second stage scene index:
  - A "GL" scene-index plugin, armed through a thread-local slot around `SelectionEngine` construction, merges an HdRetainedSceneIndex with Natron's selection tasks and two render buffers into the engine's own render index. Unarmed, it returns its input, so USDRender's Storm engines are untouched.
  - Each task's collection is `_renderCollection` with `SetRootPaths(selection)` and one material tag.
  - This means no second traversal and no duplicate GPU geometry, and depths match the beauty's.
  - **Rewritten briefs:**
    - P2.T1 is now Gui/SceneSelectionTasks.{h,cpp}: the plugin, the retained index, and `setSelection`/`setRenderTags`/`setCamera`/`setSize`, each dirtying only its own locator. Its gtest needs no GL.
    - P2.T2 is now `SelectionEngine : UsdImagingGLEngine` with `renderSelection(params, selection)` after `Render()` (calling `_Execute` with the task paths) and `colorTexture()`/`depthTexture()` from the render buffers. Its verify is unchanged.
    - In P3.T1, `ensureEngine` builds the SelectionEngine in place of the plain engine, and there is no separate pass to destroy.
  - **Wireframe draws lines (user):** the pass follows the draw mode.
