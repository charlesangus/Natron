# How Houdini feeds geometry to its viewports (research for M617)

Legend: [DOC] = stated in a cited source. [INF] = my inference. [GAP] = could not be confirmed from public sources
(closed-source; HDK exposes only the plug-in surface). Local doc root: `.local/docs/houdini/` (abbrev. LD). HDK root: https://www.sidefx.com/docs/hdk/

## 1. The SOP / OBJ viewport (native path)

Layers [DOC: HDK_viewport, https://www.sidefx.com/docs/hdk/_h_d_k__viewport.html]:
- GUI hooks decide what is drawn: `GUI_PrimitiveHook`, `DM_SceneHook`/`DM_SceneRenderHook`, registered via `DM_RenderTable`.
- Input: `GU_Detail` primitives presented as `GT_Primitive` (refined, ref-counted via `GT_PrimitiveHandle`) or raw `GEO_Primitive`. A filter hook can swap one GT prim for another (e.g. sphere -> polygon mesh refinement, curves refined using display LOD).
- GR layer: `GR_Primitive` owns update+render logic; helper `GR_GeoRender` classes (`GR_PolySurfaceGL3`, `GR_PointsGL3`, `GR_VolumeGL3`, `GR_PolySurfaceVK`...; suffix = backend GL3/GL4/VK) accept specific GT types and own the GPU buffers.
- RE (OpenGL) / RV (Vulkan) layer: `RE_Geometry`, `RE_VertexArray`, `RE_Shader`, `RE_Light`, `RE_ShadowMap`; `RV_Render`; `RE_RenderContext` wraps whichever is active. Three tiers of authoring are offered: raw GL/Vulkan, RE/RV, GR.
- Vulkan became the long-term replacement; staff said H20 aimed for parity and Vulkan as next default; H21 users still report transparency/instancing problems [DOC/forum: https://www.sidefx.com/forum/topic/92725/ , https://www.sidefx.com/forum/topic/101955/ , https://www.sidefx.com/forum/post/449357]. Some render passes (pre/post/full-scene) were still GL-only in H20 [DOC, HDK_viewport]. Hand-written GLSL deprecated for Vulkan in favour of MaterialX [forum quote, unverified by me].
- Houdini still ships its own GL/Vulkan viewport code = two backends maintained in parallel (GL3/GL4/VK GR classes) [DOC].

Incremental update model [DOC, HDK_viewport + GR_Primitive class page https://www.sidefx.com/docs/hdk/class_g_r___primitive.html]:
- A `GR_Primitive` persists across `GU_Detail` changes. After a detail changes, `acceptPrimitive()` is called on each previously generated GR primitive so it can be reused; primitives are deleted when the detail stops being displayed, the prim is removed, or reordering prevents matching. Reuse "is intended to improve performance".
- `update()` triggers: geometry change, selection change, LOD change, instancing change, scene-level change, GL state change (`needsGLStateCheck`/`checkGLState`), view change (`updateOnViewChange`; `viewUpdate()` for view-only). `update()` builds buffers/textures; `render()` is cheap and may run several times per redraw (beauty, shadow, wire-over). "update() is called much less often than render".
- Display-option versioning: `GR_DispOptChange` = UNCHANGED / CHANGED / VERSION_CHANGED, `getDisplayOptionVersion()`/`bump...` [DOC].
- `updatePrim()` is invoked "whenever the metacount version on the GU_Detail is bumped" [DOC]. Metacounts/data IDs are the SOP-side change counters (topology, per-attribute-class, per-attribute data IDs). [INF: the HDK GR_Primitive page does not enumerate them; GA_DataId is the underlying mechanism in the GA library but I could not fetch a page documenting it.]
- Viewport-facing attribute contract [DOC, HDK_viewport_GR]: `__topology` is an int64 that changes when topology changes ("if it remains constant, the topology has not changed"); `__primitive_id`/`__vertex_id`/`__point_id` int32 = GA_Offset, needed for selection. GT_Primitive exposes `getDataIdHash()` (hash over attribute lists, optionally cached only if the prim is immutable e.g. packed contents), `getTopologyVersion()`, `getUniqueID()`.
- Topology vs P-only: the documented signals are a topology version plus per-attribute data IDs; so a P-only change can re-upload just the position buffer and keep index buffers. [INF, consistent with the `__topology` doc and with the GR cache name passed to `GR_GeoRender::update()`; no explicit "P-only path" is documented. GAP.]
- Picking buffers are regenerated only when the scene changes [DOC].
- Heavy geometry: packed primitives are the main mechanism. Display option "Distance-based Packed Geometry Culling" swaps packed geo for bounding boxes once frustum polygon count exceeds "Scene Polygon Limit" (farthest first) [DOC: LD ref/windows/displayopts_3d.md ~L1266-1282]. Subdivision polygon limit exists too (L396). Packed prims are the instance/LOD unit.
- Double buffering / flash avoidance [GAP]: nothing in HDK docs. What is documented is structural: GR objects survive across cooks and are updated in place (above), so a knob drag that changes only P updates existing GPU buffers rather than destroying and recreating the drawable; the previous frame's GL buffers remain drawable until update() replaces them. [INF] The cook itself runs on the main/cook threads, with the viewport redrawing the last-cooked detail until the new one is available [INF, not documented].
- Cancellation [GAP]: no HDK doc on viewport-cancel. Houdini cooks are interruptible via UT_Interrupt (general knowledge, not verified here) [INF].

## 2. Solaris / LOPs

How SOP geometry enters USD [DOC: LD solaris/sop_import.md; LD solaris/performance.md]:
- SOP Import LOP / SOP Create LOP translate GU_Detail -> UsdGeom prims; USD Configure SOP / attributes (path attrs, usdmaterialpath, GeomSubsets) steer the conversion. Attributes are time sampled by default; docs say USD and "specifically Hydra do more work when values are time sampled, even if there is only a single time sample" -> use "Set Default Values" + Topology Attributes "Static".
- Solaris also ships an Sdf file-format plugin so `.bgeo` can be sublayered/referenced directly, configured by USD Configure SOP detail attributes (LD performance.md "Files and Layers"). This is the same "geometry as an Sdf layer via a file-format plugin" shape you use. [DOC]. It is a load path, not an interactive-edit path.
- Python LOP: Sdf-level edits inside `Sdf.Changeblock()` "get massive performance boosts" [DOC].

Stage management [DOC: HDK LOP stages page https://www.sidefx.com/docs/hdk/_h_d_k__op_basics__lop_stages.html]:
- Each LOP node holds an `HUSD_DataHandle` (shared-pointer-like) over `XUSD_Data` wrapping a UsdStage; one stage is shared by many nodes. Locks: read, write (edit target = active layer; only in cook), layer lock (write to one SdfLayer; from a write lock it triggers recomposition per edit unless wrapped in an SdfChangeBlock; directly from the handle it edits an off-stage layer). Stage structure: empty root layer + sublayers (files or anonymous "LOP Layers"); strongest is the Active Layer, the only editable one.
- Cost of sharing: locking a stage already locked by another node forces upstream recooks to find a node that does not share it, then a new stage; one of two stages discarded. Taking write+layer locks at once = "expensive double lock".
- Viewport handle: the LOPs Scene Viewer uses its own, never-shared handle that mirrors the display LOP's stage with `SdfLayer::TransferContent()`, which "only writes differences", limiting the edits Hydra must process and "lets the viewport pause updates". Viewport overrides (visibility, activation, selectability, solo) live in the session layer so the LOP stage is untouched. This is the key published Houdini technique: diff layer content into the viewer's stage; same as your per-edit diff idea. [DOC]
- Layer colours: each colour change in the network marks a new layer on the stage = composition overhead; many opinions on a layer slow the viewport; Configure Layer starts a fresh layer above to fix slow simple transforms [DOC, LD performance.md L42-44].

Known Solaris viewport limits [DOC LD performance.md + forum]:
- Hundreds/thousands of sibling prims slow USD and Hydra; consolidate meshes, use GeomSubsets. Per-prim purpose adds overhead for the viewer Hydra. Scene Polygon Limit/distance culling also applies. Unload payloads, avoid activation toggles (recomposition), population masks.
- SOP-network editing inside SOP Create/Modify: SideFX's own advice is to hide Solaris panels / set viewport "Hide Other Objects", or park an Output SOP on a null, "which will prevent the Solaris UI from re-translating your SOP work after every little tweak" (LD performance.md L34). I.e. SideFX concedes the SOP->USD translation path is too slow for interactive tweak loops and routes around it by showing the native SOP viewport while editing. [DOC + INF]
- Time-dependent SOP Import: "SOP Import only authors the current frame so each frame change requires a new layer to be recomposed"; "Background Update USD Stage" took ~32 s on a heavy RBD transform case vs near-instant for pre-cached time-sampled layers; staff (robp_sidefx) advise a Cache LOP; mtucker notes the viewport pause button ("Lock viewport stage to prevent renderer updates") exists and also affects Houdini GL [DOC forum: https://www.sidefx.com/forum/post/379562/ ; related https://www.sidefx.com/forum/post/402119 ].
- Mixed anecdotes: some users find Solaris GL slower than OBJ for the same heavy scene, others find it faster [forum, anecdotal: https://www.sidefx.com/forum/post/393808]. No official benchmark found.
- Instancer LOP: viewer shows prototypes with distance culling past the polygon limit; tens of millions of points display slower than raw points [forum, anecdotal].

Delegates [DOC: LD solaris/jumpstart.md L200, L439; glossary.md L691]:
- LOPs viewport uses its own "Houdini GL" delegate (selection, handles, prim kinds); other choices Storm ("Hydra GL"), Karma, Vulkan (H21 docs). Houdini GL is Houdini's own Hydra render delegate (HdGL-like) rather than the viewport path used for SOPs. The delegate source is not public [GAP]; documented capabilities: selection/handles/Houdini display options. Karma docs say Houdini GL/Storm are "light-weight" proxy-purpose viewers (glossary.md L642).
- GusdUSD/HUSD: HUSD is the in-Houdini USD library (HDK-exposed, above); GusdUSD ("GU" USD) is the open-source GU_Detail<->USD translation lib (ships in Houdini's USD plugin / earlier published on GitHub). [INF from general knowledge; not re-verified this session.]

## 3. Why two viewport paths exist [mostly INF]
- Historical: the SOP/GR viewport predates USD by ~two decades, is deeply tied to GU_Detail/GA data IDs, display flags, handles/selection/decorations, packed prims; cannot be replaced without regressing every SOP workflow. [INF]
- USD/Hydra is needed to share one scene description with Karma/husk and other delegates; Hydra only understands USD prims, so SOP geometry must be translated. [INF/DOC: usd.md L457 "Hydra directs a render delegate"]
- Costs [DOC where cited]: two backends x two APIs (GL3/GL4/VK GR classes + Hydra delegate); feature drift (SOP viewport has display options Hydra view lacks; GR hooks do not appear in LOPs; users must write both a GR hook and a USD schema/adapter for custom prims); SideFX's own tuning advice is to avoid Solaris translation while iterating in SOPs; HDK exposes a large surface for GR but Solaris viewport custom drawing is Hydra-plugin work.
- Note H21 added a Vulkan viewport and Karma-in-viewport, reducing but not removing the split [DOC: LD solaris/view.md L37].

## 4. Lessons for an editable native-geometry viewport [INF, ranked]
1. Houdini's *own* answer for interactive geometry editing is the native path (GR reuse + data-ID-driven partial updates), not Hydra. Even Houdini steers users away from USD re-translation during SOP editing.
2. Where Houdini does feed Hydra live, it (a) keeps a viewport-private stage, (b) pushes changes via SdfLayer::TransferContent so only diffs reach Hydra, (c) allows pausing viewport updates, (d) keeps viewer-only overrides in the session layer. Your Sdf-layer diff approach matches this; the flash is then likely from prim delete/re-create (path churn) rather than the diff concept. Houdini's diff cannot avoid recreating Hydra rprims when topology/prim type changes either [INF].
3. Time-varying or per-edit authoring costs: SideFX measured pain (32 s) from re-authoring + recomposing per change; mitigations are static topology, non-time-sampled values, shallow layer stacks, fewer sibling prims (consolidate meshes + GeomSubsets).
4. Persistent drawable objects with cheap update()/cheap render() and topology+attribute versions are what avoid flashing (no recreate-on-edit). Option (a) (native scene index into Hydra, preserving rprim identity and emitting HdDataSourceLocator dirtying for P only) replicates that inside Hydra [INF].
5. Heavy geometry: packed/instanced representation + distance-based bbox culling with a polygon budget, payload unloading; not cancellation magic (cancellation/double-buffering internals are undocumented).
6. Cost of two paths is real and acknowledged; adopting a second drawer (b) duplicates selection, picking, decorations, lights, shadows, MaterialX for each backend.

## Sources
- LD: solaris/performance.md, sop_import.md, jumpstart.md, glossary.md, usd.md, view.md, ref/windows/displayopts_3d.md, ref/env.md (HOUDINI_OGL_* debug vars)
- https://www.sidefx.com/docs/hdk/_h_d_k__viewport.html , _h_d_k__viewport_g_r.html , class_g_r___primitive.html , class_g_t___primitive.html , class_g_t___viewport_hook_collector.html
- https://www.sidefx.com/docs/hdk/_h_d_k__op_basics__lop_stages.html
- Forums: topic/92725, topic/101955, post/449357, post/393808, post/379562, post/402119
- https://dev.vulkan.org/user/pages/09.events/vulkanised-2024/vulkanised-2024-autodesk-pixar-adobe.pdf (Hgi/Storm background, not fetched in full)
- https://lucascheller.github.io/VFX-UsdSurvivalGuide/dcc/houdini/performance/overview.html (surfaced in search, not read)
Not found: SIGGRAPH/ASWF talk on the Houdini GL delegate internals; GA_DataId documentation; any doc on viewport double buffering or cook cancellation.
