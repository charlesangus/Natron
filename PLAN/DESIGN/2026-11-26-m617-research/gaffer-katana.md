# How Gaffer and Katana draw native (non-USD) scenes

Legend: [F] fact read from source/docs (path or URL given); [I] inference; [K] from general knowledge, not re-verified this session.
Gaffer source was shallow-cloned from https://github.com/GafferHQ/gaffer and https://github.com/ImageEngine/cortex (main, 2026-11-26). Paths below are repo-relative.

## 1. Gaffer

### 1.1 Scene model
- [F] The internal scene is a lazily computed hierarchy, `GafferScene::ScenePlug`, with per-location plugs: bound, transform, attributes, object, childNames, plus globals and sets. USD is not the internal form. [K] Each plug is hashed independently, and computes are cached and pulled on demand (TBB parallel).
- [F] `RenderController::plugDirtied` maps each dirtied child plug to a per-location bit (`BoundComponent`, `TransformComponent`, `AttributesComponent`, `ObjectComponent`, `ChildNamesComponent`, `VisibleSetComponent`, `IDComponent`). Globals and sets have a parallel mask. (`src/GafferScene/RenderController.cpp`)

### 1.2 Renderer abstraction (`IECoreScenePreview::Renderer`)
`include/GafferScene/Private/IECoreScenePreview/Renderer.h`
- [F] Abstract, ref-counted, registered by name via factory: `Renderer::create(type, renderType, ...)`. RenderType is Batch, Interactive or SceneDescription.
- [F] API: `option`, `output`, `attributes` (shared AttributesInterface blocks), `camera`, `light`, `lightFilter`, `object` (name, motion samples, attributes), `pointInstancer`, `command`, `render`, `pause`.
- [F] `ObjectInterface` is a handle: `transform()`, `attributes()`, `link()`, `assignID()`. In Interactive mode, releasing the last handle removes the object. Edits happen while paused. `attributes()` returns false if the renderer needs the object rebuilt (attributes and geometry not independent). So the contract is "edit in place if you can, else replace".
- [F] Backends: "OpenGL" (`src/GafferScene/IECoreGLPreview/Renderer.cpp`), plus Arnold, Cycles, RenderMan, 3Delight, all implementing the same interface. `InteractiveRender` node and `SceneGadget` both drive it through `RenderController`.

### 1.3 RenderController (the incremental-update engine)
`include/GafferScene/RenderController.h`, `src/GafferScene/RenderController.cpp`
- [F] Keeps a persistent `SceneGraph` tree mirroring the scene (separate roots for cameras, lights, light filters, objects; cameras and lights are output first). Each node stores `m_fullAttributes`, `m_fullTransform`, `m_objectInterface`, `m_boundInterface`, and cached hashes (`m_attributesHash`, `m_transformHash`, `m_objectHash`, `m_childNamesHash`).
- [F] Update = recompute only dirty components, skipping work if the plug hash equals the cached hash. `updateChildren` returns early if the child-names hash matches, reuses children by name, and creates new ones for new names. Parent attribute/transform changes propagate via `m_changedComponents`.
- [F] Children update in parallel (`tbb::parallel_for` in an isolated task group). Changed-component state is retained until all children succeed, so an interrupted (cancelled) update resumes correctly.
- [F] Public API: `updateRequired()`, `update()` (sync), `updateInBackground(callback, priorityPaths)` returning a `BackgroundTask`, `updateMatchingPaths()`, `updateRequiredSignal()`, `setScene/Context/VisibleSet/MinimumExpansionDepth`.
- [F] Cancellation: any setter first calls `cancelBackgroundTask()` which does `cancelAndWait()`. `IECore::Cancelled` is rethrown/reported as Cancelled. Other errors clear `m_updateRequired` ("No point updating again, since it'll just repeat the same error").
- [F] Priority paths: with a non-empty set, those paths update first (without signalling completion), then the whole scene.
- [F] Lazy expansion: `VisibleSet` decides which locations are expanded. Unexpanded locations get a single placeholder (bounds) at `<path>/__unexpandedChildren__` instead of children. Heavy hierarchies are only walked as the user expands them. `SceneView` sets minimum expansion depth.
- [F] Anti-flicker in the renderer layer: the source comments "destroying the object before its replacement is ready can lead to the object flickering", so GL objects are replaced, not destroyed-then-created.
- [F] Point-instancer children are not rendered by convention. Failed in-place attribute edits fall back to whole-object replacement (counted in `m_failedAttributeEdits`).

