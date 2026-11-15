# M442 - MoonRay MaterialX Backend

Render MaterialX materials in MoonRay, in `USDRender` and the progressive viewer, through M440 - MaterialX Translation Core. Pattern nodes need almost no work: M441 - MoonRay MaterialX Shaders Build ships `ND_*` map shaders that hdMoonray creates straight from the node id. This backend's job is surfaces, terminals and the gaps. MoonRay has no closure-level shaders, so every MaterialX surface shader in the libraries is mapped directly onto a MoonRay material. A raw pbrlib closure graph can't be translated and falls back. MoonRay maps have one output each, so `separate2/3/4` expand to `extract` nodes. `dot` nodes are removed. Convolution nodes and uniform string bindings are MoonRay limits and are reported unsupported.

## Scope

**In:**
- A MoonRay backend registered with the core, with the render-context handling M439 - MaterialX Translation Spike chose. If hdMoonray must list `mtlx` after `moonray` in `GetMaterialRenderContexts`, that change goes in the hdMoonray fork.
- Atomic set: every `ND_*` class present in the installed `rdl2dso/`, passed through with its id unchanged.
- Direct surface mappings onto `DwaBaseMaterial` for every surface shader in MaterialX's bxdf library: open_pbr_surface, standard_surface, UsdPreviewSurface, gltf_pbr, disney_principled, and the lama surfaces where `DwaBaseMaterial` can express them. A committed per-parameter table, with approximations flagged.
- Displacement onto `NormalDisplacement` or `VectorDisplacement`; volume closures onto `BaseVolume`.
- Expansions: `separateN` into `extract` nodes, `dot` removed, `normalmap` onto the material's normal input, primvar reads onto MoonRay's attribute maps.
- Pattern parity against the reference evaluator for every atomic node, and the backend's coverage list.

**Out:**
- Nodes MoonRay can't run: convolution (`blur`, `heighttonormal`) and uniform `string`/`filename` bindings are listed unsupported with their reason. Nothing replaces them.
- Cycles: M443 - Cycles MaterialX Backend.

**Edge cases:**
- A surface parameter `DwaBaseMaterial` can't express (OpenPBR thin film, dispersion): the approximation renders, with an info note when the value is away from its default.
- A raw pbrlib closure network (`dielectric_bsdf` and `layer` wired by hand): untranslatable in MoonRay, so it falls back with the warning naming the first closure.
- `fractal2d` (missing upstream): implemented by inlining if MaterialX defines it as a nodegraph, otherwise listed unsupported.
- A map output bound where MoonRay wants a `SceneObject` (normal map binding): goes through the material's normal map attribute, as `Material.cc:105` expects.

**Assumptions:**
- `DwaBaseMaterial` covers OpenPBR's base, specular, transmission, subsurface, fuzz, coat, emission and opacity lobes closely enough for a per-parameter table. P1.T2 records the fit for each parameter.

## Phase 442.1: Backend

- [ ] M442.P1.T1 — Register the MoonRay backend
  - files: `Engine/Nodes/Scene/Mtlx/MtlxMoonRayBackend.{h,cpp}`, `Engine/AppManager.cpp`, the hdMoonray fork's `lib/hydramoonray/RenderDelegate.cc` if the note calls for it, `Tests/Scene/USDRenderMoonRayMtlx_Test.cpp`
  - approach: build the atomic set from the `ND_*` classes in MoonRay's `rdl2dso/` at startup, so it tracks the installed shaders. Pass atomic nodes through unchanged. Apply M439 - MaterialX Translation Spike's context handling; a fork change follows M354 - MoonRay Native Shaders' rule (push the submodule fork, bump its gitlink in openmoonray, then the pin). Build the flat magenta fallback as a `DwaBaseMaterial`.
  - verify: gtest — an `mtlx` material of noise3d into multiply into a direct-mapped surface renders in MoonRay matching a reference crop; an untranslatable one renders magenta with the warning.
  - size: L

