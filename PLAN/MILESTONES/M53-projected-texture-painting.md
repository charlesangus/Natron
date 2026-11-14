# M53 - Projected Texture Painting

Full title: Mari-style texture painting in the Viewer's 3D view

Redesigned in the 2026-11-02 grill (user: "painting in the 3d viewport, mari-style, not some kind of 2d to 3d bridge"). M53 delivers a `TexturePaint` image node: you paint directly on meshes in the Viewer's 3D view with RotoPaint's brushes. Strokes land in a screen-space paint buffer, and navigating (or a Bake key) projects the buffer into the node's UDIM textures with occlusion. The UV bake (`SceneRaster`) and the projection logic (`ProjectTexture`, now internal) from Phases 53.1–53.2 are the bake engine. The common workflow: project plates onto geo with M234 - Camera Projection Nodes, then clean up in 3D. Live textures are M233 - Live Node Textures; the multi-tile URI is the new UDIM Live Textures stub, which M53 depends on. Read `StageCache` below as M19's `SceneQueryPool`.

## Scope

**In:**
- `TexturePaint` image node: Scene input (meshes via Prim Pattern; all matches share one UDIM texture set), output a multi-layer UDIM texture image that feeds M233's `USDMaterial` image inputs.
- Mari paint buffer in the Viewer's 3D view: strokes are stored in normalized view coordinates with the view camera they were painted through; the buffer has its own Buffer Resolution knob, independent of window size.
- Bake on orbit/pan/zoom/tool switch, plus a Bake key; the bake is async and never blocks navigation (unbaked strokes stay drawn until the textures update, progress shown, new strokes queue behind).
- All RotoPaint brush types (Solid, Eraser, Clone, Reveal, Blur, Sharpen, Smear, Dodge, Burn) via the Roto stroke engine.
- Natron's layer/channel system, as in RotoPaint: each stroke targets a layer and channels. The layer→material-purpose mapping travels as stream metadata (M74 - Native Metadata Core); TexturePaint writes it and downstream nodes may read it.
- Strokes are the truth, with a pixel cache. Internal bake engine: **Bake Inputs** freezes the upstream look (bound material channels, including USDProject3D projections) into one set of UDIM files as the paint base; **Bake Output** flattens strokes so far into a second set, a cache that freezes those strokes and a hand-off.
- Tile Resolution knob (1K/2K/4K/8K, default 4K).
- Static/Animated texture mode: in Animated mode strokes have RotoPaint lifetimes, caches are per frame and the output is a texture sequence. Animated cameras are supported.
- Undo per stroke and per bake.
- `ProjectTexture` folds into internals and is no longer a user node.

**Out:**
- UDIM token on live-texture URIs and `USDMaterial`: new stub "UDIM Live Textures" (M53 depends on it).
- Paint layered over an existing material (mix per channel; UsdPreviewSurface/glslfx now, MaterialX later): new stub "USDLayerMaterial".
- Standalone bake node that writes UDIM files, sharing M53's bake engine, plus M234 - Camera Projection Nodes' USDWrite bake for meshes that aren't card-like: new stub "USDBakeTextures".
- Paint-through 2D inputs, Screen or Camera mode per input, including plates through animated cameras: new stub "Paint-Through Inputs".
- Direct texel (raycast splat) painting: new stub "Direct Texel Painting".
- Baking render-delegate renders (e.g. Cycles) onto UVs: new stub "Delegate UV Baking"; the bake engine keeps a pluggable backend for it.
- Deforming geometry: none (geometry is static).
- The 2D-paint `PaintThroughCamera` preset: none (cancelled, wrong shape).

**Edge cases:**
- Matched meshes whose UVs overlap: a node warning naming the clashing prims; use one TexturePaint per object.
- A base texture of a different size than Tile Resolution: resampled, with a warning.
- A stroke started while a bake runs: queued behind it.
- Undoing a Bake Output: those strokes become live again, and the cache is discarded and rebuilt from strokes.
- Animated camera or plate in Animated mode: baked per frame.

**Assumptions:**
- Unconfirmed: bake file paths are knobs with a `<UDIM>` token (and a frame token in Animated mode), defaulting to project-relative paths.
- Unconfirmed: a matched mesh without the UV primvar gives a node error naming the prim (as ProjectTexture does).
- Unconfirmed: the paint tool is active in the 3D view while a TexturePaint node's panel is open, with RotoPaint's toolbars.
- Unconfirmed: pen pressure behaves as in RotoPaint.
- Unconfirmed: Clone/Reveal sample a CPU unlit textured view of the base plus baked textures through the session camera (deterministic, no GL in ctest), not a readback of Storm's lit frame.
- Unconfirmed: until UDIM Live Textures lands, the output shows one tile chosen by an Output Tile knob (default 1001).
- A native node can host a RotoContext and get Roto serialization and toolbars; M53.P5.T1 and M53.P6.T2 check this.

## Phase 53.1: CPU raster substrate

