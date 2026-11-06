# M356 - Automatic Preview Shader

When a builder's preview terminal is empty, translate one of its native networks into `UsdPreviewSurface` automatically, so Storm shows a useful approximation instead of its grey fallback. It updates live, and a hand-wired preview always wins. Because it is authored on universal `outputs:surface`, a renderer with no network of its own on that material reads it too.

## Scope

**In:**
- Source network: the active renderer's. If that has no network, or the active renderer is Storm, the first authored of Cycles then MoonRay.
- A committed mapping table for main surface shaders (Principled BSDF, other common Cycles BSDFs, `DwaBaseMaterial`, other common MoonRay materials) onto `UsdPreviewSurface` inputs.
- Following texture connections: a file or `natron://` image on a mapped input becomes a `UsdUVTexture`, including its alpha where mapped.
- Values update live with the source network, including animation.
- An unmapped surface shader gives a flat colour plus a node warning naming it.
- A hand-wired preview terminal turns all of this off for that builder.

**Out:**
- Translating arbitrary maps and procedurals: none. A connected shader the table can't follow counts as unmapped on that input, which keeps its value.
- Baking networks to textures: none.
- Hand-built previews: M352 - USDMaterialBuilder Node.
- Export-specific behaviour: none. Export writes what's authored.

**Edge cases:**
- Active renderer changes: the preview's source follows it, so Storm's approximation may change. Accepted by the user over a fixed source knob.
- Displacement and volume terminals: not translated. Storm ignores them for this.
- Empty network on all renderers: no preview authored, and M352 - USDMaterialBuilder Node's warning stands.
- A layered source such as `DwaLayer` or Mix Shader: uses its first branch and warns.

**Assumptions:**
- The mapping table is an approximation by design. It is reviewed by eye, not compared against delegate renders.

## Phase 356.1: Translate

- [ ] M356.P1.T1 — Translate a network to preview
  - files: `Engine/Nodes/Scene/Shaders/PreviewTranslation.{h,cpp}`, `Engine/Nodes/Scene/Shaders/manifest/preview_map.json`, `Tests/Scene/ShaderPreviewTranslation_Test.cpp`
  - approach: from a surface terminal, look the shader up in the table, then map each input's value or followed image connection to `UsdPreviewSurface` inputs with any per-input conversion. `check_manifest.py` validates the table's names against the manifests.
  - verify: gtest — Principled BSDF and `DwaBaseMaterial` values map to expected inputs; a connected image becomes a `UsdUVTexture`; an unmapped shader yields a flat colour and a warning naming it; a stale name fails the check.
  - size: L

- [ ] M356.P1.T2 — Author it live from the builder
  - files: `Engine/Nodes/Scene/USDMaterialBuilder.cpp`, `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, `Tests/Scene/ShaderPreviewAuthoring_Test.cpp`
  - approach: if the preview terminal is empty and a source exists, author the translation under `<material>/preview` on universal `outputs:surface`. Pick the source as in Scope. It takes part in the builder's hash through its source knobs, and the active renderer where that is the deciding input.
  - verify: gtest — an empty preview terminal authors the translated network; wiring one replaces it; an edit to a mapped source knob changes it; a change to an unmapped knob leaves the output identical.
  - size: M

- [ ] M356.P1.T3 — Check Storm shows it
  - files: `Tests/gui/material_builder_auto_preview.py`, `Engine/Nodes/Scene/docs/USDMaterialBuilder.md`
  - approach: build a Cycles look and a MoonRay look with a plate, without a preview, and view both in Storm. Document how the source is picked and what is approximated.
  - verify: an Xvfb script at 1920x1080 shows each look's colour and plate in Storm and updates on a knob edit; screenshots go to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `ShaderPreview*_Test` pass; `material_builder_auto_preview.py` passes under Xvfb; the user OKs the screenshots. Acceptance is at the UAT of F2 - Native Shader Networks.

## Decisions

- 2026-11-05 — **Automatic and live, from the active renderer's network (user):** chosen over an explicit button and a fixed source knob. Hand-wired always wins; unmapped shaders get a flat colour and a warning.