### 1.4 The OpenGL backend (IECoreGL)
`src/GafferScene/IECoreGLPreview/Renderer.cpp`, `cortex/src/IECoreGL/CachedConverter.cpp`
- [F] `OpenGLObject` converts the IECore object to an `IECoreGL::Renderable` using `CachedConverter::defaultCachedConverter()->convert(object)`. The cache key is `object->hash()` (MurmurHash) with a memory-cost-bounded cache. So an unchanged object (same hash) never re-uploads to the GPU, even if the whole location is re-evaluated.
- [F] Edits from controller threads do not touch GL. `transform()` and `attributes()` push lambdas onto a `tbb::concurrent_queue<Edit>`. `renderToCurrentContext` (UI thread, GL context current) first calls `processQueue()`, `removeDeletedObjects()`, then `CachedConverter::clearUnused()` (deferred GL-resource frees: "we can only delete GL resources on the main thread"). The renderer keeps its own reference to each object to iterate for drawing; client handles control lifetime.
- [F] Rendering is by command: `gl:renderToCurrentContext`; picking via `IECoreGL::Selector` ID pass; bounds via `gl:queryBound`. Visualisers (lights, cameras, locators) are drawn by `ObjectVisualiser` plugins, not by a USD schema.
- [F] No instancing in GL ("doesn't support encapsulated instancers"), so no instance IDs. [I] The GL backend is simple and does not scale like Storm for huge scenes, but it is fully under their control.

### 1.5 SceneGadget (viewport)
`src/GafferSceneUI/SceneGadget.cpp`
- [F] `setRenderer("OpenGL")` always creates an interactive OpenGL renderer first ("gives us something to fall back to"). If the user picks another renderer (Arnold, Cycles...), it builds a `CompoundRenderer` of GL plus that renderer, sets `gl:renderObjects` false, and displays the second renderer's output through an `OutputBuffer`; GL is kept for overlays, selection, bounds.
- [F] `updateRenderer()` runs when a layer is drawn: pause the renderer, apply blocking paths, call `updateInBackground()`, then `waitFor(0.1)`. Comment: "we block the UI while our updates occur. This means that for reasonably interactive animation or manipulation, we only show the final result." So: 100 ms grace; if the update finishes in time, the next frame shows it; otherwise the UI keeps drawing the old frame.
- [F] When the background update completes it calls `render()` on the background thread ("to avoid latency"), with `m_renderRequestPending` coalescing UI redraw requests.
- [F] Old frame kept: nothing is cleared at update start. The renderer is only paused and the OutputBuffer/GL scene stays as is until new data arrives. (Source comment-free; [I] this is why there is no flash.) The GL backend mutates the scene via the edit queue at draw time, so the scene is never half-built on screen. Update state is exposed as Paused/Running/Complete.
- [F] Cancel-and-pause on camera change/hide; setters for blocking/priority paths cancel the running task first. Shutdown order: reset task, controller, camera, renderer, then OutputBuffer (renderer may write pixels to it during shutdown).
- [F] Selection/picking: GL ID pass (`openGLObjectAt`), box pick with occlusion query, ID AOV via `RenderManifest` for the non-GL renderer's output.

