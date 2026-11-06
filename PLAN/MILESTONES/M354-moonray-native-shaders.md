# M354 - MoonRay Native Shaders

Make builder networks render correctly in MoonRay, in both `USDRender` and the progressive viewer, through `execComp`. hdMoonray takes `info:id` as the rdl2 class name. It reads a `moonray` render context once M325 - MoonRay Fork overrides `GetMaterialRenderContexts`; without that it reads universal only. The aim is full, or nearly full, support for what MoonRay offers: every shader in the manifest must at least author and load without errors, and any shader or attribute the delegate can't honour is fixed or reported.

## Scope

**In:**
- Every MoonRay material, map, normal map, displacement and volume shader in the manifest authors through a builder and loads without a delegate error.
- Surface, displacement and volume terminals render.
- `SceneObject` bindings such as `DwaLayer`'s materials, and normal map and `coordSys` bindings (`Material.cc:105`).
- Image shaders with `natron://` through M343 - Live Textures In Delegates, and with file paths.
- Per-material settings on the Output node wherever the delegate reads them.
- A shader or attribute the delegate rejects gives a node warning naming it.

**Out:**
- The render context override: M325 - MoonRay Fork.
- MoonRay's rejection of untiled files on disk: M252 - MoonRay Rendering's finding. This milestone doesn't change it. A failed open reports through the node warning.
- MoonRay display filters: none, they aren't shaders.
- Cycles: M353 - Cycles Native Shaders.
- `materialx_shaders`: none, MaterialX is set aside.

**Edge cases:**
- Delegate fix needed: goes to the fork from M325 - MoonRay Fork.
- `execComp` startup failure: reported as today by `MoonRayRuntime`, not as a shader error.
- `SceneObject` attribute given a non-matching shader: refused by M351 - Shader Graph Presentation's type check, so it can't be authored.
- No MoonRay network on a material: hdMoonray reads universal, so a preview or `USDMaterial` network still renders.

**Assumptions:**
- hdMoonray converts a USD connection on a `SceneObject` attribute to the right rdl2 binding. P1.T2 proves or fixes it.

## Phase 354.1: Coverage

- [ ] M354.P1.T1 — Smoke test every MoonRay shader
  - files: `Tests/Scene/USDRenderMoonRayShaders_Test.cpp`, `Tests/Scene/MoonRayShaderKnownIssues.json`
  - approach: for each manifest entry, build a minimal network wired to the right terminal and render a small frame through `USDRender` with MoonRay. Confirm the `moonray` context is chosen. Collect delegate warnings and failures per shader. The known-issues file lists what later tasks haven't yet resolved, and ends empty or with reasons accepted by the user.
  - verify: the test runs across every shader and reports per-shader results; shaders outside the known-issues file load with no delegate error; `outputs:moonray:surface` wins over universal in a test material.
  - size: M

- [ ] M354.P1.T2 — Bind scene objects and coordinate systems
  - files: `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, the MoonRay fork's `Material.cc`, `Tests/Scene/USDRenderMoonRayBindings_Test.cpp`
  - approach: author `SceneObject` attributes as connections and check hdMoonray creates the binding. Layer two materials through `DwaLayer`. Cover normal maps and `coordSys`-dependent maps. Fix in the fork where the delegate falls short.
  - verify: gtest — a `DwaLayer` of two materials renders differently from either alone; a normal map changes shading; a `coordSys`-bound map follows its transform.
  - size: L

- [ ] M354.P1.T3 — Fix remaining delegate and authoring gaps
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, the MoonRay fork's `Material.cc`, `Tests/Scene/MoonRayShaderKnownIssues.json`
  - approach: work through P1.T1's list: arrays, enums, `asset` paths. Report a rejected shader or attribute as a node warning, replacing the misleading "could not render these prims" notice for this case.
  - verify: `USDRenderMoonRayShaders_Test` passes with the known-issues file empty or holding only user-accepted reasons.
  - size: L

## Phase 354.2: Terminals and settings

- [ ] M354.P2.T1 — Render displacement and volume terminals
  - files: `Engine/Nodes/Scene/Shaders/ShaderAuthoring.cpp`, `Tests/Scene/USDRenderMoonRayTerminals_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: author `outputs:moonray:displacement` and `outputs:moonray:volume` from their terminals. Check them with known test geometry. Expose any per-material settings the delegate reads as Output node knobs.
  - verify: gtest — displaced geometry and a volume each match their reference crop, and removing the terminal removes the effect.
  - size: M

## Phase 354.3: Looks

- [ ] M354.P3.T1 — Prove representative looks
  - files: `Tests/Scene/USDRenderMoonRayLooks_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: build about five looks: `DwaBaseMaterial` with plates on several inputs, `DwaLayer` layering, glass, procedural maps and a normal map. Include `natron://` inputs. Compare renders with references checked by eye first.
  - verify: gtest — each look matches its reference through `execComp`; the user OKs the references, sent as one file each.
  - size: M

- [ ] M354.P3.T2 — Edits restart the viewer
  - files: `Gui/SceneViewport.cpp`, `Tests/gui/scene_shader_moonray.py`
  - approach: an edit to an inner shader knob or connection reaches M254 - Progressive Viewer Render's restart, with no stale network in `execComp`.
  - verify: an Xvfb script at 1920x1080 selects MoonRay, edits a `DwaBaseMaterial` knob inside a builder and rewires a map, and shows the changes in the viewer; screenshots go to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDRenderMoonRay*_Test` pass with the known-issues file empty or user-accepted; `scene_shader_moonray.py` passes under Xvfb; the user OKs references and screenshots. Acceptance is at the UAT of F2 - Native Shader Networks.

## Decisions

- 2026-11-05 — **Every shader smoke-tested, representative looks referenced (cat-plan):** a reference per shader would exceed the LFS budget, and each look exercises many shaders.
- 2026-11-05 — **Native networks, full or nearly full (user):** the renderer's own shader set, not a translation.
