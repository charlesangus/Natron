# M352 - USDMaterialBuilder Node

Make `USDMaterialBuilder` author real materials. In the main graph it is one ordinary node with a Scene output, Image inputs and promoted controls, connecting into `USDAssignMaterial` like `USDMaterial`, which stays for simple cases. Inside, one Output node holds surface, displacement and volume terminals per renderer plus a preview. One builder holds networks for several renderers on one `UsdShadeMaterial` (`outputs:cycles:surface`, `outputs:moonray:surface`, universal `outputs:surface`), and scene emission goes through `emitSceneLayer` and `SceneNodeAuthoring::openLayerStage`.

## Scope

**In:**
- `ShaderOutput` node with terminals per renderer, a preview terminal, and knobs for per-material renderer settings. Only terminals with something wired are authored.
- Authoring: shaders reachable from a terminal, under `<material>/<renderer>/<node>`, with `info:id`, non-default values, connections and named outputs. Animated knobs author the same way as `USDMaterial`.
- `Material Path` behaves as on `USDMaterial`. One builder authors one material.
- Inner Input nodes create the builder's Image arrows, as Group inputs do. An Input connects to an image shader's file input, which authors `natron://` with a linear colour space.
- Promoting any inner knob to the builder's panel. One promoted knob can drive parameters in several renderers' networks through links added by hand.
- New builder: the Output node plus the default renderer's main surface shader, wired.
- Node docs and icon.

**Out:**
- Rendering proofs in Cycles and MoonRay: M353 - Cycles Native Shaders, M354 - MoonRay Native Shaders.
- Automatic preview: M356 - Automatic Preview Shader.
- Editing materials already on a stage: M355 - Shader Tweak Node.
- Several materials per builder, and saving a builder as a library item: none yet.
- A `USDMaterial` replacement: none, it stays.

**Edge cases:**
- Storm is the active renderer when creating a builder: the starter uses Cycles.
- No terminal wired: authors the material with no outputs and warns on the node.
- Shader not reachable from any terminal: not authored.
- `MissingPluginNode` in a network: its branch isn't authored and the node warns, naming it. The rest authors.
- Input with nothing connected: the image shader falls back as `USDMaterial` does and warns.
- Image shader with a file knob and a connected Input: the connection wins.
- Two builders on one `Material Path`: compose through `USDMerge`, the same as any two scene nodes. Different attributes merge, and the stronger input wins conflicts.
- Builder copied and pasted, or nested in another Group: works as any Group. Its Material Path behaves as `USDMaterial`'s would.

**Assumptions:**
- M233 - Live Node Textures' pre-render finds image inputs through the builder Group's Input nodes. P2.T1 confirms or fixes it.

## Phase 352.1: Authoring

- [ ] M352.P1.T1 — Add the Output node
  - files: `Engine/Nodes/Scene/Shaders/ShaderOutput.{h,cpp}`, `Engine/Nodes/Scene/USDMaterialBuilder.{h,cpp}`, `Tests/Scene/ShaderOutput_Test.cpp`
  - approach: terminals per renderer from the manifest plus preview, one per builder and undeletable. Terminal inputs accept only their renderer's matching shader kind. Renderer settings knobs come from the manifest's material-level entries.
  - verify: gtest — surface terminals accept their renderer's surface shader and refuse the other renderer's and a map; a second Output is refused; a save and load round trip keeps terminals and connections.
  - size: M

- [ ] M352.P1.T2 — Author the network into USD
  - files: `Engine/Nodes/Scene/USDMaterialBuilder.{h,cpp}`, `Engine/Nodes/Scene/Shaders/ShaderAuthoring.{h,cpp}`, `Tests/Scene/USDMaterialBuilder_Test.cpp`
  - approach: walk from each terminal, author as in Scope, and emit through `emitSceneLayer`. Types come from the manifest via `SdfValueTypeNames`. Output names on connections follow M349 - Shader Node Spike's mechanism. Follow `USDMaterial.cpp:255-293` for stage and output handling.
  - verify: gtest — a builder with Cycles and MoonRay networks authors the expected prims, `info:id`s, values and both render-context outputs, read back through `UsdShadeMaterial`; an unreachable shader is absent; default values aren't authored.
  - size: L