### 1.6 USD in Gaffer
- [F] Gaffer has no Hydra integration. A grep of `src/`, `include/`, `python/`, `startup/` for "hydra" finds only an Arnold shader-algo test. The release notes (local: `.local/docs/gaffer/1.7.5.0/ReleaseNotes/1.4.0.0.html`) say "USD : Removed Embree Hydra delegate", i.e. it only shipped a render-delegate plugin build of USD in the past, never a viewport path. Release notes 1.6.20/1.7.0 add `hdPrman`-compatible USD writing so files can be rendered in usdview, an interchange concern only.
- [F] USD is I/O via IECoreUSD (`cortex/contrib/IECoreUSD`), implementing `IECoreScene::SceneInterface` (`USDScene.cpp`) over a UsdStage, plus per-type algos (`MeshAlgo`, `CurvesAlgo`, `PointInstancerAlgo`, ...) converting USD prims to/from IECore objects. Nodes `SceneReader`/`SceneWriter` are the only USD touchpoints. Env vars tune it (e.g. `IECOREUSD_USE_MODELAPI_BOUNDS` for faster bounds on big assemblies, release 1.4.8.0).
- [F] Many release notes (1.2.0.2, 1.6.0.0, 1.6.5.0, 1.7.2.0, 1.7.4.0) are fixes for UI-widget flicker, i.e. flicker is a recurring class of bug even in a mature design, mostly fixed by not rebuilding unchanged state.

## 2. Katana

Public docs only. Foundry's FnHdBridge is closed, so scene-delegate details are not documented. Stated as such.

