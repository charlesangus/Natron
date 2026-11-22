# M445 - USDOpenPBR Node

A new `USDOpenPBR` node for quick lookdev: one OpenPBR material per node, authored as MaterialX (`ND_open_pbr_surface_surfaceshader` on `outputs:mtlx:surface`), so it renders in Storm through hdSt's MaterialX support and in Cycles and MoonRay through F26 - MaterialX Rendering's translation. It is built the way `USDPreviewSurface` (today's `USDPreviewSurface`, renamed by M447 - USDPreviewSurface Rename) is: a scene source with no scene input that connects to a geometry node's Material input. Comprehensive shading (procedurals, layering, per-input networks) is `USDMaterialBuilder`'s job, with MaterialX nodes from M446 - MaterialX Builder Nodes.

## Scope

**In:**
- All 41 OpenPBR inputs as knobs, with the nodedef's defaults, grouped by its `uifolder` (Base, Specular, Transmission, Subsurface, Fuzz, Coat, Thin Film, Emission, Geometry). Labels are readable script names (`base_color` → "Base Color").
- A texture-file knob on each of the 40 texturable inputs (all but `geometry_thin_walled`), authored as an `ND_image_<type>` node read at the bound prims' `st` primvar. Colour inputs get a colour-space choice beside the file knob from project OCIO, with the per-file-type default for new nodes. Scalar and vector inputs are read raw.
- Eight live image inputs: `base_color`, `base_metalness`, `specular_roughness`, `geometry_normal`, `coat_weight`, `geometry_coat_normal`, `emission_color`, `geometry_opacity`. A connected input wins over its file knob, and is served through `natron://` exactly as on `USDPreviewSurface`.
- Normal and coat normal (from file or input) as tangent-space maps through `ND_normalmap`. Tangent and coat tangent textures are read raw.
- Use Alpha (default on): the alpha of the Base Color input, or of Emission Color when Base Color has none, drives `geometry_opacity`, as on `USDPreviewSurface`.
- A universal `UsdPreviewSurface` approximation on `outputs:surface`, for readers with no MaterialX: base colour × weight, metalness, specular roughness, IOR, coat and coat roughness, emission colour × luminance, opacity and normal, with textures followed.
- Animated knobs written as time samples. Node docs and an icon.

**Out:**
- Translation and rendering in Cycles and MoonRay: M440 - MaterialX Translation Core, M442 - MoonRay MaterialX Backend, M443 - Cycles MaterialX Backend.
- Renaming `USDMaterial`: M447 - USDPreviewSurface Rename.
- Networks on inputs, layering and procedurals: M446 - MaterialX Builder Nodes.

**Edge cases:**
- A texture path that doesn't resolve: authored as written; the node warns naming it, as `USDPreviewSurface` does.
- An image input and a file knob on the same input: the image input wins.
- Thin Walled: a uniform boolean, knob only, never textured.
- A renderer that reads both outputs: MaterialX-aware renderers pick `mtlx`; the universal preview is read only by those without it.

**Assumptions:**
- Storm renders OpenPBR through hdSt's MaterialX support in our build. M439 - MaterialX Translation Spike's P1.T5 checks; if it fails, the freshness check at claim re-plans P2.T1.

## Phase 445.1: Node

- [ ] M445.P1.T1 — Add USDOpenPBR with every input as a knob
  - files: `Engine/Nodes/Scene/USDOpenPBR.{h,cpp}`, `Engine/Nodes/Scene/USDOpenPBR.reg.inc`, `Engine/CMakeLists.txt`, `Tests/Scene/USDOpenPBR_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: mirror `USDPreviewSurface`'s structure (`USDPreviewSurface.h`, its `.reg.inc`, Prim Path behaviour and Material-input wiring). Generate the 41 knobs from a committed table of the nodedef's inputs (name, type, default, folder), with labels derived from script names. Author the material prim, an `OpenPBR` shader with `info:id` `ND_open_pbr_surface_surfaceshader`, non-default values only, and `outputs:mtlx:surface`. Animated knobs author time samples.
  - verify: gtest — a default node authors the shader with no input values and the mtlx surface output; setting `coat_weight` authors that value; an animated `base_color` authors time samples; the knob table matches the nodedef in the image's MaterialX libraries input for input.
  - size: L

- [ ] M445.P1.T2 — Texture files on every texturable input
  - files: `Engine/Nodes/Scene/USDOpenPBR.cpp`, `Tests/Scene/USDOpenPBR_Test.cpp`
  - approach: a file knob for each of the 40 texturable inputs; when set, author an `ND_image_<type>` node named after the input and read at a shared `ND_texcoord_vector2` on `st`, connected to the input. Colour inputs carry the chosen colour space. Normal and coat normal go through `ND_normalmap`. Warn on a path that doesn't resolve.
  - verify: gtest — a set colour texture authors image, texcoord and connection with its colour space; a roughness texture is read raw; a normal texture goes through normalmap; a missing path warns naming it.
  - size: M

- [ ] M445.P1.T3 — Live image inputs and Use Alpha
  - files: `Engine/Nodes/Scene/USDOpenPBR.cpp`, `Tests/Scene/USDOpenPBR_Test.cpp`
  - approach: the eight image inputs in Scope **In**, served as `natron://` paths exactly as `USDPreviewSurface`'s are (default path for a static image, per-frame time samples for an animated one), and winning over the file knob. Use Alpha drives `geometry_opacity` from the Base Color input's alpha, or Emission Color's, as `USDPreviewSurface` does.
  - verify: gtest — a connected Base Color input authors a `natron://` image node; an animated source authors per-frame paths; with Use Alpha on, opacity is wired from the image's alpha; with it off, no opacity is authored.
  - size: M

- [ ] M445.P1.T4 — Author the universal preview approximation
  - files: `Engine/Nodes/Scene/USDOpenPBR.cpp`, `Tests/Scene/USDOpenPBR_Test.cpp`
  - approach: alongside the MaterialX network, author a `UsdPreviewSurface` on `outputs:surface` with the mappings in Scope **In**; textures and live inputs on mapped inputs become `UsdUVTexture`s, as on `USDPreviewSurface`.
  - verify: gtest — the preview's diffuseColor is base colour × weight; a base colour texture appears on both networks; `USDWrite` output holds both outputs.
  - size: M

## Phase 445.2: Proof

- [ ] M445.P2.T1 — Render in every renderer
  - files: `Tests/Scene/USDOpenPBRRender_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: render a sphere with OpenPBR presets (plastic, metal, glass, coated, subsurface) through `USDRender` in Storm, Cycles and MoonRay, plus one live-texture case per renderer.
  - verify: gtest — each preset matches its per-renderer reference crop with no fallback warning; the live-texture case updates when its source changes.
  - size: M

- [ ] M445.P2.T2 — Docs, icon and panel layout
  - files: `Engine/Nodes/Scene/docs/USDOpenPBR.md`, `Gui/Resources/Images/` icon, `Tests/gui/scene_openpbr_layout.py`
  - approach: node docs covering every knob group, the textures and inputs, Use Alpha, and the preview approximation, and pointing to `USDMaterialBuilder` for comprehensive shading. A GUI script that opens the panel and screenshots each group at 1920x1080.
  - verify: the script passes under Xvfb; one screenshot per group is sent to the user as separate files.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDOpenPBR_Test` and `USDOpenPBRRender_Test` pass; `scene_openpbr_layout.py` passes under Xvfb. The user's sign-off on screenshots and references is collected at the UAT of F26 - MaterialX Rendering.

## Decisions

- 2026-11-15 — **A new USDOpenPBR node, not a mode on USDMaterial (user, grilling):** USDMaterial becomes USDPreviewSurface (M447 - USDPreviewSurface Rename).
- 2026-11-15 — **Quick lookdev (user):** every input is a knob with a texture-file knob; eight live image inputs; comprehensive shading belongs in USDMaterialBuilder.
- 2026-11-15 — **Universal UsdPreviewSurface approximation authored too (user):** for apps and renderers that read no MaterialX.
