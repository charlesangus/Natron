# M353 - Cycles Native Shaders

Make builder networks render correctly in Cycles, in both `USDRender` and the progressive viewer. hdCycles reads the `cycles` render context and universal, takes `cycles_<type>` IDs, and has surface, displacement and volume terminals with one connection per input. The aim is full, or nearly full, support for what Cycles offers: every shader in the manifest must at least author and load without errors, and any shader or attribute the delegate can't honour is fixed or reported.

## Scope

**In:**
- Every Cycles shader in the manifest authors through a builder and loads without a delegate error.
- Surface, displacement and volume terminals render.
- Named outputs, ramps and curve arrays, and enums reach the delegate correctly.
- Per-material settings on the Output node, such as displacement method, wherever the delegate reads them.
- Image shaders with file paths and with `natron://` through M343 - Live Textures In Delegates.
- A shader or attribute the delegate rejects gives a node warning naming it.

**Out:**
- OSL: none (decision `2026-11-05-no-cycles-osl`).
- MoonRay: M354 - MoonRay Native Shaders.
- Live texture transport: M343 - Live Textures In Delegates.
- Volume in `USDVolumeMaterial`: M347 - Volume Material Node, which is independent of this.
- Render settings: M253 - Delegate Render Settings.

**Edge cases:**
- Delegate fix needed: goes to the charlesangus Cycles fork, as with earlier Cycles fixes.
- Attribute Cycles reads only outside Hydra: recorded in the milestone report and in the attribute's tooltip.
- No Cycles network on a material: hdCycles reads universal, so a preview or `USDMaterial` network still renders.
- Viewer edit of a shader knob: restarts progressive refinement.

**Assumptions:**
- hdCycles passes array and enum values through its network conversion. P1.T1 finds any that don't.

## Phase 353.1: Coverage

- [ ] M353.P1.T1 — Smoke test every Cycles shader
  - files: `Tests/Scene/USDRenderCyclesShaders_Test.cpp`, `Tests/Scene/CyclesShaderKnownIssues.json`
  - approach: for each manifest entry, build a minimal network wired to the right terminal and render a small frame through `USDRender` with Cycles. Collect delegate warnings and failures per shader. The known-issues file lists what P1.T2 hasn't yet resolved, and ends empty or with reasons accepted by the user.
  - verify: the test runs across every shader and reports per-shader results; shaders outside the known-issues file load with no delegate error.
  - size: M

- [ ] M353.P1.T2 — Fix delegate and authoring gaps
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, the Cycles fork's `src/hydra/material.cpp`, `Tests/Scene/CyclesShaderKnownIssues.json`
  - approach: work through P1.T1's list: named outputs, ramps and curve arrays, enums, `asset` paths. Fix authoring here, or the delegate in the fork. Report a rejected shader or attribute as a node warning.
  - verify: `USDRenderCyclesShaders_Test` passes with the known-issues file empty or holding only user-accepted reasons; an image texture's Alpha output drives a mix in a test render.
  - size: L

## Phase 353.2: Terminals and settings

- [ ] M353.P2.T1 — Render displacement and volume terminals
  - files: `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, `Tests/Scene/USDRenderCyclesTerminals_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: author `outputs:cycles:displacement` and `outputs:cycles:volume` from their terminals. Check them against Cycles' expected behaviour using known test geometry.
  - verify: gtest — displaced geometry and a volume each match their reference crop, and removing the terminal removes the effect.
  - size: M

- [ ] M353.P2.T2 — Honour per-material settings
  - files: `Engine/Nodes/Scene/Shaders/ShaderOutput.cpp`, `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, `Tests/Scene/USDRenderCyclesSettings_Test.cpp`
  - approach: list what hdCycles reads per material, such as displacement method. Expose each as an Output node knob. Report in the milestone report any Cycles shader setting hdCycles doesn't read.
  - verify: gtest — switching displacement method between bump and true changes the render.
  - size: M

## Phase 353.3: Looks

- [ ] M353.P3.T1 — Prove representative looks
  - files: `Tests/Scene/USDRenderCyclesLooks_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: build about five looks: Principled with plates on several inputs, layered through Mix Shader, glass, procedural textures with named outputs, and a normal map. Include `natron://` inputs. Compare renders with references checked by eye first.
  - verify: gtest — each look matches its reference; the user OKs the references, sent as one file each.
  - size: M

- [ ] M353.P3.T2 — Edits restart the viewer
  - files: `Gui/SceneViewport.cpp`, `Tests/gui/scene_shader_cycles.py`
  - approach: an edit to an inner shader knob or connection reaches M254 - Progressive Viewer Render's restart, with no stale network in the delegate.
  - verify: an Xvfb script at 1920x1080 selects Cycles, edits a Principled knob inside a builder and rewires a texture, and shows the changes in the viewer; screenshots go to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDRenderCycles*_Test` pass with the known-issues file empty or user-accepted; `scene_shader_cycles.py` passes under Xvfb; the user OKs references and screenshots. Acceptance is at the UAT of F2 - Native Shader Networks.

## Decisions

- 2026-11-05 — **Every shader smoke-tested, representative looks referenced (cat-plan):** a reference per shader would exceed the LFS budget, and each look exercises many shaders.
- 2026-11-05 — **Native networks, full or nearly full (user):** the renderer's own shader set, not a translation.
