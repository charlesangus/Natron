# M233 - Live Node Textures

Feed images from the node graph straight into USD materials, live and animated, with no
disk round trip. This is a must-have for comp 3D per the user (2026-10-26 grill). It was
split out of M53 - Projected Texture Painting, whose tasks M53.P2.T2–T5 it replaces.
Mechanism: a `natron://` URI served by an in-process `ArResolver` + `HioImage` plugin, as
designed in `PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md` ("Live textures"),
adjusted for M19's long-lived scene (`PLAN/DESIGN/2026-10-25-usd-long-lived-scene-review.md`).
Survey: `PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- `USDMaterial` gets optional Image inputs for diffuseColor, roughness, metallic, normal and emissive. A connected input wins over the file knob.
- Animated textures (a plate sequence on a card). The texture follows the frame in the Viewer's 3D view and in USDRender.
- The resolver serves only images already rendered by Natron's render engine. It never renders from inside Hydra.
- USDWrite gets "Export Live Textures", which writes per-frame EXRs beside the output and rewrites the paths.

**Out:**
- Camera projection — M234 - Camera Projection Nodes.
- UV bake and paint — M53 - Projected Texture Painting.
- Image inputs on primitive nodes — none (user).
- GPU-resident zero-copy texture upload into Storm — none (v1 uploads from CPU).

**Edge cases:**
- The source node is deleted, or its render fails: the URI resolves to nothing, Storm shows its fallback texture, and USDMaterial shows a node warning.
- The texture is not ready yet when Hydra asks: the resolver returns not-found. The consumer pre-renders (M233.P2.T1) so this only happens on a race, and the next sync picks it up.
- A static input (same image hash every frame): one URI with no per-frame time samples, so it is uploaded once.
- A large texture (8K): the size is that of the input's RoD at the project format. No automatic downscale in v1 — Unconfirmed: add a resolution-cap knob on USDMaterial if memory becomes a problem.
- Non-RGBA layers: diffuse/emissive read `rgb`; roughness/metallic read the first channel of the chosen layer (default `rgba.r`).

**Assumptions:**
- Time-sampled `asset` attributes (`inputs:file` with a different `natron://…?v=<hash>` per frame) are honoured by UsdImaging/Storm texture loading per frame.
- An `ArResolver` plugin registered through `plugInfo.json` from Natron's plugin dir coexists with the default resolver (USD ≥ 24.11 URI resolvers by scheme).

## Phase 233.1: Resolver and image plugin

- [ ] M233.P1.T1 — Live-texture store and the `natron://` `ArResolver`
  - files: `Engine/Nodes/Scene/LiveTextureStore.{h,cpp}`, `Engine/Nodes/Scene/LiveTextureResolver.{h,cpp}` (`ArResolver` + `ArAsset`), `Engine/Nodes/Scene/plugInfo.json` and CMake install rules, the AppImage/bundle staging script, `Tests/Scene/LiveTexture_Test.cpp`
  - approach: URI `natron://<nodeScriptName>/<layer>?v=<imageHash>`. The store maps the URI to an immutable float RGBA buffer, filled only by Natron renders and evicted by an LRU budget and on cache purge / Free GPU Memory. The resolver is a URI resolver for the `natron` scheme: `Resolve` returns the URI if the store has it, and `OpenAsset` returns an `ArAsset` over the buffer. It never triggers a render. Install `plugInfo.json` where `PlugRegistry` finds it in the build tree, the install and the AppImage.
  - verify: gtest — a stored buffer opens through `ArGetResolver().OpenAsset()` with the expected bytes; a missing URI fails cleanly; eviction makes it fail. The bundle startup check finds the plugin.
  - size: L

