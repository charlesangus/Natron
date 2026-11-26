# Native geometry: bypassing USD staging, or our own viewer: feasibility

Source read: `.worktrees/m615` (origin/milestone/m615-geometry-loops-uat-round-1, 11e3a1b115, which contains f18). Native geometry is not on origin/main yet. OpenUSD **0.26.08** (tools/ci/local/Dockerfile:24, min 2411 in cmake/NatronUsd.cmake:13). The pxr headers exist only inside the dev image, which I did not touch. API claims below come from `.local/docs/usd/api` and from how this code already uses the API. Items marked **[verify]** need a look at the 26.08 headers or source.

## 0. How native geometry reaches the screen today

- **ViewerInstance::evaluateGeometryInput** (Engine/ViewerInstance.cpp:4041). It evaluates the node (:4053), then `GeoViewerLayer::serveFrame` (:4074, GeoViewerLayer.cpp:160) hands back a ScenePayload holding a single `natrongeo:` layer, opened once (GeoViewerLayer.cpp:246-273).
  - On failure, `lastScene = ScenePayload()` (:4026), which clears the view.
- **SceneStage.** The view's SceneStage (Engine/Nodes/Scene/SceneStage.h:79) has an anonymous root that sublayers the payload. It is created at Gui/SceneViewport.cpp:4129.
  - `GeoLayers::refresh` (GeoSdfFileFormat.cpp:591-700) builds a new GeoUsdConvert, adopts the unchanged partitions, and diffs the specs inside an SdfChangeBlock.
  - A topology change erases the prim and authors it again; a points-only change edits values.
- **GeoUsdConvert** (GeoUsdConvert.h) is a lazy *spec model*: `specType`/`field`/`visitSpecs`/`partitionValues`. GeoSdfData serves it to Sdf as SdfAbstractData.
- **Drawing.** USD change processing feeds UsdImaging's stage scene index and its filter chain, then our renderer plugins, then Storm.
- **GeoToUSD** (GeoToUSD.cpp:~930/1000) serves the *same* file format into the USD node graph. Its copy mode (:965/1013) calls `GeoUsdConvert::author`. Final render and USDWrite only ever see layers and stages.

## 1. Inventory of what consumes the staged native geometry

Legend: **S** = reads the UsdStage, **L** = Sdf layer / natrongeo registry, **C** = GeoUsdConvert/GeoDetail, **H** = Hydra (render index / scene index / AOVs).

