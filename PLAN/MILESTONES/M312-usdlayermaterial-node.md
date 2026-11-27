# M312 - USDLayerMaterial Node

A scene node that lays texture-paint layers over the material already bound to the matched prims, as `mix(original, paint, paint alpha)` per material input, with no baking. Member of F11 - Texture Painting.

What exists today:
- **Paint metadata:** `USDTexturePaint` writes `natron/usdTexturePaint/purpose/<layer>` metadata (`Engine/Nodes/Scene/USDTexturePaint.h:76-77`, `USDTexturePaint.cpp:829-843`).
- **No paint-only alpha:** its tiles are strokes over the baked base (`USDTexturePaint.cpp:2884-2900`), so there is no paint-only alpha to mix by.
- **Live UDIM textures:** material nodes connect them through `connectTexture` (main `USDPreviewSurface.cpp:121-158`) and `natron/udimTiles` (`Engine/Nodes/Metadata/ImageMetadata.h:73`).
- **USDProject3D:** its terminal is a glslfx on the universal surface (main `USDProject3D.cpp:1201-1246`), with separate Cycles and MoonRay networks (`:219`, `:425`).
- **Storm:** builds texture, primvar and transform2d parameters for a glslfx terminal, but does not evaluate an arbitrary mix node between a texture and UsdPreviewSurface. So the Storm backend is a layered glslfx terminal, not a mix node.

## Scope
**In:**
- **`USDLayerMaterial`:**
  - Inputs: Scene, and Paint (a multi-layer UDIM image).
  - For each matched prim's bound material, it authors `<material>_layered`, which specializes the original (`UsdShadeMaterial::SetBaseMaterial`), and rebinds the matched prims to it.
- **Backends:**
  - UsdPreviewSurface. Mixable inputs: diffuseColor, emissiveColor, specularColor, metallic, roughness, clearcoat, clearcoatRoughness, opacity, occlusion, normal.
  - USDProject3D, which mixes its plate.
  - MaterialX is a declared backend that only warns.
- **Mapping:** from the purpose metadata by default, with a per-input override.
- **Supporting changes:** a Paint Only output on `USDTexturePaint`, and `MaterialBake` evaluating layered materials.

**Out:**
- Cycles and MoonRay layered networks: a follow-up milestone.
- MaterialX mixing: F26 - MaterialX Rendering.
- Inputs that can't be mixed: displacement, useSpecularWorkflow, ior, opacityMode/opacityThreshold.
- Renaming the metadata key: M427 - Native Geometry Texture Painting.

**Edge cases:**
- A material shared with unmatched prims is left untouched; the matched prims get the specialized copy.
- A prim with no bound material, or with an unknown terminal, gets a warning naming it and is passed through.
- A collection binding that beats ours gets a warning (reuse `USDAssignMaterial.cpp:716-727`).
- A layer with no alpha is treated as alpha 1.
- Normals are mixed in tangent space, then normalized.
- Scalar inputs take the layer's first channel.
- An already-layered material is specialized again, so layers stack.

**Assumptions (unconfirmed):**
- Storm picks `outputs:glslfx:surface` over the universal surface. Check `hdSt/materialNetwork.cpp` in natron-dev before P1.T2.
- M311 URIs resolve non-rgba planes (`natron://<node>/<layer>.<UDIM>`).

## Phase 312.1: Inputs and terminals
- [x] M312.P1.T1 — Paint Only output on USDTexturePaint
  - files: `Engine/Nodes/Scene/USDTexturePaint.{h,cpp}`, `USDTexturePaintCache.cpp`, `docs/USDTexturePaint.md`, `Tests/Scene/USDTexturePaint_Test.cpp`
  - approach:
    - Add an `outputMode` knob: "Output Mode", Over Base | Paint Only, default Over Base.
    - Paint Only lays strokes over transparent black, so alpha is coverage.
    - Frozen sessions read a paint-only companion set that Bake Output writes beside the flattened set (`<path>.paint.<UDIM>.exr`).
    - The cache key includes the mode.
  - verify: gtest.
    - After Bake Inputs, Paint Only gives alpha 0 off the stroke and 1 on it.
    - After Bake Output, Paint Only still matches the pre-bake paint.
    - Over Base output is unchanged.
  - size: M
- [x] M312.P1.T2 — Layered glslfx terminals for Storm
  - files: `Engine/Nodes/Scene/shaders/natronLayeredPreviewSurface.glslfx` (new; previewSurface.glslfx is Apache-2.0), `shaders/natronProject3D.glslfx`, shader CMake install
  - approach:
    - The preview-surface copy takes `<input>Paint` (float4) for each mixable input and mixes before lighting.
    - natronProject3D gains `paint` and `paintEnabled`, sampled at `st` and mixed over the plate.
  - verify: gtest via USDRender (Storm). A red-diffuse quad with a half-covered green paint texture renders green and red halves. The normal mix stays unit length.
  - size: L

