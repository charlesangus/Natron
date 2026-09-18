# Milestone 53: Projected texture painting — UV bake, ProjectTexture, live textures

Stage 2 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 2, "Texture painting"). Mari-style projection painting built from
Natron's existing 2D tools: a CPU rasterizer that bakes a prim's UV space
to world positions, a `ProjectTexture` node that projects any image
through a camera into that UV space with occlusion, and an in-process
`Ar`/`Hio` plugin so `Material3D` can bind a node's output as a live
texture in Storm. Requires M52 (`PrimSelector`) and M20 (`HydraRender`,
`Material3D`, `Viewport3D`, `WriteScene`).

## Phase 53.1: CPU raster substrate

- [ ] M53.P1.T1 — `SceneRaster`: camera-space z-buffer and UV-space bake over a composed stage
  - files: `Engine/Nodes/Scene/SceneRaster.h`/`.cpp`, `Tests/SceneRaster_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: pure-CPU triangle rasterizer, no GL. Input: composed `UsdStage` (via `StageCache`), `UsdTimeCode`, prim set. Triangulate `UsdGeomMesh` faces with Hydra's `HdMeshUtil` (ships in the USD lib; handles n-gons and primvar index remapping). Mode A, camera: given a `UsdGeomCamera` path and a resolution, write depth, prim id and world position per pixel with a z-test. Mode B, UV bake: given one prim, its `st` primvar name and a resolution, rasterize triangles in UV space and write world position and world normal per texel, plus a coverage mask. Parallel over scanline chunks (reuse `NativeEffectBase::makeDeepScanlineChunks`' partition shape). Subdivision surfaces rasterize their control cage (documented limitation).
  - verify: unit tests on hand-authored `.usda` — a unit quad facing the camera fills the expected pixel rect with monotonic depth; a UV-mapped quad bakes a full 0–1 coverage mask whose corner texels carry the quad's corner world positions.
  - size: L

- [ ] M53.P1.T2 — Primvar interpolation and UDIM tile selection in the UV bake
  - files: `Engine/Nodes/Scene/SceneRaster.cpp`, `Tests/SceneRaster_Test.cpp`
  - approach: handle `faceVarying`, `vertex` and `varying` `st` (indexed or not) through `HdMeshUtil`'s primvar remap; add a UDIM tile parameter so the bake covers tile `1001 + u + 10*v` — UV values are offset by the tile origin before rasterization. Missing `st` sets a node error naming the prim.
  - verify: unit test — the same quad with `faceVarying` indexed `st` bakes identically to the `vertex` case; a quad whose UVs lie in tile 1002 bakes empty at 1001 and full at 1002.
  - size: M

## Phase 53.2: Nodes

- [ ] M53.P2.T1 — `ProjectTexture` node: scene + image → UV-space image
  - files: `Engine/Nodes/Scene/ProjectTexture.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/ProjectTexture_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene, Image (the thing to project; typically a `HydraRender` + paint, or any image). Knobs: camera path, `PrimSelector` (first match is the bake target), `st` primvar name, UDIM tile, output resolution, occlusion on/off with a depth bias, edge dilation in texels. Render (output kind **image**, RoD = resolution): UV-bake the prim with `SceneRaster`; for each covered texel project its world position through the camera into the input image's pixel space and sample bilinearly; texels occluded per the camera z-buffer, off-screen, or back-facing get alpha 0; dilate the result by N texels so bilinear texture filtering doesn't bleed black. The input image is fetched for the camera's full frame at the project format. Result is a normal `Image` and caches normally.
  - verify: unit test — a constant red image projected onto a camera-facing UV quad yields an all-red, alpha-1 texture inside the coverage mask; a second quad in front of half of it leaves that half at alpha 0 with occlusion on and red with it off.
  - size: L

- [ ] M53.P2.T2 — Live-texture registry and `Ar` resolver plugin
  - files: `Engine/Nodes/Scene/LiveTextureRegistry.h`/`.cpp`, `Engine/Nodes/Scene/LiveTextureResolver.h`/`.cpp` (`ArResolver` + `ArAsset`), `Engine/Nodes/Scene/plugInfo.json` + CMake to install it where `PlugRegistry` finds it, `Tests/LiveTexture_Test.cpp`
  - approach: URI scheme `natron://<nodeScriptName>/<plane>?v=<imageHash>`. The registry maps node script name → weak `NodePtr` plus the time the image was registered for (set by `Material3D` at layer-authoring time). The resolver answers `Resolve()` with the URI itself when the node exists and `OpenAsset()` with an in-memory `ArAsset` wrapping the node's rendered RGBA float image for that time. A URI whose node is gone resolves to nothing, so Hydra shows the missing-texture fallback rather than crashing. Registered via `plugInfo.json`; USD's plugin path must include the Natron binary's plugin dir in the installed and AppImage layouts.
  - verify: unit test — register a `Constant` node, `ArGetResolver().OpenAsset()` on its URI returns pixel bytes of the expected size; unregistering makes the resolve fail cleanly. Packaging (M15) still builds with the plugin dir.
  - size: L