### 2.1 Viewer API and data path
(https://learn.foundry.com/katana/dev-guide/Plugins/Viewer/GettingStartedGuide.html ; https://learn.foundry.com/katana/3.1/dev-guide/Plugins/Viewer/ViewerDelegate.html)
- [F] The Viewer is a Python tab with ViewportWidgets (QOpenGLWidget) wrapping C++ `Viewport` plugins. A `ViewerDelegate` has no GL context and listens to a Geolib3 "terminal op" set via `setViewOp()`. It receives `locationEvent()` per cooked/removed location, caches cooked attributes (`getAttributes()`), and dirties viewports, which redraw on the next idle event.
- [F] Scene data is Geolib3 (location + attribute cook), not a UsdStage. USD enters Geolib via `usdPrimEvent()` (prims added/removed/updated) and the separate "sources" view. Katana 8.5+/9.0 distinguish USD-native paths from Geolib3 paths.
- [F] Only locations exposed in the Scene Graph tab (plus pinned) are in the viewer. `openLocation()`/`closeLocation()` control cooking of children. Lazy expansion again.
- [F] Proxies are computed in a second Geolib3 runtime (`ViewerProxies`) in parallel, flagged `isVirtualLocation`, cached.
- [F] Live edits/manipulators: `setManipulatedAttribute(..., isFinal)`. While `isFinal` is false the value is only held in the delegate cache (`getAttributes()` returns it instead of the cooked value), with no recook ("cooks can be slow"). A recook is requested only on release. Grouped edits via `openManipulationGroup`. Hydra viewer pref `manipulationFreezePeriod` delays interactive updates so they process in batches ("increases responsiveness"). (`.local/docs/katana/9.0/Content/ug/using_hydra_viewer/hydra_manipulators.html`)
- [F] Layers: viewport layer stack (e.g. GLManipulatorLayer, Monitor layer overlays a live render on the viewport). Picking via `pickerDraw()` or `customPick()`; image-based selection from the ID buffer of a render.

### 2.2 Hydra Viewer (Katana 3.0+) vs older VP1
- [F] Katana 3.0 added "Viewer (Hydra)", "substantially more interactive and scalable than the existing Viewer tab" (https://learn.foundry.com/katana/Content/release_notes/whats_new_3.0.html); the legacy viewer remained alongside. Third-party report (DigitalProduction, 2021, https://digitalproduction.com/2021/02/10/katana-schwingt-erneut/): Foundry claimed 7-10x faster than the OpenSceneGraph viewer; instancing, tens of thousands of instances smooth (in test, instances shown as boxes). [F as vendor claim; not independent.]
- [F] Viewer API was rewritten in parallel with the Hydra viewer and diverged from 2.6; plugins on it are excluded from binary-compat promises.
- [F] Default delegate is HdStorm ("GL"); third-party delegates (hdPrman, etc.) load via plugInfo.json; there are controls to pause/stop the delegate on heavy scenes and "Redraw Until Converged".
- [F] Katana 9.0 notes: the Hydra 2.0 Viewer (behind `KATANA_ENABLE_HYDRA2=1`) is built from `FnHdBridge2.0`, "an internal library that will not be publicly available", rendering Geolib data (and USD) directly into Hydra scene indices. A "Hydra Scene Browser" shows the unflattened scene from each viewer's terminal scene index and has a notice logger for dirtied/added/removed/renamed events. Foundry ships an `FnUsdShim` so studios can write their own `SceneIndexFilter`s. Parity with Hydra 1 not reached; USD stages do not yet go through expansion-based loading there.
- [I] FnHdBridge (1.0) is a Katana-specific scene delegate fed from Geolib3 location events (the ViewerDelegate events are the natural feed). FnHdBridge2.0 is the scene-index equivalent. Foundry's choice to move to scene indices in 9.0 (Foundry is replacing the delegate with indices) signals the direction. Not documented; they never expose an Sdf layer/UsdStage as the intermediary for Geolib data.

## 3. Comparison

| | Gaffer | Katana |
|---|---|---|
| Native model | ScenePlug (hash-per-plug lazy compute) | Geolib3 (op-tree cooks, attributes) |
| Viewport | Own GL renderer (IECoreGL) behind a Renderer interface | Hydra (Storm) fed by Katana bridge; legacy OSG viewer retained |
| Other backends | Same interface drives Arnold/Cycles/RenderMan/3Delight | Hydra render delegates (hdPrman etc.) |
| Change detection | per-location per-component hash vs cached hash | per-location cook events |
| Updating | background BackgroundTask, cancel-and-wait, priority paths | cooks in Geolib; viewer dirties and redraws idle |
| Anti-flash | keep old GL scene; edits queued and applied on the draw thread; replace before destroy; 100 ms grace wait | cached `getAttributes()` override during drag; freeze period batching |
| USD | I/O only (IECoreUSD SceneInterface), never viewport | USD stages are an input type; Geolib data feeds Hydra without an Sdf layer |
| Lazy scale | VisibleSet expansion + placeholder bounds | open/closeLocation, exposed locations only |

## 4. Lessons for the decision (inferences)
1. Neither product converts its native scene to a USD layer to display it. Gaffer draws native objects through a renderer interface; Katana feeds Hydra through a native bridge (delegate, now scene index). Option (a) matches Katana and (b) matches Gaffer; the current path (Sdf layer round-trip) has no precedent in either. USD as import/export (IECoreUSD) is the shared pattern.
2. Identity by content hash is the core trick for no-flash and low cost: per-component hashes at each location, and a geometry-conversion cache keyed by object hash (Gaffer CachedConverter), so an unchanged mesh is never rebuilt or re-uploaded. A native scene index should do the same: emit dirty notices only for components whose hash changed.
3. Flicker avoidance is structural: never clear the displayed state when an update starts; stage edits and apply them at draw time (edit queue); replace-then-destroy; coalesce redraws; short grace wait so fast updates land in one frame. A flash is usually "destroy + rebuild" (e.g. prim removed and re-added, or a stage swapped), which a per-prim dirty (`HdChangeTracker`-style) feed avoids.
4. Drag handling: Katana never recooks during a drag (override value cached, commit on release) and batches via freeze period; Gaffer cancels the in-flight update when a new edit arrives and restarts. Both rely on cancellable background evaluation.
5. Lazy expansion (VisibleSet / open-closeLocation) with bounding-box placeholders is what makes heavy scenes viable in both.
6. GL-thread discipline: GL resources are created/freed only on the draw thread; background threads queue work.
7. Katana's evidence on Hydra: it works at production scale, but the bridge is an internal, closed, large library, 2.0 parity is still unfinished in 9.0, and Katana kept the old viewer for years. Hydra is a real cost; Gaffer chose not to pay it and ships a modest GL renderer instead. Gaffer's GL backend lacks instancing; it is adequate because heavy scenes defer to real renderers.
8. A renderer-abstraction layer (Gaffer) buys multiple backends (GL preview plus interactive path tracers) from one change-propagation engine; Hydra's scene-index/render-delegate split is the equivalent if Hydra is used.

## 5. Gaps
- Katana's FnHdBridge internals (scene delegate vs index, threading, how cooks map to HdChangeTracker dirty bits) are not public; the above is inferred from the ViewerDelegate/Viewer API and 9.0 release notes.
- No independent performance numbers for either viewer on heavy geometry.
- Gaffer's ScenePlug compute/hash cache details are from general knowledge, not read this session.