- [x] M53.P1.T1 — `SceneRaster`: camera-space z-buffer and UV-space bake over a composed stage
  - files: `Engine/Nodes/Scene/SceneRaster.h`/`.cpp`, `Tests/SceneRaster_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: pure-CPU triangle rasterizer, no GL. Input: composed `UsdStage` (via `StageCache`), `UsdTimeCode`, prim set. Triangulate `UsdGeomMesh` faces with Hydra's `HdMeshUtil` (ships in the USD lib; handles n-gons and primvar index remapping). Mode A, camera: given a `UsdGeomCamera` path and a resolution, write depth, prim id and world position per pixel with a z-test. Mode B, UV bake: given one prim, its `st` primvar name and a resolution, rasterize triangles in UV space and write world position and world normal per texel, plus a coverage mask. Parallel over scanline chunks (reuse `NativeEffectBase::makeDeepScanlineChunks`' partition shape). Subdivision surfaces rasterize their control cage (documented limitation).
  - verify: unit tests on hand-authored `.usda` — a unit quad facing the camera fills the expected pixel rect with monotonic depth; a UV-mapped quad bakes a full 0–1 coverage mask whose corner texels carry the quad's corner world positions.
  - size: L

- [x] M53.P1.T2 — Primvar interpolation and UDIM tile selection in the UV bake
  - files: `Engine/Nodes/Scene/SceneRaster.cpp`, `Tests/SceneRaster_Test.cpp`
  - approach: handle `faceVarying`, `vertex` and `varying` `st` (indexed or not) through `HdMeshUtil`'s primvar remap; add a UDIM tile parameter so the bake covers tile `1001 + u + 10*v` — UV values are offset by the tile origin before rasterization. Missing `st` sets a node error naming the prim.
  - verify: unit test — the same quad with `faceVarying` indexed `st` bakes identically to the `vertex` case; a quad whose UVs lie in tile 1002 bakes empty at 1001 and full at 1002.
  - size: M

## Phase 53.2: Nodes

- [x] M53.P2.T1 — `ProjectTexture` node: scene + image → UV-space image
  - files: `Engine/Nodes/Scene/ProjectTexture.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/ProjectTexture_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene, Image (the thing to project; typically a `USDRender` + paint, or any image). Knobs: camera path, `PrimSelector` (first match is the bake target), `st` primvar name, UDIM tile, output resolution, occlusion on/off with a depth bias, edge dilation in texels. Render (output kind **image**, RoD = resolution): UV-bake the prim with `SceneRaster`; for each covered texel project its world position through the camera into the input image's pixel space and sample bilinearly; texels occluded per the camera z-buffer, off-screen, or back-facing get alpha 0; dilate the result by N texels so bilinear texture filtering doesn't bleed black. The input image is fetched for the camera's full frame at the project format. Result is a normal `Image` and caches normally.
  - verify: unit test — a constant red image projected onto a camera-facing UV quad yields an all-red, alpha-1 texture inside the coverage mask; a second quad in front of half of it leaves that half at alpha 0 with occlusion on and red with it off.
  - size: L

- [ ] ~~M53.P2.T2 — Live-texture registry and `Ar` resolver plugin~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Engine/Nodes/Scene/LiveTextureRegistry.h`/`.cpp`, `Engine/Nodes/Scene/LiveTextureResolver.h`/`.cpp` (`ArResolver` + `ArAsset`), `Engine/Nodes/Scene/plugInfo.json` + CMake to install it where `PlugRegistry` finds it, `Tests/LiveTexture_Test.cpp`
  - approach: URI scheme `natron://<nodeScriptName>/<plane>?v=<imageHash>`. The registry maps node script name → weak `NodePtr` plus the time the image was registered for (set by `USDMaterial` at layer-authoring time). The resolver answers `Resolve()` with the URI itself when the node exists and `OpenAsset()` with an in-memory `ArAsset` wrapping the node's rendered RGBA float image for that time. A URI whose node is gone resolves to nothing, so Hydra shows the missing-texture fallback rather than crashing. Registered via `plugInfo.json`; USD's plugin path must include the Natron binary's plugin dir in the installed and AppImage layouts.
  - verify: unit test — register a `Constant` node, `ArGetResolver().OpenAsset()` on its URI returns pixel bytes of the expected size; unregistering makes the resolve fail cleanly. Packaging (M15) still builds with the plugin dir.
  - size: L

- [ ] ~~M53.P2.T3 — `Hio` image plugin for live-texture assets~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Engine/Nodes/Scene/LiveTextureImage.h`/`.cpp` (`HioImage` subclass), `Engine/Nodes/Scene/plugInfo.json`, `Tests/LiveTexture_Test.cpp`
  - approach: an `HioImage` implementation for the `natron` scheme that reads the `ArAsset` from T2 as RGBA float with the correct size and orientation (Natron images are bottom-up; Hio expects top-down — flip once). Registered in the same `plugInfo.json`. This is what lets Storm's texture loader consume a node output without touching disk.
  - verify: unit test — `HioImage::OpenForReading()` on a registered `Constant` URI reads back matching dimensions and the constant's colour at the corners.
  - size: M

- [ ] ~~M53.P2.T4 — `USDMaterial` texture inputs bound as live textures~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Engine/Nodes/Scene/USDMaterial.cpp`, `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/` (USDMaterial and USDWrite tests)
  - approach: add optional **Image** inputs to `USDMaterial` for diffuseColor, roughness, metallic, normal and emissive alongside the existing file-path knobs (a connected input wins). For a connected input, author a `UsdUVTexture` shader whose `file` is the `natron://` URI with `?v=<input image hash>`, register the node with `LiveTextureRegistry`, and fold the input image hash into the node's layer hash so a changed texture yields a new layer, a new stage and a new texture path Storm reloads. `USDWrite` gains "export live textures": on export, each `natron://` reference is rendered to `<usd basename>_textures/<node>_<plane>.exr` next to the output and the exported layer's path is rewritten to it.
  - verify: unit test — a `Constant` → `USDMaterial` diffuse input composes a `UsdUVTexture` with a `natron://` asset path; `USDWrite` produces the `.exr` and a layer referencing the relative file path that `USDRead` composes cleanly.
  - size: M

