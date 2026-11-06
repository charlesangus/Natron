# M355 - Shader Tweak Node

A `USDShaderTweak` scene node that edits materials already on the stage, such as those in imported assets, without rebuilding them. A list of tweaks changes parameter values, and an Image input can route a comp image into a texture's file, which is the core comp case of putting a plate on an imported asset's material. It reads whatever `UsdShade` network is on the stage, so it needs no manifest and works with any renderer's shaders.

## Scope

**In:**
- Materials chosen by path pattern, as `USDAssignMaterial` matches its targets.
- A list of tweaks: shader prim, input name, mode (replace, add, multiply) and value, with the value's knob typed from the attribute. Each row can be enabled, disabled and animated.
- A picker that browses materials, their shaders and inputs on the incoming stage.
- Image swaps: a tweak replaces a texture's file with `natron://` from one of the node's Image inputs.
- Authored as overs in the node's own layer through `emitSceneLayer`, so upstream layers are untouched.
- Node docs and icon.

**Out:**
- Adding or breaking connections, or inserting shaders: none. Use M352 - USDMaterialBuilder Node.
- Creating materials: M352 - USDMaterialBuilder Node.
- Reassigning materials: `USDAssignMaterial`.
- Live texture transport to delegates: M343 - Live Textures In Delegates.

**Edge cases:**
- Pattern matches nothing, or a named input is missing: warning on the node naming it, and the other tweaks apply. An Ignore Missing knob silences it for assets where some materials lack the input.
- Add or multiply on a type that doesn't support it (string, asset, token): refused on that row with a warning.
- Input driven by a connection, not a value: a value tweak has no effect there, so the node warns on that row. An image swap on the upstream texture's file is the way through.
- Matched prim is instanced: authoring can't override inside an instance proxy, so the node warns naming it.
- Image input unconnected on a swap tweak: the row is skipped with a warning.
- Same input on two rows: applied in list order.

**Assumptions:**
- The incoming stage is readable at edit time, as `USDAssignMaterial`'s matching does.

## Phase 355.1: Values

- [ ] M355.P1.T1 — Author value tweaks
  - files: `Engine/Nodes/Scene/USDShaderTweak.{h,cpp}`, `Engine/Nodes/Scene/USDShaderTweak.reg.inc`, `Tests/Scene/USDShaderTweak_Test.cpp`
  - approach: add a native scene node with one Scene input. Match materials as `USDAssignMaterial` does. For each row, resolve the shader and input through `UsdShade`, compute the value by mode from the incoming value, and author as an over in the node's layer.
  - verify: gtest — replace, add and multiply on float and colour inputs give the expected authored values on two matched materials; the upstream layer is unchanged; a missing input warns and the other rows still apply.
  - size: L

- [ ] M355.P1.T2 — Add the tweak list and picker
  - files: `Engine/Nodes/Scene/USDShaderTweak.cpp`, `Gui/ShaderTweakPicker.{h,cpp}`, `Tests/gui/shader_tweak_list.py`
  - approach: use the existing dynamic or user knob mechanism for rows, each typed from the picked attribute, saved with the node and animatable. The picker lists materials, their shaders and inputs from the incoming stage.
  - verify: an Xvfb script at 1920x1080 loads a USD asset, picks a parameter, edits and animates it, saves and reloads, and sees the viewer change; a screenshot goes to the user.
  - size: L

## Phase 355.2: Images

- [ ] M355.P2.T1 — Swap textures for comp images
  - files: `Engine/Nodes/Scene/USDShaderTweak.{h,cpp}`, `Tests/Scene/USDShaderTweakImage_Test.cpp`
  - approach: Image inputs on the node; a swap row points a texture input (`file`, or a native shader's equivalent) at one of them, authoring `natron://` with a linear colour space where the shader has that setting. Make sure M233 - Live Node Textures' pre-render finds the input.
  - verify: gtest — a `UsdUVTexture`'s file becomes the source's URI and follows its hash; a Storm render shows a Constant's colour on an asset; a native Cycles image texture in a hand-authored layer gets the URI and renders that colour through Cycles.
  - size: M

- [ ] M355.P2.T2 — Check native networks in each renderer
  - files: `Tests/Scene/USDShaderTweakRenderers_Test.cpp`, `Engine/Nodes/Scene/docs/USDShaderTweak.md`, one reference crop per renderer (≤256 px, half-float, zip)
  - approach: tweak values and swap images on materials from a builder, rendered through Cycles and MoonRay. Write the docs and icon.
  - verify: gtest — a tweaked `DwaBaseMaterial` and a tweaked Principled BSDF each match their reference crop.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDShaderTweak*_Test` pass; `shader_tweak_list.py` passes under Xvfb; the user OKs the screenshot. Acceptance is at the UAT of F2 - Native Shader Networks.

## Decisions

- 2026-11-05 — **Values and image swaps, no rewiring (user):** a plate on an imported material is the core comp case; rewiring overlaps the builder and is much bigger.
