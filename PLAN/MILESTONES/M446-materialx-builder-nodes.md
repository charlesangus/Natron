# M446 - MaterialX Builder Nodes

Bring MaterialX nodes into `USDMaterialBuilder`, so comprehensive MaterialX networks are built in Natron and render in Storm, Cycles and MoonRay. It extends F2 - Native Shader Networks' manifest (M350 - Shader Manifest And Nodes) with entries generated from MaterialX nodedefs, adds an `mtlx` terminal set to `ShaderOutput` (M352 - USDMaterialBuilder Node), and adds two MaterialX builder presets. MaterialX defines one nodedef per type variant (about 1,000 in the standard libraries). Natron shows one node per node name (about 250) with a Type knob instead. Ships on its own after F2 - Native Shader Networks and F26 - MaterialX Rendering land.

## Scope

**In:**
- Manifest entries for every node in MaterialX's stdlib, pbrlib and bxdf libraries, and for custom libraries found on `PXR_MTLX_PLUGIN_SEARCH_PATHS` at startup.
- One Natron shader node per MaterialX node name, with a Type knob choosing the nodedef. The type follows the first connection, and mismatched links are refused by the shader data-kind check.
- Every nodedef input as a knob, promotable to a port per M350 - Shader Manifest And Nodes' rules; labels are readable script names; panel groups from `uifolder`; docs and tooltips from the nodedef's `doc`.
- Multi-output nodes (`separate3`, `extract`…) with named outputs, using M349 - Shader Node Spike's named-output mechanism.
- `ShaderOutput` gains `mtlx` surface, displacement and volume terminals; the network authors under `<material>/mtlx/<node>` with `info:id` the chosen nodedef, non-default values and connections.
- Builder presets `USDMaterialXOpenPBRBuilder` (open_pbr_surface on the mtlx surface terminal) and `USDMaterialXStandardSurfaceBuilder` (standard_surface), as menu items that create `USDMaterialBuilder` with that starter network, the way Merge's presets create `Merge`.

**Out:**
- The native presets (`USDCyclesPrincipledBSDFBuilder`, `USDMoonRayDwaBaseBuilder`) and the blank builder: M352 - USDMaterialBuilder Node.
- Translation and rendering: F26 - MaterialX Rendering.
- An automatic preview for MaterialX networks: none. Storm reads `mtlx` itself, so M356 - Automatic Preview Shader needs no MaterialX source.

**Edge cases:**
- Changing a node's Type: knobs are rebuilt; values carry over where the types convert (float to colour is broadcast, colour to float takes the first channel, otherwise reset to default); connections that no longer type-check are cut, and the node warns naming them.
- MaterialX and native shader nodes in one network: refused by the data-kind check; each context's network is separate.
- A custom library missing at load: the node loads as `MissingPluginNode`, keeping its values and connections, as M350 - Shader Manifest And Nodes does for renderer upgrades.
- A nodedef that the active renderer's backend can't translate: authors normally; the render shows F26's fallback and warning.

**Assumptions:**
- M350 - Shader Manifest And Nodes' manifest schema can carry a node with several nodedefs selected by a Type knob. P1.T1 extends it if it can't.

## Phase 446.1: Nodes

- [ ] M446.P1.T1 — Generate MaterialX manifest entries
  - files: `tools/shaders/gen_mtlx_manifest.py` (or the C++ generator beside M350 - Shader Manifest And Nodes' Cycles one), `Engine/Nodes/Scene/Shaders/manifests/mtlx.json`, `Engine/Nodes/Scene/Shaders/ShaderManifest.{h,cpp}`
  - approach: walk every nodedef in the image's MaterialX libraries, grouped by node name, and write one entry per node with its typed nodedefs, inputs (name, type, default, `uifolder`, `doc`, uniform flag), and outputs. At startup, add entries for nodedefs found on `PXR_MTLX_PLUGIN_SEARCH_PATHS` through the same code path. Extend the schema for the Type choice if needed.
  - verify: regeneration is byte-stable; the manifest has an `add` entry with every type variant and an `open_pbr_surface` entry with its 41 inputs; a custom library in a temporary search path adds its node at startup.
  - size: L

- [ ] M446.P1.T2 — Type knob and connection typing
  - files: `Engine/Nodes/Scene/Shaders/MtlxShaderNode.{h,cpp}`, `Tests/Scene/MtlxShaderNode_Test.cpp`
  - approach: the Type knob lists the node's nodedef types. The first connection to an untyped node picks the matching type. Changing it rebuilds knobs with the carry-over rules in Scope **Edge cases**, and cuts and warns about links that no longer type-check.
  - verify: gtest — connecting a color3 output to a new `add` sets color3; switching to float carries the first channel; an incompatible downstream link is cut with a warning naming it.
  - size: L

- [ ] M446.P1.T3 — Author the mtlx terminals
  - files: `Engine/Nodes/Scene/Shaders/ShaderOutput.{h,cpp}`, `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, `Tests/Scene/USDMaterialBuilderMtlx_Test.cpp`
  - approach: add `mtlx` surface, displacement and volume terminals; author reachable MaterialX nodes under `<material>/mtlx/<node>` with `info:id` the chosen nodedef, non-default values, connections and named outputs, onto `outputs:mtlx:surface`, `:displacement` and `:volume`.
  - verify: gtest — a builder with open_pbr_surface fed by noise3d through separate3 authors the expected prims, ids and named-output connections, and round-trips save and load.
  - size: M

- [ ] M446.P1.T4 — Add the MaterialX builder presets
  - files: `Gui/Resources/PyPlugs/initGui.py`, `Engine/Nodes/Scene/docs/USDMaterialBuilder.md`, `Tests/gui/material_builder_mtlx_presets.py`
  - approach: menu items `USDMaterialXOpenPBRBuilder` and `USDMaterialXStandardSurfaceBuilder` that create `USDMaterialBuilder` with the surface node wired to the mtlx surface terminal, laid out left to right, like the Merge presets. Projects only ever hold `USDMaterialBuilder`. Document both.
  - verify: the GUI script creates each preset from the tab menu under Xvfb at 1920x1080, finds the wired network inside, and sends one screenshot per preset to the user.
  - size: M

## Phase 446.2: Proof

- [ ] M446.P2.T1 — Render builder networks everywhere
  - files: `Tests/Scene/USDMaterialBuilderMtlxRender_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: build an OpenPBR network with a fractal3d into base colour, an image into roughness and a displacement, and render it through `USDRender` in Storm, Cycles and MoonRay; edit one inner knob and render again.
  - verify: gtest — each renderer matches its reference crop with no fallback warning, and the edit changes all three renders.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `MtlxShaderNode_Test`, `USDMaterialBuilderMtlx_Test` and `USDMaterialBuilderMtlxRender_Test` pass; `material_builder_mtlx_presets.py` passes under Xvfb; the user OKs the screenshots and tests an AppImage (UAT steps in its `.md`).

## Decisions

- 2026-11-15 — **One node per MaterialX node name with a Type knob (user, grilling):** about 250 nodes instead of about 1,000 typed ones.
- 2026-11-15 — **Builder presets instead of a starter rule (user):** a plain builder starts blank; presets create it with a starter network, like Merge's presets. MaterialX OpenPBR and Standard Surface presets here; Cycles Principled BSDF and MoonRay DwaBaseMaterial presets in M352 - USDMaterialBuilder Node.
- 2026-11-15 — **MaterialX and native nodes never share a network:** each render context's network is separate in USD and the delegates read one context each.