- [ ] M53.P2.T3 — `Hio` image plugin for live-texture assets
  - files: `Engine/Nodes/Scene/LiveTextureImage.h`/`.cpp` (`HioImage` subclass), `Engine/Nodes/Scene/plugInfo.json`, `Tests/LiveTexture_Test.cpp`
  - approach: an `HioImage` implementation for the `natron` scheme that reads the `ArAsset` from T2 as RGBA float with the correct size and orientation (Natron images are bottom-up; Hio expects top-down — flip once). Registered in the same `plugInfo.json`. This is what lets Storm's texture loader consume a node output without touching disk.
  - verify: unit test — `HioImage::OpenForReading()` on a registered `Constant` URI reads back matching dimensions and the constant's colour at the corners.
  - size: M

- [ ] M53.P2.T4 — `Material3D` texture inputs bound as live textures
  - files: `Engine/Nodes/Scene/Material3D.cpp`, `Engine/Nodes/Scene/WriteScene.cpp`, `Tests/` (Material3D and WriteScene tests)
  - approach: add optional **Image** inputs to `Material3D` for diffuseColor, roughness, metallic, normal and emissive alongside the existing file-path knobs (a connected input wins). For a connected input, author a `UsdUVTexture` shader whose `file` is the `natron://` URI with `?v=<input image hash>`, register the node with `LiveTextureRegistry`, and fold the input image hash into the node's layer hash so a changed texture yields a new layer, a new stage and a new texture path Storm reloads. `WriteScene` gains "export live textures": on export, each `natron://` reference is rendered to `<usd basename>_textures/<node>_<plane>.exr` next to the output and the exported layer's path is rewritten to it.
  - verify: unit test — a `Constant` → `Material3D` diffuse input composes a `UsdUVTexture` with a `natron://` asset path; `WriteScene` produces the `.exr` and a layer referencing the relative file path that `ReadScene` composes cleanly.
  - size: M

- [ ] M53.P2.T5 — `Viewport3D` refreshes when a live texture's source changes
  - files: `Gui/Viewport3D.cpp`, `Engine/Nodes/Scene/Material3D.cpp`
  - approach: the viewport already re-composes when the viewed node's stack hash changes (M19.P1.T3); confirm the `?v=<hash>` path change propagates through Storm's texture registry as a reload, and add an explicit `HdChangeTracker` material-dirty mark if a stale texture persists. Keep it simple: whole-texture reload per change is acceptable for v1 (design doc, Risks).
  - verify: manual — paint a stroke upstream of a live-textured material and see it appear on the model in `Viewport3D` without touching the viewport.
  - size: S

## Phase 53.3: Paint workflow

- [ ] M53.P3.T1 — `PaintThroughCamera` preset group
  - files: a shipped PyPlug/preset under the existing built-in presets location, `Gui/` only if the preset loader needs a menu entry, brief usage notes in `Engine/Nodes/Scene/README.md` (create if absent)
  - approach: a group node wiring `HydraRender`(camera) → `RotoPaint` → `ProjectTexture`(same camera, same prim) → 2D `Merge` over an optional existing-texture input → output image, with the camera path and prim selector promoted to the group. Its output feeds `Material3D`'s diffuse input. Multi-angle painting is several of these merged in 2D; say so in the notes.
  - verify: manual on real hardware — paint a stroke in the 2D viewer on the rendered view; the stroke shows on the model in `Viewport3D` from a different angle, correctly occluded where another prim was in front.
  - size: M

- [ ] M53.P3.T2 — Paint pipeline integration test in CI
  - files: `Tests/ProjectTexture_Test.cpp` (extend), reference `.usda` with a UV-mapped quad and an occluder
  - approach: headless graph — `ReadScene` + `Camera3D` + `Constant` → `ProjectTexture` → assert coverage and occlusion; `ProjectTexture` → `Material3D`(live) → `WriteScene`(export textures) → assert the exported `.exr` matches the `ProjectTexture` output. No GL involved (`SceneRaster` is CPU).
  - verify: test green in `build-and-test`.
  - size: S

**Verification gate:** CI green including M53.P3.T2; on real hardware the `PaintThroughCamera` round trip works end to end with occlusion; `WriteScene` exports a portable USD + textures that `usdview` displays with the painted texture; packaging still builds with the `Ar`/`Hio` plugin discoverable; pre-existing ctest suite green.