- [ ] ~~M53.P2.T5 — The Viewer's 3D view refreshes when a live texture's source changes~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Gui/SceneViewport.cpp`, `Engine/Nodes/Scene/USDMaterial.cpp`
  - approach: the viewport already re-composes when the viewed node's stack hash changes (M19.P1.T3); confirm the `?v=<hash>` path change propagates through Storm's texture registry as a reload, and add an explicit `HdChangeTracker` material-dirty mark if a stale texture persists. Keep it simple: whole-texture reload per change is acceptable for v1 (design doc, Risks).
  - verify: manual — paint a stroke upstream of a live-textured material and see it appear on the model in the Viewer's 3D view without touching the viewport.
  - size: S

## Phase 53.3: Paint workflow

- [ ] ~~M53.P3.T1 — `PaintThroughCamera` preset group~~ — cancelled 2026-11-02: 2D-to-3D bridge, replaced by painting in the 3D view (Phases 53.4–53.6).
  - files: a shipped PyPlug/preset under the existing built-in presets location, `Gui/` only if the preset loader needs a menu entry, brief usage notes in `Engine/Nodes/Scene/README.md` (create if absent)
  - approach: a group node wiring `USDRender`(camera) → `RotoPaint` → `ProjectTexture`(same camera, same prim) → 2D `Merge` over an optional existing-texture input → output image, with the camera path and prim selector promoted to the group. Its output feeds `USDMaterial`'s diffuse input. Multi-angle painting is several of these merged in 2D; say so in the notes.
  - verify: manual on real hardware — paint a stroke in the 2D viewer on the rendered view; the stroke shows on the model in the Viewer's 3D view from a different angle, correctly occluded where another prim was in front.
  - size: M

- [ ] ~~M53.P3.T2 — Paint pipeline integration test in CI~~ — cancelled 2026-11-02: superseded by the TexturePaint gtests and M53.P6.T3.
  - files: `Tests/ProjectTexture_Test.cpp` (extend), reference `.usda` with a UV-mapped quad and an occluder
  - approach: headless graph — `USDRead` + `USDCamera` + `Constant` → `ProjectTexture` → assert coverage and occlusion; `ProjectTexture` → `USDMaterial`(live) → `USDWrite`(export textures) → assert the exported `.exr` matches the `ProjectTexture` output. No GL involved (`SceneRaster` is CPU).
  - verify: test green in `build-and-test`.
  - size: M

## Phase 53.4: Paint substrate

Before M53.P5.T4, merge the current tips of M233 - Live Node Textures, M52 - USD Layer-Stack Completion and M234 - Camera Projection Nodes into this branch: `PrimSelector`, `LiveTextureStore` and `USDProject3D` are not on main.

- [x] M53.P4.T1 — `SceneRaster`: render through an arbitrary view camera and output UV per pixel
  - files: `Engine/Nodes/Scene/SceneRaster.h`/`.cpp`, `Tests/Scene/SceneRaster_Test.cpp`
  - approach: `CameraArgs` only accepts `cameraPath` (SceneRaster.h:53). Add an optional explicit camera (camera-to-world as a Gf row-major matrix, view height, aspect, orthographic flag, clip range). It mirrors main's `SceneViewportRenderer::CameraPose` but lives in Engine, since Engine must not include Gui. Add an optional `st` + UDIM-tile buffer to `CameraBuffers` for the CPU unlit textured view.
  - verify: gtest — a camera given as a matrix gives the same depth/primIndex as the same `UsdGeomCamera` given by path; the UV buffer at a quad's corners matches its authored `st`.
  - size: M

- [x] M53.P4.T2 — Fold ProjectTexture into an internal `TextureProjection` helper and unregister the node
  - files: `Engine/Nodes/Scene/TextureProjection.h`/`.cpp` (new), delete `Engine/Nodes/Scene/ProjectTexture.{h,cpp,reg.inc}`, `Tests/Scene/ProjectTexture_Test.cpp` → `Tests/Scene/TextureProjection_Test.cpp`
  - approach: move the private `ProjectTexture::renderTexture` logic into a free function. It takes a float buffer + the M53.P4.T1 camera + one mesh + `st` name + UDIM tile + resolution, and returns RGBA + coverage, keeping occlusion, depth bias, back faces and edge dilation. Deleting the `.reg.inc` unregisters it (Engine/CMakeLists.txt globs `Nodes/*.reg.inc`).
  - verify: the existing red-quad and half-occluder cases, ported to the helper, give identical results; `ProjectTexture` is missing from the plug-in list.
  - size: L

- [x] M53.P4.T3 — Let the Roto stroke tree run on a node other than RotoPaint, with a host-supplied source
  - files: `Engine/RotoDrawableItem.cpp`, `Engine/RotoContext.h`/`.cpp`, `Engine/EffectInstance.h`/`.cpp`, `Tests/RotoLayer_Test.cpp`
  - approach: `refreshNodesConnections` hard-codes the host's input 0 as the bottom of the tree (RotoDrawableItem.cpp, `getContext()->getNode()->getInput(0)`). Add `virtual NodePtr EffectInstance::getPaintTreeSource()`, defaulting to input 0. Host nodes opt in through `getCurveContextKind()` returning `eCurveContextKindPaint` (EffectInstance.cpp checks the plug-in ID only), and `RotoContext`'s `isDefaultPaintNode` must accept them too.
  - verify: the Roto gtests pass unchanged; a new gtest has a test effect whose source is a `Constant`, and its Solid stroke merges over that constant, not over input 0.
  - size: M

## Phase 53.5: TexturePaint node

- [x] M53.P5.T1 — `TexturePaint` node: knobs, a hosted RotoContext, paint sessions, save/load
  - files: `Engine/Nodes/Scene/TexturePaint.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Scene/docs/TexturePaint.md`, `Tests/Scene/TexturePaint_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Scene input; `PrimSelector` Prim Pattern (all matched meshes share one UDIM set); UV Primvar; Tile Resolution 1K/2K/4K/8K (default 4K); Buffer Resolution; Static/Animated mode; Bake Inputs / Bake Output paths with a `<UDIM>` token. A *session* is the set of strokes painted through one view camera: a `RotoLayer` carrying the M53.P4.T1 camera plus a frozen flag, all serialized. Knob labels match script names. Check that hosting a RotoContext gets Roto serialization.
  - verify: gtest — create the node, add a session with two strokes, save the project and load it; strokes, camera matrices, frozen flags and knob values all round-trip.
  - size: L

- [x] M53.P5.T2 — Render a session's strokes into the paint buffer, all nine stroke types
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintView.h`/`.cpp` (new), `Tests/Scene/TexturePaint_Test.cpp`
  - approach: `TexturePaintView` is an internal image source: the CPU unlit textured view through the session camera at Buffer Resolution, M53.P4.T1's UV buffer sampled from the current base plus the baked textures. It is returned from `getPaintTreeSource()` (M53.P4.T3), so Clone/Reveal/Blur/Sharpen/Smear/Dodge/Burn sample the 3D view. Strokes are stored in normalized view coordinates and rasterized at Buffer Resolution; each targets a layer and channels as RotoPaint's do.
  - verify: gtest — a Solid stroke fills the expected buffer pixels; Eraser removes them; Clone copies view pixels by the offset; a stroke on `rgba` leaves another layer untouched; the buffer size does not change with the viewer size.
  - size: L

- [x] M53.P5.T3 — Bake sessions into UDIM tiles; multi-layer output with layer→material-purpose metadata; overlapping-UV warning
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintCache.h`/`.cpp` (new), `Tests/Scene/TexturePaint_Test.cpp`
  - approach: for each matched mesh and covered tile, `TextureProjection` (M53.P4.T2) projects each session buffer into a per-tile pixel cache, base under strokes. Lazy, per tile, half-float on disk (an 8K float RGBA tile is about 1 GB per layer). Until UDIM Live Textures lands, the output image is one tile chosen by an Output Tile knob (default 1001), as multiple planes via `getComponentsNeededAndProduced`. Override `deriveOutputMetadata` to write keys such as `natron/texturePaint/purpose/rgba = diffuseColor`. Meshes whose UVs overlap get a node warning naming both prims.
  - verify: gtest — stroke → buffer → tile 1001 shows paint only on the visible half (occluder case) and none on back faces; the metadata keys are present downstream of a `ViewMetadata`; two overlapping quads produce the warning text.
  - size: L

- [x] M53.P5.T4 — Bake Inputs: freeze the bound material's look into UDIM base files
  - files: `Engine/Nodes/Scene/MaterialBake.h`/`.cpp` (new), `Engine/Nodes/Scene/TexturePaint.cpp`, `Tests/Scene/TexturePaint_Test.cpp`
  - approach: evaluate on the CPU at each texel from `SceneRaster::bakeUV`: UsdPreviewSurface constants, `UsdUVTexture` files (OiioReadSupport), `natron://` (M233 `LiveTextureStore`), and USDProject3D's projection, which reimplements M234's `natronProject3D.glslfx` with `TextureProjection`. Keep the evaluator behind an interface so USDBakeTextures, MaterialX and delegate backends can plug in later. Write one half-float EXR per tile per purpose (OiioWriteSupport); a base of a different size is resampled to Tile Resolution with a warning.
  - verify: gtest — a material with constant red diffuse bakes red into `<UDIM>`=1001; a 1K file texture at 2K resamples and warns; a USDProject3D card bakes its plate; a deleted base file reloads as missing with an error.
  - size: L

- [x] M53.P5.T5 — Bake Output, freezing, and undo per stroke and per bake
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintUndo.h`/`.cpp` (new), `Engine/RotoUndoCommand.h` (reuse), `Tests/Scene/TexturePaint_Test.cpp`
  - approach: Bake Output writes base plus all sessions so far to the output files and marks those sessions frozen; frozen strokes are not re-rendered and come from the files. A stroke is undone through RotoPaint's own undo commands; a bake through a new command that unfreezes the sessions, discards the cache and rebuilds it from strokes. Strokes stay the truth.
  - verify: gtest — after Bake Output the files match the output; a new stroke leaves frozen sessions untouched; undoing the bake brings them back live with pixels identical to the pre-bake output; undoing a stroke restores the previous tile.
  - size: L

- [x] M53.P5.T6 — Async bake scheduler with queued strokes and progress
  - files: `Engine/Nodes/Scene/TexturePaintBaker.h`/`.cpp` (new), `Engine/Nodes/Scene/TexturePaint.cpp`, `Tests/Scene/TexturePaint_Test.cpp`
  - approach: subclass `GenericSchedulerThread`, as `TrackScheduler` does (TrackerContext.h). Bake requests queue, and new strokes queue behind a bake in progress; a rate-limited progress/finished signal bumps the node hash. Never take the viewer lock. Abort when the node is deleted.
  - verify: gtest — three queued session bakes finish in order; enqueue never blocks the main thread more than 50 ms; deleting the node mid-bake exits cleanly.
  - size: M

- [x] M53.P5.T7 — Animated mode: stroke lifetimes, per-frame caches, texture sequences
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintCache.cpp`, `Tests/Scene/TexturePaint_Test.cpp`
  - approach: use the RotoDrawableItem lifetime knobs; key the cache by frame; bake file paths take a frame token next to `<UDIM>`; the output reports itself as frame-varying, and an animated session camera bakes per frame. Geometry stays static.
  - verify: gtest — a stroke with lifetime 5–7 shows at frames 5–7 only; Bake Output writes one file per frame; Static mode ignores lifetimes.
  - size: M

## Phase 53.6: Viewer 3D paint

- [x] M53.P6.T4 — Generalise RotoPaint's brush knobs and stroke interact to any paint host
  - files: `Engine/RotoPaint.{h,cpp}`, `Engine/RotoPaintInteract.{h,cpp}`, `Engine/RotoPaintPrivate.h` (if separate), `Engine/Nodes/Scene/TexturePaint.cpp`, `Tests/RotoLayer_Test.cpp` or a new `Tests/RotoPaintHost_Test.cpp`
  - approach: move the paint toolbar and brush knob creation (tool buttons, colour, opacity, size, hardness, pressure toggles, build-up, source/clone settings, lifetimes) out of `RotoPaint::initializeKnobs()` into a shared helper any paint-kind host calls, and build the stroke interact from an `EffectInstance*` plus that knob set instead of `RotoPaintPrivate*`, exposing a stroke-building entry point (begin/continue/finish with points and pressure) a non-viewer caller can drive. RotoPaint behaves and serializes exactly as before. TexturePaint creates the same knobs.
  - verify: the Roto/RotoPaint gtests and `Tests/gui` RotoPaint scripts pass unchanged; a gtest drives the shared stroke builder on a TexturePaint session and gets a stroke with the toolbar's brush type, size and pressure.
  - size: L

- [x] M53.P6.T1 — 3D-view paint hook: pen events to TexturePaint; navigation and the Bake key trigger bakes
  - files: `Gui/SceneViewport.h`/`.cpp`, `Gui/ViewerTab.cpp`, `Engine/Nodes/Scene/TexturePaintInteract.h`/`.cpp` (new)
  - approach: add `setPaintNodeProvider`, modelled on `setTransformNodeProvider` (ViewerTab.cpp): a TexturePaint node with its panel open. `mousePressEvent`/`mouseMoveEvent` send normalized view coordinates plus the `CameraPose` to the interact, which drives `RotoPaintInteract`'s stroke building, with pen pressure. Orbit/pan/zoom/tool switch closes the session and queues a bake; a Bake key in `keyPressEvent` does the same.
  - verify: gtest — a test that drives the interact directly with synthetic poses gets one session per pose, with a camera change ending the session.
  - size: L

- [x] M53.P6.T2 — Draw the unbaked buffer, brush cursor, progress and paint toolbar in the 3D view
  - files: `Gui/SceneViewport.cpp`, `Gui/ViewerTab.cpp`, `Gui/ViewerTab20.cpp`
  - approach: in `paintGL`, composite the live buffer as a screen-aligned textured quad until its bake finishes, then draw a brush ring and a progress line. Show the RotoPaint tool and brush toolbars when the 3D view is active; the layout fits 1920x1080.
  - verify: under Xvfb at 1920x1080, with a TexturePaint panel open and an injected unbaked session, a screenshot shows the buffer quad, brush ring and paint toolbar; after the bake finishes the quad is gone.
  - size: M

- [x] M53.P6.T3 — Xvfb GUI script: paint in 3D, orbit, check the texture
  - files: `Tests/gui/m53_texture_paint.py`, a fixture `.usda` under `Tests/Scene/`
  - approach: run through `build.sh --gui`. Paint a Solid stroke on a UV quad, orbit, and wait for the `natronTexturePaintBaked` property. Check tile 1001's pixels and a screenshot from the new angle; undo a Bake Output and check the strokes are live again.
  - verify: the script passes remotely; screenshots shared for sign-off.
  - size: M

**Verification gate:** all M53 gtests (stroke → buffer → UDIM bake, occlusion, Bake Inputs/Output files and cache invalidation, Animated mode, save/load) and `Tests/gui/m53_texture_paint.py` pass remotely with screenshots shared before sign-off, alongside the pre-existing ctest suite; an AppImage UAT on real hardware cleans up a USDProject3D projection by painting in 3D.

## Decisions

- 2026-10-28: Phases 1–2 implemented (build pending). SceneRaster stores the nearest mesh's index into a returned list of mesh paths (exact, no hashed primId); depth is view-axis distance (+inf empty); bake normal = flat face normal; empty prim list = all meshes; invisible/guide meshes skipped; point instancers not drawn; "prim set" = meshes at or under the given paths. ProjectTexture maps onto the Image input's own format/PAR; adds Fit Mode (matches USDRender framing); bake target = first selected mesh; Depth Bias is a fraction of texel distance (0.01); Edge Dilation (4 texels) grows only outside the UV layout. M234's USDWrite bake to be wired to SceneRaster::bakeUV when the branches meet. Open (user): Camera input instead of a camera menu, like USDProject3D? Add Project On (front/back/both) and occlusion modes (self/world)?
- 2026-10-25 — **Long-lived USD scene (bosley-native-nodes, from the review in `PLAN/DESIGN/2026-10-25-usd-long-lived-scene-review.md`):** Drop "the viewport re-composes on stack hash": the viewer keeps one long-lived stage, and a `?v=` texture path change is a plain property/asset-path invalidation. Check that the `natron://` resolver never renders synchronously from inside Hydra on the GUI thread (deadlock risk).
- 2026-10-26 — **Live textures split out (user, M19/M20 UAT grill):** M53.P2.T2–T5 move to M233 - Live Node Textures, which ships first in the 3D MVP; `ProjectTexture` and the paint preset bind their output through M233's `USDMaterial` image inputs. Live camera projection is M234 - Camera Projection Nodes; this milestone's bake remains the occlusion-aware and export path.
- 2026-11-02 — Audit: P1.T1–T2 at 27eebf91f and P2.T1 at 030f17ce0 are committed and pushed; P3 is not started. The branch merged M233 before M233 was rewritten (it holds 4ad6edf36… where M233 now has 206267031…) and M52 before its last GUI-check commit, so it needs a re-merge of the current M233 and M52 tips. Worktree clean, no PR, 23 behind origin/main. The 2026-10-28 open questions to the user are unanswered (audit).
- 2026-11-02 — **Redesign: paint in the 3D view, Mari-style (user, /cat-grill; answers the 13:25 inbox change-request):** the 2D-paint-then-project shape (PaintThroughCamera preset, M53.P3.T1–T2) was wrong. M53 is a `TexturePaint` node painting directly in the Viewer's 3D view. Phases 53.1–53.2 stay as its bake engine.
- 2026-11-02 — Paint model: Mari paint buffer first; direct texel splat becomes its own later milestone (user: "both, buffer first").
- 2026-11-02 — Persistence: strokes are the truth plus a pixel cache (user). Bake trigger: auto on navigate/tool switch plus a Bake key (user). Brushes: all RotoPaint stroke types (user).
- 2026-11-02 — Layers: Natron's layer/channel system, as in RotoPaint (user). The layer→material-purpose mapping lives in stream metadata: a node that sets it writes it, and downstream nodes may read it (user; M74's native metadata carries arbitrary keys).
- 2026-11-02 — UDIM: extend live textures with a `<UDIM>` token in a new UDIM Live Textures milestone, which M53 depends on (user). Base and layering: paint overlays the existing material via a new USDLayerMaterial node, with Bake Down to UV textures as well (user: "both"). The common case is projecting plates with M234 and cleaning up in 3D; plan for MaterialX later (user).
- 2026-11-02 — Baking is internal to TexturePaint: Bake Inputs → one file set (frozen base), Bake Output → another (frozen-stroke cache that keeps painting fast, and a hand-off). This is a deliberate exception to one-node-one-job, because it is key to fluid painting (user). A standalone USDBakeTextures node, whose main job is writing files, shares the engine, which keeps a pluggable backend for future render-delegate UV bakes (user).
- 2026-11-02 — Paint-through 2D inputs, Screen or Camera mode per input, are a separate stub (user). One texture set per node; overlapping UVs warn (user). ProjectTexture folds into internals and is unregistered (user).
- 2026-11-02 — Split: M53 is TexturePaint core; new E9 stubs are UDIM Live Textures, USDLayerMaterial, USDBakeTextures (+ M234's non-card USDWrite bake), Paint-Through Inputs, Direct Texel Painting and Delegate UV Baking. UDIM Live Textures comes first, layering after (user).
- 2026-11-02 — Undo per stroke and per bake (user). Geometry is static. Textures can animate behind a Static/Animated switch with Roto-style lifetimes, in M53, and animated cameras are supported (user: "this is comp, animated image sequences are kind of the whole game"). Tile Resolution knob, default 4K; the buffer has its own resolution, independent of the screen (user). The bake is async and never blocks (user). Gate: ctest + Xvfb GUI script with screenshots + AppImage UAT (user).
- 2026-11-02 — The 2026-10-28 open items (Camera input vs menu, Project On, occlusion modes) are moot: TexturePaint paints through the view camera, and camera projection belongs to the Paint-Through Inputs stub.
- 2026-11-02 — Branch state at the redesign (audit): phases 53.1–53.2 committed and pushed on milestone/m53-projected-texture-painting (14 ahead of main); the branch merged an M233 - Live Node Textures that has since been rewritten and an older M52 - USD Layer-Stack Completion, so it needs both re-merged before Phase 53.4 (PM).
- 2026-11-02 — On hold until the in-flight milestones are through; no new work starts before then (user).
- 2026-11-12 — Hold lifted (user, 2026-11-12): resume the remaining tasks once a lane frees up; UAT and screenshots are collected at F11 - Texture Painting's UAT.
- 2026-11-12 — **Branch rebuilt on F11 (PM):** the old branch held pre-rewrite snapshots of M233 - Live Node Textures and M52 - USD Layer-Stack Completion, and merging F11 (now equal to main, which has M233, M52 and M234) gave 34 conflicts. M53's own commits (27eebf91f, 030f17ce0) were cherry-picked onto `feature/f11-texture-painting` as a5871ad1e and 025fd8606 and force-pushed with lease; the old tip is kept locally as `backup/m53-pre-rebase-20261112`. Phase 53.4's re-merge note is satisfied. Tests now live under `Tests/Scene/`.
- 2026-11-12 — P4.T1 (c5a5d73bf) and P4.T2 (747111bed) implemented. `ExplicitCamera::viewHeight` is `2*tan(fovY/2)` for perspective (world height for ortho), matching the viewer's `CameraPose`. `TextureProjection::project` takes RGBA only and a dilation pass count; its `coverage` is 1 where a texel is on the UV layout and sampled the image. `SceneRaster::cameraFrustum` is public for it. Tests run with the phase build.
- 2026-11-12 — P4.T3 implemented (2ae246c7d): `EffectInstance::getPaintTreeSource()` defaults to input 0 and is used by `refreshNodesConnections`, `refreshRotoPaintTree` and `RotoObjectSelect`; `getCurveContextKind()` was already virtual. A non-RotoPaint host must call `refreshRotoPaintTree()` itself when its source changes. Phase 53.4 build running.
- 2026-11-12 — P5.T1 implemented (457c85858). Layers had no way to carry custom data, so Roto layers gained host knobs, created through a new `EffectInstance::onCurveLayerCreated` hook and saved under a YAML `knobs` key. A session is a layer directly under the base layer, with knobs `cameraToWorld`, `viewHeight`, `aspect`, `orthographic`, `nearClip`, `farClip` and `frozen`. `bufferResolution` sets the buffer's longer side (default 2K, at least one buffer pixel per screen pixel at 1920x1080). The mode knob is `textureMode`. The paint-tree source is null until P5.T2. Default bake paths are made unique per node in P5.T2.
- 2026-11-12 — P5.T2 implemented (5a174edc9). `TexturePaintView` is a hidden internal node per session, since the stroke tree connects to nodes. Strokes are stored in a fixed paint space of 8192 units on the view's longer side (normalized, scaled; the buffer is that space at a mip level), not literal 0–1, because mip levels can't upscale. The base shown is the bound UsdPreviewSurface's constant diffuse, else grey 0.18. No path variable expands to the node name, so P5.T3 sets default bake paths at node creation.
- 2026-11-12 — P5.T3 (105937201) and P5.T4 (1af16f70f) implemented. The bake cache lives under the disk cache root, `TexturePaint/<project+node hash>/` (there's no per-project cache dir). It holds zip half-float EXR tiles keyed by strokes, sessions, scene knobs and input hash, with about 0.6 GB per 4K tile bake in flight. Purpose metadata: `natron/texturePaint/purpose/rgba = diffuseColor`, and other planes map to their layer ID. Default bake paths are set at node creation as `[Project]/textures/<script>_inputs|_output.<UDIM>.exr`. Bake Inputs has a `MaterialBake::Backend` interface with a UsdPreviewSurface backend: constants, UsdUVTexture with wrap modes, primvar reader and Transform2d, `natron://` textures, and USDProject3D projection into diffuseColor. A missing material texture uses the shader fallback with a warning; only a deleted baked file is an error. Known gaps for later tasks: Bake Inputs runs synchronously (P5.T6 makes bakes async); Static mode bakes the first-requested frame (P5.T7); each session costs two projections per mesh.
- 2026-11-13 — P5.T5 (e5611e97e), P5.T6 (c5ef4f005) and P5.T7 (b2ffd815c) implemented. Bakes run on a `GenericSchedulerThread` baker, and the buttons only enqueue. A Bake Output undo step is pushed when the bake lands; its redo restores the stashed files without re-baking. Animated mode uses a `####` frame token and bakes the whole project range. Static mode pins to the range's first frame, and lifetimes are ignored there via a non-saved `RotoCurveContext` flag. A stroke on a frozen session starts a new session with the same camera (P6.T1).
- 2026-11-13 — **P6.T4 added before P6.T1 (PM):** TexturePaint had no brush toolbar, and `RotoPaintInteract` is built from `RotoPaintPrivate*` with private stroke building. The Scope calls for every RotoPaint brush type through the Roto stroke engine, plus RotoPaint's toolbars in the 3D view. So the brush knobs and stroke interact are generalised to any paint host rather than duplicated. P6.T1's interact then drives the shared builder, converting brush size between canonical pixels and the 8192-unit paint space.
- 2026-11-13 — P6.T4 (c93b6f162), P6.T1 (77e193108), P6.T2 (8f7bdb903) and P6.T3 plus the async overlay refresh (98254b4e0) implemented; all tasks implemented, full gate running. Painting: a plain left drag paints while a TexturePaint panel is open and a brush tool is selected; navigation bindings are unchanged; Bake Paint is `B` in the 3D view (claimed only while painting). Brush size maps to paint space as size × 8192 / the view's longer side in device pixels. The overlay is drawn with QPainter (sRGB-encoded, touched pixels only), rendered on a latest-wins worker. The GUI script fixture lives in `Tests/fixtures/texture-paint-quad.usda`, beside `scene-reference.usda`.
- 2026-11-13 — Gate green: ctest 838/838 (run 43400759845) and both GUI scripts (`m53_texture_paint.py` 27/27, `m53_texture_paint_overlay.py` 14/14, run on 1e733adf2). Fixed on the way: `fixFilePath` doubled `[Project]` before the first save; a failing top-level render threw; Static-mode stroke bounds; the pending-bakes teardown race. The scripts view the scene with the TexturePaint panel open; painting doesn't need the Viewer to view TexturePaint. PR #269 opened into F11, review running.
- 2026-11-13 — Gate run 43431969579 on f99992c23 failed only `TexturePaintTest.StrokeOnColorLeavesAnotherPlaneUntouched`: after the review fix (88256c3e0), a plane the node doesn't produce (`renderRoI` Ok with no layers) counted as a failed render. `renderNodeImageIfAny` now reads it as transparent black unless aborted (17348cccb). Re-gating.
- 2026-11-13 — Gate green on 17348cccb (run 43443646421). Review round 2 (Claude): round 1 9 fixed / 7 partly; 16 new findings, three high (a failed undo swap then Redo deletes the user's pre-bake original; the worker can reach a torn-down Node after the non-blocking stop; an empty strokes render paints zeros over the texture). All being fixed. This is the last review round allowed; the fixes get the gate, not a third review.
- 2026-11-13 — Review round 2 fixes (7a8a2f75c), 8 new gtests. Round 2 closed on the PR. `reloadBakedFiles()` was deleted: a file edited outside Natron mid-session is picked up on the next bake, undo/redo or reload (accepted limitation, PM). Tile directories are held with a lock file; shared directories that are unheld and untouched for 30 days are pruned. Final gate running; it merges into F11 if green.
- 2026-11-13 — Review round 3 requested by the user ("continue reviewing M53 until it's clean"); rounds continue until clean, overriding the two-round limit for this milestone (user).
- 2026-11-13 — Review round 3: all 16 round-2 findings addressed (15 fixed, 1 partly), no high. Not clean: one medium (the undo identity check misses files rewritten in place, so a user's file can be deleted), one low-medium (a dropped completion rolls back on the main thread without the identity check), seven lows. All being fixed; round 4 follows.
- 2026-11-13 — Round 3 fixes (49d57c215), 6 new tests; round 3 closed. Round 4 review and the re-gate are running. A paste of a paste from a different group keeps the old group name in its suffix (accepted, PM).
- 2026-11-13 — Round 4: not clean. Two medium: detached rollbacks are unordered (two dropped bakes, or a new bake during a rollback, can lose the original), and can outlive AppManager at quit. One low-medium: a skipped target's original backup is deleted. Eight lows, mostly tests. Fix: one serial rollback worker joined at shutdown, and a skipped original kept as `<name>.pre-bake`. Round 5 follows.
- 2026-11-13 — Round 4 fixes (86ffae765): a serial process-wide rollback worker joined in `~AppManager`; `.pre-bake` originals; claim back-off. Round 5 review and the gate are running.
- 2026-11-13 — Round 5: not clean. Medium: baker threads are never joined at quit, so a bake cancelled at quit rolls back during static teardown, and a completion queued at exit never rolls back. Low-medium: cross-node bakes on one file can still roll back in the wrong order. PM call after four rounds of ordering and lifetime findings: replace the patches with one design that is correct by construction. Every Bake Output file mutation (backup and write, seal, undo/redo swap, rollback), for all nodes, goes through one process-wide serial file queue in submission order. A cross-node claim on an unsettled file is refused. Shutdown stops and joins every baker, then drains the queue. In progress; round 6 follows.
- 2026-11-13 — Round 5 fixes (ad7f2b06d): `TexturePaintFileQueue` replaces the rollback worker and its eligibility rules. A cross-node claim on an unsettled file is refused, naming the holder. Undo/redo is refused while a Bake Output of the node is pending (stricter; documented in `docs/TexturePaint.md`). `TexturePaintBaker::shutdownAll()` runs in `~AppManager`. Round 6 review and the gate are running.
- 2026-11-13 — Round 6: the reviewer confirmed the queue design holds. Not clean: one low-medium (a dropped undo/redo swap still moves the files) and five non-cosmetic lows (a refused undo still moves the stack; claims keyed by raw paths; test-state leaks; real-node tests; EXR writes serialised on the queue thread). Fixes keep the design: undo/redo queue behind a pending bake instead of being refused. Round 7 follows.
- 2026-11-13 — Round 6 fixes (b865e0bde): swaps owned by their completion; undo/redo queue behind pending bakes and re-check the record; canonical claim keys; tiles encode to temp files on the baker. The Bake Output undo step is still pushed when the bake lands, so Ctrl+Z right after a Bake Output click undoes the previous step (refused if the bake lands first). Round 7 review and the gate are running.
- 2026-11-13 — Round 7: all round-6 findings fixed; nothing above low. Six lows being fixed: a failed reverse swap now reconciles node state, crash-leftover bake temps are cleaned (host+pid names), each tile is written once, plus a reverse-path test and owner-name tidying. Round 8 should confirm clean.
- 2026-11-13 — **Review round 9: clean** (nothing above low; all round-8 findings fixed). Three remaining lows are being tidied (temp removal only on ENOENT, an empty token when the own start time is unknown, EXPECT in a test helper). Merges into F11 once the final gate is green.
- 2026-11-13 — Gate on c0d3ac549: four new tests failed, all test bugs, fixed in 95e9087bd (the drop tests didn't wait for a stopping baker to accept their marker; the queued-bakes test painted the next session before bake 1 rendered). They exposed a product bug: a queued Bake Output listed the sessions to freeze when queued but rendered the strokes present when it ran, so a session painted meanwhile was baked without being frozen and could show twice after an undo. PM decision: a bake freezes exactly what it rendered (sessions snapshotted on the main thread when the bake starts). Being fixed.
- 2026-11-13 — Bake-freeze fix (eb0b9bb85): a queued Bake Output takes its session snapshot on the main thread when it starts and renders exactly that. A session changed mid-bake fails the bake and restores its files. Work queued while the baker stops runs once it has stopped. Re-gating.
- 2026-11-13 — Gate on eb0b9bb85: two cross-node tests timed out. They waited without pumping, and a bake's start now needs the main loop; fixed in 0d81d3b3b. F11 merged in (12d6c4dd8, `NodeColorSampler` keeps both the render context and `emptyIsResult`). Re-gating.
- 2026-11-14 — PR CI (debug asserts) on 12d6c4dd8: TexturePaintTest and CacheEquivalenceSweepTest aborted on `RotoCurveContext::getLayers()`'s main-thread assert. `TexturePaint::getSessions()` walked the layer list from render and bake threads. A new MT-safe `RotoCurveContext::getBaseLayer()` (also used by `getContentHash`) fixes it (6a04a2bf3). The release gate's legacy leg also failed `USDRenderMoonRayTest` once; watching whether it recurs. Re-gating.
