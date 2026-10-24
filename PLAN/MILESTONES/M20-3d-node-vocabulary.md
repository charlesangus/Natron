# M20 - 3D Node Vocabulary

Full title: 3D node vocabulary and USDRender

"M-Scene-2" in the design doc: the Nuke-14-inspired, deliberately small v1 node
vocabulary over M19's `SceneOps`, plus the two remaining consumers —
`USDRender` (scene → Image planes) and `USDWrite`. Requires M19's gate.
The vocabulary is a cap, not a floor (design doc, "Scope gravity"); additions
happen in named milestones with the design doc amended first.

All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Scene/`, authoring
one anonymous layer each via `SceneOps`, memoized per M19.P1.T3.

## Scope

**In:**
- Create nodes `USDCamera`, `USDPrim`, `USDLight`; edit nodes `USDTransform` (with the Viewer 3D-view gizmo), `USDMerge`, `USDPrune`, `USDMaterial`; consumers `USDRender` and `USDWrite`.
- `USDRender` renders through Storm only, into `rgba`, `depth.Z` (camera-space distance) and `primId.id` (path-hash float plus a hash → path map in frame metadata). It has a format knob that defaults to the project format, a camera-path knob, and an aperture fit-mode knob.
- `USDRender` hands its pixels to the image graph through a GL→Vulkan ladder that mirrors M107 - GPU Viewer Hand-off's rungs in reverse: ZeroCopy → ZeroCopyHostSync → GL readback into a CPU Image.
- `USDRender` uses the GUI's shared 4.5 compatibility context in the GUI, and a dlopened EGL surfaceless 4.5 context when headless (NatronRenderer, ctest) on any driver, llvmpipe included.
- `USDWrite` is an output node triggered by Render, the render queue or `NatronRenderer -w`. It writes once per render. Layered mode flattens implicit layers, Solaris-style; flattened mode bakes everything.
- An automated `USDRender` test under the strict llvmpipe GL runner, a headless scene-vocabulary integration test in CI, and a manual AMD + NVIDIA check with screenshots.

**Out:**
- Render delegates other than Storm (Cycles, Embree, RenderMan…), plugInfo discovery, and progressive convergence — none (a future named milestone).
- Cryptomatte-style ranked ID mattes — none.
- Image inputs feeding textures into `USDMaterial`, and texture export from `USDWrite` — M53 - Projected Texture Painting.
- Cards, generators (cube, sphere…) and geometry import — M54 - SOPs-Style Geometry.
- `PrimSelector`/`SdfPathExpression` pattern knobs, schema-typed `USDPrim`, and `USDEditPrim`/`USDReference`/`USDVariant`/`USDSetAttribute` — M52 - USD Layer-Stack Completion.
- Per-node layer save paths in `USDWrite` (a Configure Layer LOP equivalent) — M52 - USD Layer-Stack Completion.
- A group pivot for `USDTransform` that turns all matches about one shared centroid — none.
- Authoring `UsdGeomXformCommonAPI` pivots — none (Solaris doesn't either).
- Honouring USD `colorSpace` metadata, or any implicit colour transform — none (M50's explicit-only rule).
- MaterialX — none.
- Motion blur in `USDRender` — none.
- `USDMerge` duplicate/flatten modes — none (a future amendment).

**Edge cases:**
- GL→Vulkan zero-copy unavailable (UUID mismatch such as llvmpipe+RADV, `NATRON_GPU=off`, no Vulkan device on a farm node, device loss): `USDRender` silently drops down the ladder to readback into a CPU Image, and the output is the same on every rung.
- Headless with no GL 4.5 from EGL: `USDRender` fails its render with a clear error naming the version it got; 2D-only pipelines are unaffected.
- Camera path doesn't resolve on the composed stage (renamed or pruned upstream): node error naming the path; never a silent fall back to another camera. The dropdown lists every `UsdGeomCamera` on the stage, and the knob defaults to the first one found.
- Camera aperture aspect ≠ output format aspect: the fit-mode knob (fill / horizontal / vertical, USD's `CameraUtilConformWindow`) conforms the aperture to the format.
- Rays that hit nothing: `depth.Z` = +inf, and `primId.id` = 0 with no metadata entry.
- `USDTransform` matches several prims: each gets the same appended `xformOp:transform:<nodename>` and turns about its own local-space pivot knob. The gizmo sits at the first match's local origin, and a "centre pivot" button sets the pivot knob from the matches' bbox. Zero matches: node warning, scene passed through unchanged.
- `USDTransform` over a prim that already has an xformOp stack: Append mode (the default) keeps the stack and its animation underneath. Replace mode writes `!resetXformStack!` plus the node's op.
- The same upstream layer reaches `USDMerge` more than once (shared ancestor, diamond graphs): each layer handle stays only once, at its strongest position.
- `USDMaterial` or a dome `USDLight` texture path that doesn't resolve: the path is authored anyway (it may resolve on the farm or through an `ArResolver`), a persistent node warning names it, and Storm shows its fallback.
- `USDWrite` output exists: overwritten, as Write does. Its frame-range knob (default: project range) clips the exported time samples.

**Assumptions:**
- Storm's AOV render buffers can be exported to a Vulkan device through `GL_EXT_memory_object_fd`/semaphores on the drivers where M107 - GPU Viewer Hand-off reached zero-copy in the other direction.
- Storm renders correctly in an EGL surfaceless 4.5 context with no window system, including on llvmpipe.
- Storm produces a `primId` AOV that can be mapped back to prim paths from the render index, so the path hash can be computed per pixel.
- Unconfirmed: Storm antialiasing uses a fixed MSAA sample count, with no samples knob in v1.
- Unconfirmed: `USDWrite` rewrites external asset paths relative to the output file (the Solaris "Use Relative Paths" default), with no option for absolute paths.

## Phase 20.1: Create nodes

- [ ] M20.P1.T1 — `USDCamera` and `USDPrim`
  - files: `Engine/Nodes/Scene/USDCamera.cpp`, `Engine/Nodes/Scene/USDPrim.cpp`, CMake source list
  - approach: `USDCamera` authors a `UsdGeomCamera` prim (focal length, aperture, clipping, transform — animated knobs as time samples per M19's time policy). Defaults 50 mm focal, 36×24 mm aperture, clipping 0.1–10000, written in USD's camera units (tenths of a scene unit), and not linked to the project format. `USDPrim` authors empty xforms / simple prims for layout scaffolding. Both take an optional scene input (append to stack) or act as scene sources. Colour-free.
  - verify: unit tests — composed stage contains the camera with knob-matching attributes (aperture/focal in USD units); 3D-view look-through of a `USDCamera` matches its knob values.
  - size: M

- [ ] M20.P1.T2 — `USDLight`
  - files: `Engine/Nodes/Scene/USDLight.cpp`, CMake source list
  - approach: one node, type knob selecting the UsdLux prim (distant, sphere, rect, dome; dome takes a texture path). Intensity/exposure/color knobs map to UsdLux attributes directly — no native light model. Colour values are in the project working space and are written unchanged. A dome texture path that doesn't resolve is still authored, with a persistent node warning naming it.
  - verify: unit test — each type authors the right UsdLux prim; a missing dome texture authors the path and sets the warning; a lit sphere renders visibly differently per light type in `USDRender` (M20.P3.T1) once it lands.
  - size: M

## Phase 20.2: Edit nodes

- [ ] M20.P2.T1 — `USDTransform` with viewport gizmo
  - files: `Engine/Nodes/Scene/USDTransform.cpp`, `Gui/SceneViewport.cpp` (gizmo drawing + drag → knob writeback), CMake source list
  - approach: Solaris Transform LOP semantics on a prim pattern (path or wildcard match). Each matched prim gets the same `xformOp:transform:<nodename>` (numbered if the name is taken), authored as the strongest opinion in the node's layer. Mode knob: Append (default; added after the existing ops) or Replace (`!resetXformStack!` plus the node's op). Pivot knob in local space; nothing is authored to `XformCommonAPI` pivots. Zero matches: node warning, pass-through. Viewport gizmo: translate/rotate/scale handles drawn in the Viewer's 3D view when a `USDTransform` is selected, placed at the first match's local origin plus the pivot. A "centre pivot" button sets the pivot from the matches' bbox centre. Drags write back to knobs (undoable through the existing knob undo stack).
  - verify: unit tests — Append composes over the input's animated xformOps; Replace discards them; a two-prim wildcard authors the op on both; zero matches warns and leaves the stage identical. Manual — a gizmo drag moves the prim and creates one undo entry; centre pivot rotates about the bbox centre.
  - size: L

- [ ] M20.P2.T2 — `USDMerge` and `USDPrune`
  - files: `Engine/Nodes/Scene/USDMerge.cpp`, `Engine/Nodes/Scene/USDPrune.cpp`, CMake source list
  - approach: `USDMerge` — layer-stack union (Nuke GeoMerge "merge layers" mode; duplicate/flatten modes deferred to a future amendment), multi-input. Later inputs are stronger, as in the Solaris Merge LOP, so input N+1's layers stack above input N's in the strongest-last `layerStack`. A layer handle that arrives more than once is kept only at its strongest position. `USDPrune` — deactivation/visibility opinions by prim pattern.
  - verify: unit tests — the last input wins a conflicting attribute; a diamond graph (one source through two branches into one merge) yields that source's layer once; pruned prims are inactive on the composed stage.
  - size: M

- [ ] M20.P2.T3 — `USDMaterial`
  - files: `Engine/Nodes/Scene/USDMaterial.cpp`, CMake source list
  - approach: authors a `UsdPreviewSurface` material (diffuse/metallic/roughness/emissive knobs; a texture file path knob for each) and binds it to a prim pattern. Colour knobs are in the project working space and written unchanged. A texture path that doesn't resolve is still authored, with a persistent node warning naming it. Image inputs are M53's; MaterialX is out of scope for v1.
  - verify: unit test — material prim + binding present on composed stage; a missing texture authors the path and sets the warning. Manual — the shaded 3D view shows the material.
  - size: M

## Phase 20.3: Consumers

- [ ] M20.P3.T1 — `USDRender` node: scene + camera → Image (readback path)
  - files: `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp` (AOV/engine plumbing), `Engine/LayerRegistry.cpp` (the `primId` layer), CMake source list
  - approach: scene input + camera-path knob (dropdown of every `UsdGeomCamera` on the composed stage, defaulting to the first; a path that doesn't resolve is a node error naming it) → renders through Storm (the only delegate in v1, no delegate knob) into Natron Image layers: `rgba` (treated as the project working space, no transform), `depth.Z` (linear camera-space distance in scene units, +inf on misses), `primId.id` (32-bit hash of the prim path, bit-cast to float the way Cryptomatte does; 0 on misses; hash → path map in frame metadata). Format knob defaults to the project format; a fit-mode knob (fill / horizontal / vertical) conforms the aperture with `CameraUtilConformWindow`. **Output kind: image**, so its result flows through the normal `renderRoI` path, image cache included. In the GUI it renders on a context shared with the global 4.5 compatibility context (M19.P2.T8). This task delivers the readback rung (GL read into a CPU Image); M20.P3.T5 adds the headless context and M20.P3.T4 the zero-copy rungs.
  - verify: in the GUI, the reference scene renders, and `depth.Z` grows with distance; a prim's `primId.id` value maps to its path through the metadata; a bogus camera path errors with the path; a 2:1 format with a 3:2 aperture conforms per fit mode.
  - size: L

- [ ] M20.P3.T5 — Headless `USDRender` on an EGL surfaceless context
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/HeadlessGLContext.h`/`.cpp` (new), CMake source list
  - approach: when no GUI context exists (NatronRenderer, ctest), dlopen EGL and create a surfaceless (`EGL_MESA_platform_surfaceless` / device platform) 4.5 compatibility context on whatever driver the host has, llvmpipe included, with no X server. Below 4.5, or with no EGL, the render fails with a clear error naming what it got; 2D-only pipelines are unaffected. One context per render thread, or one serialized context — whichever Storm requires; record the choice in `## Decisions`.
  - verify: NatronRenderer renders a `USDRender` comp to PNG with `DISPLAY` unset, inside natron-dev (llvmpipe); with EGL hidden (`LD_LIBRARY_PATH` pointing at an empty stub), the node fails with the error and a 2D Write in the same project still renders.
  - size: M