- [ ] M233.P1.T2 — `HioImage` plugin for `natron://` assets
  - files: `Engine/Nodes/Scene/LiveTextureImage.{h,cpp}`, `Engine/Nodes/Scene/plugInfo.json`, `Tests/Scene/LiveTexture_Test.cpp`
  - approach: an `HioImage` subclass registered for the scheme that reads the `ArAsset` from T1 as RGBA float with the correct size. Flip once from Natron's bottom-up layout to Hio's top-down.
  - verify: gtest — `HioImage::OpenForReading` on a stored URI reads matching dimensions, and the corner pixels match a known gradient (orientation checked).
  - size: M

## Phase 233.2: Material inputs

- [ ] M233.P2.T1 — USDMaterial image inputs authored as time-sampled live URIs
  - files: `Engine/Nodes/Scene/USDMaterial.{h,cpp}`, `Engine/Nodes/Scene/SceneNodeAuthoring.cpp`, `Tests/Scene/USDMaterial_Test.cpp`
  - approach: add optional Image inputs per channel. For a connected input, author a `UsdUVTexture` whose `inputs:file` is time-sampled per project frame with the input's per-frame image hash in `?v=`, or a single default when the hash doesn't vary. Hashes come from the engine without rendering, and they go into the layer key so a change re-authors the one layer. The scene action records which (node, layer, frame) URIs the layer needs.
  - verify: gtest — a Constant into diffuse authors a single-default `natron://` path; an animated input authors one sample per frame with distinct `v=` values; changing the upstream colour changes the hash and re-authors only USDMaterial's layer.
  - size: M

- [ ] M233.P2.T2 — Consumers pre-render the live textures they need
  - files: `Gui/SceneViewport.cpp` (viewer sync worker), `Engine/Nodes/Scene/HydraRenderImpl.cpp` (USDRender job), `Engine/Nodes/Scene/LiveTextureStore.cpp`, `Tests/Scene/USDRenderStorm_Test.cpp`, `Tests/gui/scene_live_texture.py`
  - approach: before syncing Hydra for a frame, the viewer's worker and the USDRender job collect the live URIs that frame needs (from the stage's `UsdUVTexture` inputs at that time) and render any missing ones through the normal render engine on their own thread. The result goes into the store before Storm commits. The GUI thread never renders, which addresses the deadlock risk recorded in M53's 2026-10-25 decision. Missing textures after a failed render leave Storm's fallback.
  - verify: strict-GL test — a Constant (red) into a card's material renders red through USDRender; an animated Ramp source gives different pixels at frames 1 and 10; Xvfb GUI script — editing the Constant's colour updates the card in the 3D view without touching the viewport.
  - size: L

- [ ] M233.P2.T3 — USDWrite exports live textures
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: an "Export Live Textures" bool (default on). For each `natron://` URI in the written range, render it to `<usd basename>_textures/<node>_<layer>.####.exr` (one file when static), and rewrite the exported attribute to the relative path, with a `<UDIM>`-free frame token through time samples. Without the option, URIs are written as-is with a warning that the file is not portable.
  - verify: gtest — export of an animated live texture writes N EXRs and a layer whose time samples point at them; `USDRead` of the export renders the same pixels through USDRender as the live scene.
  - size: M

**Verification gate:** release gate green; strict-GL live-texture test passes; on real hardware a plate sequence on a USDCard plays in the 3D view and in USDRender; an export opens in `usdview` with textures; screenshots shared before sign-off.

## Decisions

- 2026-10-28: GUI checks pass on Actions (M269 remote runs); AppImage and UAT doc in build/appimages/, contact sheet build/uat-shots/; awaiting user UAT.
- 2026-10-26 — **Live textures split from M53 and moved into the MVP (user, M19/M20 UAT grill).**
- 2026-10-26 — **The resolver never renders (cat-plan):** consumers pre-render needed URIs on their own worker before Hydra sync; this replaces M53's plan of rendering inside `OpenAsset`, which risked deadlock on the GUI thread.
- 2026-10-26 — **Animated textures by time-sampled asset paths (cat-plan):** the payload stays time-independent (M19), and per-frame hashes come without rendering.