- [ ] M352.P1.T3 — Re-author on inner edits
  - files: `Engine/Nodes/Scene/USDMaterialBuilder.cpp`, `Tests/Scene/USDMaterialBuilderHash_Test.cpp`
  - approach: inner knob and connection edits change the builder's scene hash, so downstream nodes and viewers refresh. Animated knobs change per frame, static ones don't. Edits to Backdrops don't change it.
  - verify: gtest — inner knob edit changes the hash; moving a node doesn't; an animated knob differs between frames 1 and 10 and a static network doesn't.
  - size: M

## Phase 352.2: Comp connections

- [ ] M352.P2.T1 — Feed images in through Inputs
  - files: `Engine/Nodes/Scene/USDMaterialBuilder.cpp`, `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, `Tests/Scene/USDMaterialBuilderTexture_Test.cpp`
  - approach: an Image input on the builder, through an inner Input, connects to an image shader's file input. Author `natron://` as `USDMaterial`'s texture path does, with linear colour space. Make sure M233 - Live Node Textures' pre-render finds it.
  - verify: gtest — the authored `file` is a `natron://` URI with the source's hash; changing the source changes it; a Storm render of a builder using a preview network with a Constant input shows that colour.
  - size: M

- [ ] M352.P2.T2 — Promote knobs to the builder panel
  - files: `Engine/Nodes/Scene/USDMaterialBuilder.cpp`, `Gui/KnobGui.cpp`, `Tests/gui/material_builder_promote.py`
  - approach: reuse Natron's existing knob-link and group parameter handling to expose an inner knob on the builder, and let a second inner parameter link to the same promoted knob. Promoted controls save with the builder and can be animated.
  - verify: an Xvfb script promotes Roughness from a Principled BSDF, links a `DwaBaseMaterial` parameter to it, edits it on the builder and asserts both authored values change after save and reload; a screenshot goes to the user.
  - size: M

- [ ] M352.P2.T3 — Start from a useful network
  - files: `Engine/Nodes/Scene/USDMaterialBuilder.cpp`, `Engine/Nodes/Scene/docs/USDMaterialBuilder.md`, `Tests/Scene/USDMaterialBuilderStarter_Test.cpp`
  - approach: on user creation only, not on load, add the Output and the default renderer's main surface shader, wired. Storm active means Cycles. Laid out left to right. Write docs and the icon.
  - verify: gtest — a new builder has both nodes connected and authors a surface output for that renderer; a loaded project with them removed stays as saved.
  - size: M

## Phase 352.3: Together

- [ ] M352.P3.T1 — Check the workflow end to end
  - files: `Tests/gui/material_builder_workflow.py`, `Tests/Scene/USDMaterialBuilderExport_Test.cpp`
  - approach: create a builder from the main graph, dive in, wire a preview network and a Principled BSDF, feed a Read, connect to `USDAssignMaterial` on a card, and view in Storm. Export the stage and read back the material.
  - verify: an Xvfb script at 1920x1080 passes, with main-graph, inner-graph and viewer screenshots to the user; gtest — the exported layer holds the expected outputs and shaders.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDMaterialBuilder*_Test` and `ShaderOutput_Test` pass; `material_builder_*.py` pass under Xvfb; the user OKs the screenshots. Acceptance is at the UAT of F2 - Native Shader Networks.

## Decisions

- 2026-11-05 — **One builder holds all renderers (user):** plates and shared controls are wired once, and a mismatched material path can't happen. Separate builders still compose through `USDMerge`.
- 2026-11-05 — **Starter is the active renderer's main surface shader (user):** common looks start one step from done. Storm being active picks Cycles (cat-plan).