| Consumer | Where | Reads | Survives a scene-index bypass? |
|---|---|---|---|
| Storm draw | SceneViewport.cpp:4738-4755, 6143-6196 `engine->Render(stage->GetPseudoRoot())` | S→H | Yes, if the native SI is merged into the engine's chain |
| Engine construction, plugin arming | SceneViewport.cpp:2059-2112 (SelectionEngine, SceneDrawFilter::EngineScope) | H | Yes. The same arming slot is where the native SI goes in |
| Prim picking (Storm) | SceneViewport.cpp:6491-6600 `TestIntersection(..., stage->GetPseudoRoot())`, results' `hitPrimPath` | H (+S root) | Likely, **[verify]**: in scene-index mode TestIntersection maps hits through HdxPrimOriginInfo, so native prims may need an HdPrimOriginSchema `scenePath` (= own path) or they could come back empty. No code here sets primOrigin today |
| Marquee pick | SceneViewport.cpp:6609-6676 | H | Same as above |
| Component pick (points/edges/faces) | SceneViewport.cpp:6696-6785, SelectionEngine.cpp:228-260 `testComponentIntersection` (raw HdxPickHit objectIds = render-index paths) | H | Yes. It needs no primOrigin |
| Pick to GeoDetail mapping | GeoViewerLayer.cpp:361-635 (`pickPrimitives`, `mapComponentHits`, `componentHighlights`), GeoDisplay.cpp:992-1063 | C | Yes, as long as the SI keeps GeoUsdConvert's paths and element numbering |
| Non-Storm pick | SceneViewport.cpp:6500-6540 via `progressive->pick` (AOV-based; HydraRenderImpl.cpp:1995, 4303 `DecodeIntersection`) | H | Same primOrigin caveat **[verify]** |
| Selection tint pass | SelectionEngine.cpp:149-197 (collection root paths), SceneViewport.cpp:2810-2851 | H | Yes. Collections filter by path |
| Component highlight | SelectionEngine.cpp:263 `_selTracker->SetSelection(HdSelection)` | H | Yes |
| Display highlight (points/faces resolved to world) | SceneViewport.cpp:3393-3480: `UsdGeomPointBased::GetPointsAttr`, `UsdGeomMesh` topology, `UsdGeomXformCache` | **S** | No. Needs a native resolver (easy: read the GeoDetail/convert) |
| Grade/OCIO | SceneViewport.cpp:2711 `drawGrade` (reads the colour/depth AOVs) | H | Yes |
| Stale cue | Gui/ViewerTab.cpp:1427-1443 (timer-driven tint over the frame) | — | Yes |
| Progressive admission / hide rules | SceneDrawFilter.cpp:270 (allRenderers), visibility + instancerTopology mask; SceneViewport.cpp:2107 | H | Yes, if the native SI merges *upstream* of the filter |
| Stand-in boxes | Gui/SceneDrawBoxes.cpp:115-158, 642-687: `UsdGeomBBoxCache`, `UsdGeomPointBased`, `UsdGeomPointInstancer` | **S** | No. Needs native bounds (packed bounds and point bounds exist in GeoPacked/geoDetailBounds) |
| Framing (F / frame selection) | SceneViewport.cpp:5437-5488: `UsdGeomBBoxCache::ComputeWorldBound(pseudoRoot)` | **S** | No. Needs a native bounds union |
| Transform handles on prims | SceneViewport.cpp:2961-2973, 5860-5899; SceneTransformWidget.cpp:70-238; SceneGizmo.cpp:266-297 (`UsdGeomXformable`, ordered ops) | **S** | Partly. A geometry-only view mostly uses op handles (`computeTargetSpace` :3020 reads the prim's space) and component handles (C). Prim-level handles on native prims would need a native frame |
| Drag preview of prim transforms | SceneStage.cpp:428-518 `setPreviewTransforms` (authors ops on the root layer) | **S/L** | No. Native prims would need an xform override in the SI (that is easier, a dirty on xform) |
| Geometry guides (markers, normals, packed-USD bounds, group highlight) | GeoDisplay.cpp:160-376 (collected from the GeoDetail), authored as guide-purpose prims in an anonymous Sdf layer at GeoDisplay.cpp:377-505, 631-655, 862-900; TfNotice resync counter :539 | C + **L** | Must move: either a second retained SI, or leave it USD (it is O(N) Sdf per new detail, a listed hotspot) |
| Point/primitive labels | GeoDisplay::paintLabels (from the convert) | C | Yes |
| Camera/light guides, look-through, render-settings prim, headlight decision | SceneViewport.cpp:1385-1450, 1545-1600, 1682-1816; SceneGuides.cpp:240-431; HydraRenderImpl.cpp:3092/3849 | S | Unaffected. Native geo has no cameras or lights. The stage would be empty in a geo-only view, which is fine |
| Outliner / scene graph | SceneGraphModel.cpp:83-139, 360-663, 1047 (TfNotice), 2956; SceneGraphPanel.cpp:158-188 | **S** | No. Native prims become invisible to the outliner unless it grows a native source |
| Prim list, picker, breadcrumbs, inspector | ScenePrimListPopup.cpp:255-275, ScenePrimPickerPopup.cpp:97-501 (`SceneOps::selectPrims` expressions), SceneBreadcrumbBar.cpp:82-91, ScenePrimInspector.cpp:485, 1038 (composition query) | **S** | No. Same as above |
| Match search, selection actions | SceneNodeStageSource.cpp:517 (own SceneStage per node), SceneSelectionActions.cpp:1266-1528 | **S** | No. Same as above |
| Geometry spreadsheet | GeoSpreadsheet.cpp:209-217, 661-702 (`viewer->getShownGeometry`) | C | Yes, already native |
| Viewer Cycles/MoonRay | HydraRenderImpl.cpp ProgressiveRender (:2965), a *second* SceneStage `workerStage` synced from `sceneStage->getSnapshot()` (:3793-3799; SceneViewport.cpp:3999-4005), engine from `newEngine` (:489-507) | S→H | Only if the native SI is also armed into the worker's engine, *and* given a snapshot of the same geometry (the snapshot is layer-based today) |
| Cycles/MoonRay/Storm plugin filters | CyclesAovs.cpp:617, MoonRayAovs.cpp:157 (uses UsdImagingAdapterRegistry :463-485 for type mapping), LiveTextureSceneIndex.cpp:459, StormTextureBudget.cpp:808, StormPtexBindingFilter.cpp:356, StormAddedPrimDedupe.cpp:133, StormIdPass.cpp:435 | H | Yes. All pure Hd schemas downstream of the merge |
| Materials | GeoUsdConvert writes no material bindings; only `Cd`→`primvars:displayColor` (GeoUsdConvert.cpp:1237) | — | Trivial: displayColor primvar, no material prims |
| Packed / instancing | GeoUsdConvert: PointInstancer with `Prototypes` children (:1170-1196, 1477-1530); "native instances" = `instanceable` + internal references (:1511-1513); packed USD = an `over` on the source path (only bounds guides draw in a geo-only view) | L→UsdImaging (PI adapter, NI prototype propagation) | Must be synthesised natively: Hydra instancer + instancerTopology + instancedBy. NI mode would become a Hydra instancer as well (UsdImaging does this for us today) |
| GeomSubsets (prim groups) | GeoUsdConvert.cpp:1461-1474 | L→UsdImaging | Native: geomSubset child prims (HdGeomSubsetSchema) |
| Final render / export | USDRender, USDWrite, HydraRenderImpl take only ScenePayload layers; GeoToUSD serves natrongeo or authors via `GeoUsdConvert::author` | L/S | Unaffected *if the bypass is viewer-only*. Parity risk exists only if the SI stops deriving from GeoUsdConvert |
| Mixing native geo with USD scenes | Happens only downstream of GeoToUSD, as real layers with overs composed onto input prims (inputScene, GeoUsdConvert.h input-scene docs) | S (composition) | Out of reach for an SI: overs on USD prims need composition. Keep that path staged |
| Python | GeoPython.cpp/GeoPyBuffer.cpp use the GeoDetail only; no GUI Python for viewer pick/selection found | C | Unaffected |
| Upstream push of edits | GeoEdit.cpp:41, GeoScatter: `refreshLayers`/`findLayers` | L | Needs the native equivalent ("detail changed" → SI diff) |

**Bottom line of the inventory:** the *drawing* side (Storm draw, pick, selection, grade, admission, delegate filters) is already Hydra-level and would mostly survive. The *query* side (framing, stand-in boxes, display highlight, prim transform handles and preview, outliner/inspector/picker/search, guides layer, the Cycles/MoonRay worker snapshot) reads the UsdStage and would each need a native path, or would just lose native geometry.

## 2. Existing scene-index plumbing ("we already use that pattern")

The spike was right. There are eight renderer scene-index plugins, all registered via `HdSceneIndexPluginRegistry::RegisterSceneIndexForRenderer(..., InsertionOrderAtStart)`. Two of them already **merge an HdRetainedSceneIndex into the engine's chain**:
- `StormIdTasks::append` (Engine/Nodes/Scene/StormIdPass.cpp:446-466): `HdMergingSceneIndex::New(); AddInputScene(input, "/"); AddInputScene(retained, prefix)`.
- `SceneSelectionTasks::append` (Gui/SceneSelectionTasks.cpp:336-346): the same pattern.

**Per-engine targeting** uses thread-local arming objects that live across the `UsdImagingGLEngine` constructor (StormIdTasks::Arming, SceneSelectionTasks::Arming, SceneDrawFilter::EngineScope, SceneViewport.cpp:2071). The engine subclasses (`SelectionEngine`, `StormIdEngine`) reach protected members `_taskControllerSceneIndex`, `_renderer`, `_sceneDelegateId`, `_intersectCollection`, `_selTracker`, `_Execute`, and `_UpdateHydraCollection`.

**How a native-geometry SI would slot in:**
- Write a `NativeGeoSceneIndex : HdSceneIndexBase`.
- Arm it through the same kind of thread-local scope, with an `allRenderers` registration.
- Merge it in the *first* plugin, so SceneDrawFilter, the selection and id tasks, and the Cycles/MoonRay filters all see it. The cleanest way is to do the merge inside `SceneDrawFilter::appendFilter` itself, because InsertionOrderAtStart ordering between our own plugins is "UNDOCUMENTED" (SceneDrawFilter.cpp:259-262).
- The engine would still be given an (empty or guide-only) UsdStage root for `Render()`/`TestIntersection()`.
- Paths must sit under `_sceneDelegateId` (SelectionEngine.cpp:155 uses ReplacePrefix) and inside the render collection roots.

**Does UsdImagingGLEngine accept extra scene indices?**
- There is no public "add a scene index" API on the engine, and `Parameters` has no scene-index field (docs: class_usd_imaging_g_l_engine).
- The supported routes are (a) renderer scene-index plugins, which is what we do, and (b) `_CreateUsdImagingSceneIndices`/`_CreateSceneIndexChainAndRenderer`, which are protected **non-virtual**, so not overridable.
- `HdRenderIndex::InsertSceneIndex` exists, but the engine's render index is not exposed in the member list (`_renderer` is the 26.x HdRenderer wrapper). **[verify]**
- Plugins see data *after* UsdImaging's own chain: flattening, material-binding resolution, NI/PI prototype propagation, the display-style override and the selection SI. So the native SI must supply already-flattened data: `xform` with `resetXformStack`, `visibility`, `purpose`, `extent`, `primvars`, `mesh`/`basisCurves`/`points` topology, `instancerTopology`, `instancedBy` and `geomSubset`. Refine level / cull style overrides from render params would not reach it unless we add them.
- Render tags (guide/proxy/render purposes) come from `HdPurposeSchema`, so the native SI must author `purpose`.

**Can non-stage prims be picked?**
- Component picks: yes. `testComponentIntersection` returns raw render-index objectIds (SelectionEngine.cpp:228-260).
- Whole-prim `TestIntersection`/`DecodeIntersection` in scene-index mode resolve paths through prim-origin info **[verify in 26.08 usdImagingGL/engine.cpp]**. Expect to add `HdPrimOriginSchema{scenePath: own path}` to every native prim, or to call the pick task directly as `testComponentIntersection` already does (`pickTarget = pickPrimsAndInstances`). Either is a small change. There is no fundamental blocker.

## 3. Work estimates

### (a) Native scene index for viewer native geometry (viewer-only; GeoToUSD/export stay USD)

**Build:**
1. **NativeGeoSceneIndex data sources, from GeoUsdConvert.** Reuse its partitioning, paths, attribute→primvar mapping and `partitionValues()` VtArrays (copy-free); flatten `xformOp:transform` to `HdXformSchema`. This covers mesh, curves, points, PointInstancer (Hydra instancer + instancerTopology + prototypes + instancedBy), native instances (synthesised as instancers, the UsdImaging NI job), GeomSubsets, extent, purpose, visibility and primOrigin. Size: **L (2-3 weeks)**.
2. **Change diffing → HdSceneIndexObserver.** Use `adoptUnchangedPartitions` and `partitionSignature` to emit PrimsAdded/Removed and DirtyPrims with exact locators (`primvars/points` for point edits, `mesh/topology` for rebuilds). This replaces `GeoLayers::refresh` (GeoSdfFileFormat.cpp:591-700) and USD change processing. Size: **M**.
3. **Plumbing.** Arming/merge into the Storm viewer engine *and* the ProgressiveRender worker engine (HydraRenderImpl.cpp:3793). The worker uses a *snapshot* of layers, so it needs a geometry-snapshot equivalent that is thread-safe against the viewer's edits. Also: stage lock semantics (the SI must not change while Hydra syncs, same rule as SceneDrawFilter.h:95-100) and a ViewerInstance/GeoViewerLayer API change, where the payload becomes "detail + options" instead of a layer. Size: **M**.
4. **Native replacements for the stage readers:**
   - framing bounds (5437)
   - stand-in boxes (SceneDrawBoxes)
   - display highlight (3393)
   - prim transform handles/preview for native prims (SceneStage::setPreviewTransforms → SI xform override)
   - the GeoDisplay guide layer → retained SI (also kills the O(N) Sdf guide rebuild)
   - GeoEdit/GeoScatter `refreshLayers` callers
   
   Size: **M-L**.
5. **Outliner/inspector/picker/search.** Either native geo vanishes from them in a geometry-node view (acceptable? the geo spreadsheet is the native inspector), or SceneGraphModel gets a second, native source. Size: **M if added**.
6. **Tests.** The existing GeoViewerLayer_Test, m361_geo_pick and m559 checks assume layers. Pick/selection/highlight GUI checks need to be redone. Size: **M**.

**Total:** about **6-10 engineer-weeks**, medium-large, as the spike said.

**Keeps working:** Storm draw, grade/OCIO, stale cue, the selection/id passes, admission and hide rules, Cycles/MoonRay filters, component picking, GeoUsdConvert-based pick mapping, the spreadsheet, and final render/export.

**Export parity risk:** **low**, *if* the SI is driven by GeoUsdConvert (same partitioning, names, values). It is **high** if written from GeoDetail independently. What remains is semantic drift between UsdImaging's interpretation (PI/NI adapters, subset handling, primvar interpolation, implicit normals, orientation, `doubleSided`) and ours. Viewer ≠ render surprises would show up in edge cases. Mitigation: a parity test that renders the same detail through both paths and compares the Hydra scene dumps (HdSceneIndex prim data sources) prim by prim.

**Gains:** no Sdf diff, no USD change processing, no UsdImaging adapter resync. Points-only drags dirty one locator. Topology changes still rebuild Storm rprims and batches, so Storm's own cost is unchanged.

### (b) Own drawer

**Native-geo-only GL drawer alongside Hydra**, reimplementing:
- mesh/curve/point/instance drawing, with display modes (smooth, flat, wire, wire-on-shaded, points) and lighting matching Storm's look
- depth compositing with Hydra's colour/depth AOVs (drawGrade reads them; both renderers share one depth, which is feasible since Storm's depth AOV is already read)
- prim, component and marquee picking (an own id buffer)
- selection tint and component highlight
- admission/hide rules, stand-in boxes and budgets (M481 polygon budgets), guides
- clip planes, look-through camera, OCIO path integration
- GPU memory budget, cancel-fast behaviour
- and: **no Cycles/MoonRay viewer for native geo** unless it is still staged to USD for those delegates, which means two code paths and visual mismatch between Storm-view and render-view