- [ ] M20.P3.T4 — GL→Vulkan hand-off ladder for `USDRender`
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/HydraHandoff.h`/`.cpp` (new), `Engine/ViewerHandoff.*` (M107's — reuse its interop helpers, factoring shared code out rather than duplicating it), CMake source list
  - approach: mirror M107 - GPU Viewer Hand-off's ladder in the reverse direction. When the render's placement is GPU: ZeroCopy (export Storm's AOV textures with `GL_EXT_memory_object_fd` and semaphores, import into the Vulkan device as a `GpuDeviceImage`) → ZeroCopyHostSync → readback (M20.P3.T1's CPU path, uploaded by the existing placement). Pick the rung per device pair by UUID match and extension support, as M107 does; drop rungs silently on failure; on device loss drop the interop and use readback until `recoverIfLost`. The output is identical on every rung.
  - verify: unit test — forcing each rung (env or test hook) yields byte-identical AOVs; `NATRON_GPU=off` and an injected device loss fall back to readback with no error dialog or log spam. Manual on AMD and NVIDIA — the report shows the zero-copy rung reached and no root download for a `USDRender → GPU node → Viewer` chain.
  - size: L

- [ ] M20.P3.T2 — `USDWrite`
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, CMake source list
  - approach: an output node like Write. The Render button, the render queue and `NatronRenderer -w` trigger it, and it writes one `.usd`/`.usda`/`.usdc` file once per render whatever the frame range. A frame-range knob (default: project range) clips the exported time samples. Layered mode follows the Solaris USD ROP's "Flatten Implicit Layers": every Natron-authored anonymous layer is flattened into the output file, and external file layers (USDRead's) stay sublayers/references with asset paths rewritten relative to the output. Flattened mode bakes the whole stack, externals included, into one layer. An existing output is overwritten.
  - verify: round-trip test — a node-built scene → USDWrite → USDRead composes an identical prim tree (diff via `UsdStage::ExportToString`), in both modes; layered output references the original external file through a relative path; the frame-range knob clips the time samples; `NatronRenderer -w` writes the file once for a 10-frame range.
  - size: M

- [ ] M20.P3.T3 — Scene vocabulary integration test in CI
  - files: `tests/`, reference `.usda` assets
  - approach: headless graph — USDRead → USDTransform → USDMerge(+ USDPrim/USDLight/USDMaterial branch) → USDPrune → USDWrite — asserting on the exported composition (prims, xforms, bindings, deactivations). USDRender is covered by M20.P3.T6, plus the hardware check at the gate.
  - verify: test green in `build-and-test`; each node's authored opinions visible in the exported layer stack.
  - size: M

- [ ] M20.P3.T6 — Automated `USDRender` test under the strict GL runner
  - files: `Tests/` (GL test beside M19.P2.T7's Storm case), `build/m63-gui/run-gl-tests.sh` if it needs the case listed, a committed reference scene and reference image
  - approach: under the strict llvmpipe runner (headless EGL, M20.P3.T5), render the reference scene built from create/edit nodes through `USDRender` and compare `rgba` to the committed reference with a tolerance (not bit-exact); check that `depth.Z` grows with distance and is +inf on background, and that `primId.id` at known pixels decodes to the expected prim paths. Skips without GL like its siblings.
  - verify: passes under `build/m63-gui/run-gl-tests.sh` with `NATRON_TEST_REQUIRE_GL=1`; moving a prim in the reference scene makes it fail.
  - size: M

**Verification gate:** CI green including M20.P3.T3; M20.P3.T6 passes under the strict llvmpipe GL runner; `NatronRenderer` renders a `USDRender` comp headless with no display; on real AMD and NVIDIA hardware a scene built purely from create/edit nodes renders through `USDRender` to an image matching the committed reference, M20.P3.T4's report shows the zero-copy rung reached, and screenshots are shared before sign-off; the same scene round-trips through `USDWrite`/`USDRead` in both modes; gizmo manipulation works with undo; pre-existing ctest suite green.

## Decisions

- 2026-10-24 — `USDRender` hands pixels to the image graph through GL→Vulkan interop, not a plain CPU readback: user's call, since M19 - USD/Hydra Foundation left Hydra → Vulkan compute to `USDRender`.
- 2026-10-24 — The interop mirrors M107 - GPU Viewer Hand-off's ladder (ZeroCopy → ZeroCopyHostSync → readback), with a silent fallback: CI, llvmpipe+RADV and GPU-less farm nodes still render, and the output is the same on every rung.
- 2026-10-24 — Headless `USDRender` uses a dlopened EGL surfaceless context: it needs no X server, works on any driver including llvmpipe, and the shipped Qt has no xcb EGL integration for `QOffscreenSurface`.
- 2026-10-24 — Output resolution comes from a format knob defaulting to the project format, with an aperture fit-mode knob: the usual Natron pattern (Read/Constant), with no mixed-kind bg input.
- 2026-10-24 — `depth.Z` is linear camera-space distance, +inf on misses: EXR/deep convention, so M21 - Deep Tier-2 And Bridges' `DeepFromImage` needs no inversion. The registry's existing `depth` layer is reused.
- 2026-10-24 — `primId.id` is a path-hash float plus a hash → path metadata map: IDs stay stable across upstream edits and a picker can name the prim. Full Cryptomatte was rejected as a big lift that Storm doesn't support natively.
- 2026-10-24 — Storm is the only delegate in v1: user's call; other delegates, discovery and progressive convergence go to a future named milestone.
- 2026-10-24 — The camera is a path knob with a stage dropdown, and a path that doesn't resolve is an error: a silent fallback would hide broken comps.
- 2026-10-24 — `USDRender` is proven by a strict llvmpipe GL test plus a manual AMD/NVIDIA check: M19 - USD/Hydra Foundation showed Storm runs under the strict runner, so the old "no GL under Xvfb" exclusion was stale; hardware proves the zero-copy rung.
- 2026-10-24 — `USDTransform` follows the Solaris Transform LOP: the same `xformOp:transform:<nodename>` on every match, a local pivot, no `XformCommonAPI` pivots, and Append by default with a Replace mode. The gizmo sits at the first match's origin, with a centre-pivot button. Asked to follow Solaris by the user.
- 2026-10-24 — In `USDMerge`, later inputs are stronger: matches the Solaris Merge LOP and the strongest-last `layerStack`.
- 2026-10-24 — `USDMerge` keeps each layer handle once, at its strongest position: the composition is the same, and diamond graphs no longer grow the stack exponentially.
- 2026-10-24 — `USDWrite` is a render-triggered output that writes once, with a frame-range knob that clips samples: it fits Write, the render queue and `NatronRenderer -w` without wasting the time-sample design.
- 2026-10-24 — `USDWrite` layered mode flattens implicit layers, Solaris-style: anonymous Natron layers go into the output, external layers stay referenced with relative paths. Per-node save paths are M52's. Asked to follow Solaris by the user.
- 2026-10-24 — Colours are in the project working space throughout, with no implicit transform and USD `colorSpace` metadata ignored in v1: M50's explicit-only rule.
- 2026-10-24 — A texture path that doesn't resolve is authored anyway, with a node warning: assets may resolve on the farm or through a pipeline `ArResolver`.
- 2026-10-24 — `USDCamera` defaults to 50 mm and a 36×24 mm aperture, not linked to the format: `USDRender`'s fit mode handles aspect, as it does for cameras from files.
- 2026-10-24 — `Deps` is M19: the milestone requires M19 - USD/Hydra Foundation's gate, and `USDRender` reuses M107 - GPU Viewer Hand-off's ladder, which M19 already depends on.
- 2026-10-24 — The old M20.P3.T1 was split into the readback render (P3.T1), the headless EGL context (P3.T5), the interop ladder (P3.T4) and the strict GL test (P3.T6); `USDWrite` was re-sized S → M: per the task sizing rule, each is one coherent change, and the export logic plus its tests is not haiku-safe.