- [ ] M442.P1.T2 — Map surface shaders onto DwaBaseMaterial
  - files: `Engine/Nodes/Scene/Mtlx/MtlxMoonRaySurfaces.cpp`, `Tests/Scene/USDRenderMoonRayMtlx_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: one committed table per surface shader, listing for each of its inputs the `DwaBaseMaterial` attribute, any conversion (weights times colours, IOR to reflectivity, radius scale), and whether it is approximate. Read MoonRay's class JSON for attribute names and ranges. Cover every bxdf surface listed in Scope **In**; a lama surface `DwaBaseMaterial` can't express goes to the known-unsupported list with its reason.
  - verify: gtest — a sphere per surface shader at its defaults and at one non-default setting per lobe matches its reference crop; every input of each mapped surface appears in its table.
  - size: L

- [ ] M442.P1.T3 — Route displacement and volume
  - files: `Engine/Nodes/Scene/Mtlx/MtlxMoonRayBackend.cpp`, `Tests/Scene/USDRenderMoonRayMtlx_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: `displacement_float` onto `NormalDisplacement` and `displacement_vector3` onto `VectorDisplacement`, with scale. Volume closures (`absorption_vdf`, `anisotropic_vdf`, emission) onto `BaseVolume`'s attenuation, scattering, anisotropy and emission.
  - verify: gtest — a displaced plane and a VDB with an `mtlx` volume each match their reference crop, and removing the terminal removes the effect.
  - size: M

- [ ] M442.P1.T4 — Expand nodes MoonRay lacks
  - files: `Engine/Nodes/Scene/Mtlx/MtlxMoonRayBackend.cpp`, `Tests/Scene/MtlxMoonRayBackend_Test.cpp`
  - approach: `separate2/3/4` become one `ND_extract_*` per connected output; `dot` nodes are removed and their links joined; `normalmap` feeds the material's normal input; `geompropvalue`/`texcoord` use the primvar names the core kept. Convolution and uniform string bindings are reported unsupported.
  - verify: gtest on the translated network — a separate3 with two used outputs becomes two extract nodes wired to the right channels; a dot chain disappears; a blur reports unsupported.
  - size: M

## Phase 442.2: Proof

- [ ] M442.P2.T1 — Coverage list and pattern parity
  - files: `Tests/Scene/data/mtlx/moonray-unsupported.json`, `Tests/Scene/MtlxParityMoonRay_Test.cpp`, `Tests/Scene/MtlxCoverage_Test.cpp`
  - approach: register the backend in `MtlxCoverage_Test`, and list every unsupported nodedef with its reason. For every atomic nodedef the backend translates, run M440 - MaterialX Translation Core's parity harness: reference evaluation against a MoonRay render, each type variant at defaults and at one non-default input set, with a per-node tolerance.
  - verify: `MtlxCoverage_Test` passes with the MoonRay list; `MtlxParityMoonRay_Test` passes for every translated atomic node, or the node is on the list with its reason, accepted by the user.
  - size: L

- [ ] M442.P2.T2 — Viewer check and docs
  - files: `Tests/gui/scene_mtlx_moonray.py`, `Engine/Nodes/Scene/docs/MaterialX.md`
  - approach: a GUI script that opens a `.mtlx` through USDRead, renders it in the progressive viewer with MoonRay, edits a parameter through a stage edit and sees the render update. Fill the MoonRay section of the doc: coverage, unsupported nodes and why, approximated parameters per surface.
  - verify: the script passes under Xvfb at 1920x1080; one screenshot of the viewer is sent to the user; the doc's unsupported list matches `moonray-unsupported.json`.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDRenderMoonRayMtlx_Test`, `MtlxMoonRayBackend_Test`, `MtlxParityMoonRay_Test` and `MtlxCoverage_Test` pass; `scene_mtlx_moonray.py` passes under Xvfb. The user's sign-off on references, screenshots and the unsupported list is collected at the UAT of F26 - MaterialX Rendering.