Size: **XL (3-6 months)** for parity, plus permanent upkeep of two renderers.

**Fully own viewer (replace Hydra):** loses USD scenes, materials (MaterialX/Preview), lights, textures and every delegate. It is not viable for this product direction (native nodes → USD/Hydra → deep).

## 4. Middle grounds

1. **Points-only override SI (smallest).** Keep USD staging for structure and topology. On a drag step whose partitions' topology signatures are unchanged, skip `GeoLayers::refresh` and push the new points/primvar VtArrays through a small filtering SI. It sits first in the plugin chain and overrides `primvars/points` (and `xform`) for those paths, dirtying only those locators. Re-sync the Sdf layer lazily (on drag end, idle, or before any stage query, export or snapshot).
   - Stage readers (framing, boxes, highlight) see slightly stale points mid-drag. That is acceptable, or they can read the override.
   - Size: **S-M (1-2 weeks)**. This is the M620 prototype shape.
   - Risk: two sources of truth during a drag. The worker snapshot for Cycles/MoonRay would lag unless the override is shared.
2. **GeoUsdConvert-backed SI only for the geometry-node viewer, staged USD for everything downstream of GeoToUSD.** This is option (a) scoped as above. It is the "Blender HYDRA vs USD export" split, with parity anchored on one converter.
3. **Build an HdRetainedSceneIndex directly.** Simpler than a custom SI, since there is no lazy GetPrim and the existing code already uses HdRetainedSceneIndex. But it costs a full data-source rebuild per change and needs eager values, which loses GeoUsdConvert's lazy per-partition build. It is fine for guides (the GeoDisplay guide layer is the best first candidate) and marginal for multi-million-point geometry.
4. **Guides first.** Move the GeoDisplay guide layer (markers, normals, packed bounds, highlight) to a retained SI now. It removes one of the measured O(N)-through-Sdf per-cook costs without touching the geometry path, and it proves the merge, arming and pick path (including the primOrigin question) on low-risk prims.

## Biggest risks

- **The Hydra-level picture is good; the stage-query surface is the hidden cost.** About 12 GUI consumers read the UsdStage for geometry the viewer shows (framing, boxes, highlight, transforms/preview, outliner family, search).
- **ProgressiveRender's worker stage is layer-snapshot based.** A non-stage geometry source needs its own thread-safe snapshot, or Cycles/MoonRay viewers silently drop native geo.
- **Prim-level picking through TestIntersection** may need primOrigin **[verify]**.
- **Plugin order between our own InsertionOrderAtStart plugins is undocumented.** Merge inside the first plugin.
- **Semantic drift from UsdImaging** (instancing, subsets, interpolation): viewer/render mismatch unless the SI is built from GeoUsdConvert and parity-tested.
- **The bypass cannot cover native geo composed into USD scenes** (overs on input prims). Those remain staged, so both paths exist forever.
- **None of this is measured.** M618 measurement should still decide whether (a) or middle ground 1 is worth it.