## Phase 312.2: Node
- [x] M312.P2.T1 — USDLayerMaterial authoring for the UsdPreviewSurface and USDProject3D backends
  - files: `Engine/Nodes/Scene/USDLayerMaterial.{h,cpp,reg.inc}`, `docs/USDLayerMaterial.md`, `Tests/Scene/USDLayerMaterial_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - Knobs: `primPattern`, `materialPurpose`, `mix` (0-1), and one `<input>Layer` choice per mixable input (From Metadata | None | each Paint plane). Labels match script names.
    - Build the specialized material and author `outputs:glslfx:surface` to the layered terminal. Feed it the original's connections or values, plus live-texture paint (the `connectTexture` pattern). The universal surface stays.
    - Detect the backend from the terminal's `info:id` or source asset. MaterialX terminals warn "MaterialX layering is not implemented".
    - Toolbar: the USD Materials submenu if M520 - USD Toolbar Category And Submenus has landed, otherwise `PLUGIN_GROUP_3D_USD`.
  - verify: gtest on `.usda` fixtures:
    - the mapping read from metadata, and an override;
    - an unmatched prim keeps the original binding;
    - a USDProject3D card mixes its plate;
    - the MaterialX and no-material warnings;
    - the knobs save and load.
  - size: L
- [x] M312.P2.T2 — MaterialBake evaluates layered materials
  - files: `Engine/Nodes/Scene/MaterialBake.{h,cpp}`, `Tests/Scene/USDLayerMaterial_Test.cpp`
  - approach: a `MaterialBake::Backend` for the layered terminals. It evaluates the base backend, then the per-texel mix from the live paint tiles.
  - verify: gtest. Bake Inputs of a downstream USDTexturePaint over a layered quad reproduces the T1 render's texels within ulps.
  - size: M
- [x] M312.P2.T3 — Xvfb GUI script
  - files: `Tests/gui/m312_usd_layer_material.py`, fixture under `Tests/fixtures/`
  - approach: USDPreviewSurface → USDTexturePaint (Paint Only) → USDLayerMaterial. Paint, wait for `usdTexturePaintBaked`, and screenshot the 3D view.
  - verify: the script passes remotely; screenshots shared.
  - size: M

**Verification gate:**
- The M312 gtests and the unchanged M53 gtests pass in ctest.
- `m312_usd_layer_material.py` passes remotely, with screenshots shared.
- AppImage UAT is collected at F11 - Texture Painting's UAT.

## Open questions
- Cycles and MoonRay layering: follow-up milestone (PM default), unless the user wants it in M312.
- Bake Output writes a second, paint-only file set, which doubles the disk use. Alternative: store only the paint-only set and composite at read time.

## Decisions
- 2026-11-26 (PM): file overlap with M315 - Direct Texel Painting (USDTexturePaint). M312.P1.T1 lands before M315 starts.
- 2026-11-27 — **Paint Only: the mode drives the session source (user, option A).** In Paint Only each session view renders transparent black, so brushes that read the texture (blur, smear, clone, dodge, burn) see nothing under them in that mode. Bake Output writes the current mode's set; the over-base set in Paint Only is rebuilt as the paint-only set laid over the Bake Inputs base. Switching mode after a Bake Output asks for a new bake. This replaces "Bake Output always writes both sets". Paint Only colour is premultiplied by coverage, so the layer mix is original * (1 - a) + paint (PM).
- 2026-11-27 (PM) — P1: the mode switch after a Bake Output is locked (Output Mode greyed while a bake is in effect; undo frees it). A Paint Only bake writes the paint set and the over-base set rebuilt over the Bake Inputs base. normalPaint holds the painted normal premultiplied (0–1, no scale/bias; the shader decodes 2*rgb - alpha), so Phase 2 authoring must not scale or bias it. The layered shader's opacityMode is an int, so the node converts the token. Storm uses outputs:glslfx:surface before the universal surface (hdSt/renderDelegate.cpp:702, usdShade/material.cpp:486-533, OpenUSD v26.08). Open: paint that lowers opacity doesn't make an opaque material translucent; MaterialBake and the Slang projection kernels don't mirror projection paint yet.
- 2026-11-27 (PM) — P1 verified on m312-gate3 (ctest green apart from the SplitOnGaps startup flake, which passed on rerun) after merging F11's tile-key fix (8c2a3c8252). Two P1 test fixes: the layered Storm tests pin gpuTextureBudget (the automatic budget is 0 on software GL — filed to the inbox), and the Paint Only bake test pushes its command like the other bake tests.
- 2026-11-27 (PM) — P2 design calls after the implementer found two gaps in LiveTextureStore (shared with F11): (1) live textures unpremultiply and bleed colour into alpha-0 texels, but the layered shaders take premultiplied paint — LiveTextureStore gains a premultiplied delivery (a source/URI flag that skips the unpremultiply and the colour fill), used for Paint Only paint, so the shaders and the premultiplied-paint decision stay; (2) live textures only rendered the RGBA plane — renderSource renders the source's named plane, so per-input layers (roughness, normal, metallic, From Metadata) work. (3) Stacked layered materials: MaterialBake evaluates the whole stack; Storm's glslfx path reads only texture/primvar/transform2d upstream nodes, so where two layers paint one input Storm shows the later one and the node warns.
- 2026-11-27 (PM) — P2 deviations: no usdTexturePaintBaked signal exists, the GUI check waits on bakeGeneration like m53; From Metadata has no colour-plane fallback (an input with no matching purpose gets no paint); layers store as Layer<k>_<input> with natron:layerDepth; MaterialBake has no normal purpose, so normal paint isn't baked; Cycles and MoonRay don't show paint (milestone Out).
- 2026-11-27 (PM) — Gate m312-gate5 green: ctest 1078/1078, m312_usd_layer_material.py 28/28 (red Paint Only stroke over the blue material in Storm), m53_usd_texture_paint.py 27/27. PR #404 into F11.
