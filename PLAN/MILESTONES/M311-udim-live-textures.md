# M311 - UDIM Live Textures

Extend M233 - Live Node Textures with a `<UDIM>` token: `natron://` texture URIs, `USDMaterial` image inputs and USDWrite export all accept it, so every UDIM tile of a node-driven texture shows live in the Viewer's 3D view and exports as a tile set. M53 - Projected Texture Painting depends on it (user, 2026-11-02 /cat-grill).

## Scope

**In:**
- `natron://<node>/<layer>.<UDIM>?v=<hash>` URIs: Storm substitutes each 4-digit tile and resolves it; a tile exists only if it is in the live-texture store.
- A source declares the tiles it covers with a `natron/udimTiles` metadata key; USDMaterial and USDEmissiveMaterial inputs whose source declares tiles author `<UDIM>`.
- Each tile renders under a render-context (RFV) variable naming the tile and is stored under a per-tile version, so an edit re-renders only the tiles it changes.
- The Viewer's 3D view (Storm) and USDRender show every tile; USDWrite writes a `<UDIM>` path plus one EXR per tile.
- Cycles and MoonRay show every tile (Phase 311.4, once F9 - Delegate Projection's M343 - Live Textures In Delegates is on main).

**Out:**
- USDProject3D (camera projection, not UV).
- UDIM file sequences on disk read by Read (Storm already handles them).
- M53 - Projected Texture Painting's TexturePaint itself; it only overrides the hooks added here.

**Edge cases:**
- Tiles above 1100: USD only enumerates 1001–1100, so they get a node warning.
- Tiles of mixed sizes in one set.
- One tile fails to render: only that tile falls back, with a warning naming it.
- A source with no `natron/udimTiles`: one texture, as today.
- A disabled source uses its pass-through input; a deleted source shows magenta (M343 rule).
- The tile set changes between frames.
- Sets larger than the store budget: no size cap.
- Storm loads a UDIM set as one texture array, so any edit gives the set a new URI and re-uploads every tile; only changed tiles are re-rendered.

**Assumptions:**
- Storm needs at least one tile in the store before it reads the stage; `prepare()` already runs before the Hydra sync (`Gui/SceneViewport.cpp` ~:4042, `USDRender.cpp` ~:638).
- `LiveTextureImage` scaling is enough for Storm's UDIM mip loads, and each tile is found by a plain resolve; P2.T1 confirms both.

## Phase 311.1: Tile context

- [x] M311.P1.T1 — Render a node under a UDIM tile context
  - files: `Engine/RenderContext.{h,cpp}`, `Engine/NodeColorSampler.{h,cpp}`, `Engine/ParallelRenderArgs.{h,cpp}`, `Engine/PyRFV.cpp`, `Tests/RenderContextUdim_Test.cpp`
  - approach: add a namespaced variable `RenderContextName::udimTile()`. Add an optional root `RenderContextPtr` to `renderNodeImageOverWindow` (today the request pass always starts from the empty context, `ParallelRenderArgs.cpp` ~:1249). Expose the variable to Python the way loop variables are (`PyRFV.cpp` ~:435). Native nodes declare the read via `getExtraContextReads()`.
  - verify: gtest — an expression reading the tile gives a different value per tile; an upstream node that doesn't read it renders once for two tiles (its restricted context collapses).
  - size: L

- [x] M311.P1.T2 — Tile coverage metadata and per-tile versions
  - files: `Engine/Nodes/Metadata/ImageMetadata.h`, `Engine/EffectInstance.{h,cpp}`, `Tests/UdimTileVersion_Test.cpp`
  - approach: `natron/udimTiles` is an int vector, and also accepts a string such as "1001-1004,1011" because `MetadataModify` only sets int/float/string. Add `virtual U64 getUdimTileHash(int tile, time, view)` whose default mixes the node's own hash with its inputs' tile versions; `udimTileVersion(node, tile)` walks upstream.
  - verify: gtest — a test effect overriding the hook: an edit to one tile changes only that tile's version, including through a Grade; a Grade edit changes every tile.
  - size: L

## Phase 311.2: Store, resolver, authoring

- [x] M311.P2.T1 — UDIM live-texture sources in the store and resolver
  - files: `Engine/Nodes/Scene/LiveTextureStore.{h,cpp}`, `Engine/Nodes/Scene/LiveTextureResolver.cpp`, `Tests/Scene/LiveTexture_Test.cpp`
  - approach: `LiveTextureSource` gets `tiles` and `getUri` a `<UDIM>` form. `prepare()` renders each tile under P1.T1's context over the format only (no overscan), skips any tile whose `udimTileVersion` is already stored and points that tile's new URI at the existing buffer. `_Resolve` succeeds for stored tile URIs.
  - verify: gtest — `UsdShadeUdimUtils::ResolveUdimTilePaths` on the set's URI returns exactly the declared tiles; after a one-tile edit the second `prepare()` renders one tile; `HioImage` opens every tile.
  - size: L

- [x] M311.P2.T2 — USDMaterial and USDEmissiveMaterial author `<UDIM>` inputs
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Engine/Nodes/Scene/USDMaterial.cpp`, `Engine/Nodes/Scene/USDEmissiveMaterial.cpp`, `Engine/Nodes/Scene/docs/USDMaterial.md`, `Engine/Nodes/Scene/docs/USDEmissiveMaterial.md`, `Tests/Scene/USDMaterial_Test.cpp`
  - approach: `getLiveTextureSource` reads the source's tiles. A UDIM input gets no `UsdTransform2d` (`connectLiveTextureCoordinates`), which would shift the UVs that pick the tile. The file-path check uses `ResolveUdimPath`. Tiles go into the layer hash.
  - verify: gtest — the asset path holds `<UDIM>` and there is no transform; an input without tiles is unchanged; changing the tile set changes the layer hash.
  - size: M

## Phase 311.3: Viewer and export

- [x] M311.P3.T1 — Every tile shows in Storm
  - files: `Gui/SceneViewport.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/USDRenderStorm_Test.cpp`, one reference crop (≤256 px, half-float), `Tests/gui/scene_live_texture_udim.py`
  - approach: a Constant whose colour expression reads the tile, then `MetadataModify` with "1001,1002", feeding a quad with UVs 0–2 in u. Editing tile 1002 re-renders only that tile (render count from a test property).
  - verify: the gtest golden shows two colours; the Xvfb script takes screenshots before and after the edit.
  - size: M

- [x] M311.P3.T2 — USDWrite exports tile sets
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (`planLiveTextureExports`, the URI rewrite), `Engine/Nodes/Scene/docs/USDWrite.md`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: write `<node>_<layer>.<tile>.exr` (or `.<tile>.<frame>.exr` when time-varying) and rewrite the set's URI to the matching `<UDIM>` path.
  - verify: gtest — the written `file` is the relative `<UDIM>` path, one EXR per tile, and `ResolveUdimTilePaths` on it finds them all; with Export Live Textures off, the URI and warning are as today.
  - size: M

## Phase 311.4: Delegates

Moved out of this milestone (user, 2026-11-13): this phase becomes its own follow-up milestone, outside F11 - Texture Painting, depending on F9 - Delegate Projection. Kept here as the brief for it.

- [ ] ~~M311.P4.T1 — UDIM live textures in Cycles and MoonRay~~ (moved to M426 - UDIM Textures In Delegates)
  - files: `Engine/Nodes/Scene/LiveTextureSceneIndex.cpp`, `Engine/Nodes/Scene/LiveTextureSharedMemory.cpp`, `Tests/Scene/LiveTexture_Test.cpp`, Cycles and MoonRay goldens
  - approach: the shared-memory path contains `<UDIM>`, one file per tile. hdCycles never fills `ImageTextureNode::tiles`, so the filter sets a `tiles` int array; `ImageManager::add_image` substitutes `%04d`. MoonRay globs `prefix*`, so the prefix must be unique to the set.
  - verify: both goldens show two tiles; a missing tile is magenta with a warning.
  - size: L

**Verification gate:** P1–P3 gtests and `Tests/gui/scene_live_texture_udim.py` pass in a remote `build.sh release --tests` alongside the existing ctest suite. Screenshots are collected for F11 - Texture Painting's UAT.

## Decisions

- 2026-11-12 — **Elaborated (consultant; PM engineering calls):** tiles are declared by source metadata (`natron/udimTiles`), not a per-input knob, since TexturePaint knows the tiles its meshes cover and the key passes through Grades. Each tile renders under an RFV tile variable. Storm re-uploads the whole set on any edit and only changed tiles re-render, which is accepted (a custom Storm texture object would cost far more). Tiles stay within USD's 1001–1100, with a warning beyond. The delegate phase stays in this milestone, blocked on F9; whether to split it out instead is asked under the board's Open questions.
- 2026-11-12 — Effect on M53 - Projected Texture Painting: in P5.T3 the Output Tile knob goes away. TexturePaint reads the tile from its render context (declared in `getExtraContextReads`), writes `natron/udimTiles` for the tiles its meshes cover, and overrides `getUdimTileHash` with the strokes that touch each tile.
- 2026-11-12 — P1.T1 implemented (dd24c3bd8): expressions read the tile as `udim.tile` (None outside a tile render, like `loop.t`); `computeRequestPass` takes an optional root context.
- 2026-11-12 — P1.T2 (ae7ad17e6), P2.T1 (6d71ab956) and P2.T2 (d918c7cd2) implemented. Set URIs are `natron://<node>/<layer>.<UDIM>?v=<hex>`, and the `?v=` query survives USD's UDIM substitution (probed on USD 0.26.8). Storm sizes a whole set from its lowest tile and resamples the rest by nearest neighbour, which is documented in the material docs. Unchanged tiles share their buffer under the new URI. `getLiveTextureSource` reads tiles only when the caller asks, so USDProject3D is unchanged.
- 2026-11-12 — P3.T1 (c5fb65d39) and P3.T2 (e4cea16b9) implemented. The Storm test uses USDEmissiveMaterial so colours are exact without lights; golden `Tests/fixtures/native-golden/USDRender/StormUdimTiles.exr` is recorded by the gate run. The viewer publishes per-tile render counts as `natronLiveTextureRenders`. Built-in nodes move every tile on any knob edit, so the one-tile-only re-render is covered at store level (LiveTexture_Test). USDWrite writes one EXR per declared tile, even above 1100, which the store already warns about. Phases 1–3 gate running with golden recording; Phase 311.4 waits on F9.
- 2026-11-13 — Gate run 43367466484: the USDWrite UDIM tests had a setup bug (diffuse input already occupied, so the tile source never connected), fixed in d3ce91ea4. The Storm UDIM test passed every check except the missing golden: `--record-goldens` has no Storm leg, so Storm goldens are recorded in the published image from the build tree. That recording is pending after the re-run gate.
- 2026-11-13 — Gate run 43419804120 on b25e9d1d2 green (both ctest legs, `scene_live_texture_udim.py`, `scene_live_texture.py`); its `StormUdimTiles.exr` golden is committed (0e31e0ca2). Phases 1–3 are done in code; Phase 311.4 waits on F9 - Delegate Projection or the split question.
- 2026-11-13 — Phase 311.4 split out (user): M311 ships phases 1–3 into F11 - Texture Painting; the Cycles/MoonRay phase becomes a new milestone depending on F9 - Delegate Projection, outside F11, so F11 does not wait on F9. The new row is for /cat-plan to add (open question).
- 2026-11-13 — PR #271 opened into F11 - Texture Painting; review round 1 by a Claude reviewer (Codex quota exhausted).
- 2026-11-13 — PR #271 review round 1 (Claude, Codex exhausted): 8 findings (3 medium: stale tile files in USDWrite, one tile set for every frame, per-prepare tile-version walks), all being fixed.
- 2026-11-13 — Review round 1 fixes (cd1718efc): all 8 findings addressed; USDWrite tile sets are now per frame for varying sources (the Scope edge case). Re-gating.
- 2026-11-13 — Gate green on the round-1 fixes (run 43443542792). Review round 2 (final): 6 of 8 fixed. 2 new medium: stale-tile deletion could remove another export's tiles (`scene.usda` and `scene.usdc` share a textures dir); whether a source is UDIM still depends on the playhead frame. Being fixed: recorded-file deletion, per-file textures dirs, a tile marker in file names.
- 2026-11-13 — Round 2 (final) fixes (b75565bb1); round 2 closed. Tile files are `<stem>.udim.<tile>[.<frame>].exr` in `<file name>_textures/`, and deletion is limited to files the output recorded writing. Final gate running.
- 2026-11-13 — Final gate on b75565bb1: both GUI scripts and all ctest green except `LiveTextureWarningsSurviveTheNextEvaluation` (the material warning didn't name failed tiles; it now does, a9f6dfb46). `SceneViewportStorm` timed out in one leg (load). ctest re-running.
- 2026-11-13 — PR #271 merged into F11 (752e41286); main merged into F11. Done.
