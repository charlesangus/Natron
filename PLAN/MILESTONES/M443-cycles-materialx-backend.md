# M443 - Cycles MaterialX Backend

Render MaterialX materials in Cycles, in `USDRender` and the progressive viewer, by translating them into native Cycles SVM nodes through M440 - MaterialX Translation Core. hdCycles builds a native node for any `cycles_<type>` id (`src/hydra/material.cpp:619` in the Cycles fork), so the backend writes networks of those ids. Unlike MoonRay, Cycles has closure nodes, so pbrlib's closures map onto Cycles BSDFs. Surfaces with no direct mapping (lama, custom) therefore inline and render. MaterialX procedurals (noise, fractal, cellnoise, worley, unified noise, hex tiling) need new SVM nodes to match MaterialX exactly. They come with M444 - Cycles MaterialX Procedurals and sit on this backend's known-unsupported list until then.

## Scope

**In:**
- A Cycles backend registered with the core, with the render-context handling M439 - MaterialX Translation Spike chose. If hdCycles must list `mtlx` after `cycles`, that change goes in the Cycles fork (`charlesangus/cycles` `natron/deep-5.2`).
- Direct surface mappings onto `principled_bsdf` for open_pbr_surface, standard_surface, UsdPreviewSurface, gltf_pbr and disney_principled. A committed per-parameter table, with approximations flagged.
- pbrlib closures onto Cycles BSDF and closure nodes, so every other surface renders through inlining.
- Every non-procedural atomic node: math, conditional, channel, compositing, adjustment, colour transform, geometric, texture (`image`, `tiledimage`), application. Typed values split and combine across Cycles' float, colour and vector sockets.
- Displacement and volume terminals.
- Pattern parity for every translated atomic node, and the backend's coverage list.

**Out:**
- MaterialX procedurals and hex-tiled textures: M444 - Cycles MaterialX Procedurals.
- Convolution nodes (`blur`, `heighttonormal`): Cycles has no convolution in shaders, so they are listed unsupported with that reason.
- OSL: none (decision `2026-11-05-no-cycles-osl`).
- MoonRay: M442 - MoonRay MaterialX Backend.

**Edge cases:**
- A MaterialX type with no Cycles socket (matrix33/44, integer, boolean, vector4/color4): matrices become vector-math subgraphs where a node uses them. vector4/color4 travel as a colour plus a float. integer and boolean become floats. A value that can't be carried that way is unsupported, with its reason.
- `frame` and `time` nodes: constants per frame, taken from the Hydra time at translation, so they change when the frame does.
- An image with `uaddressmode`/`vaddressmode` that differ: Cycles' image node has a single extension mode, so the backend wraps the UVs in math instead.
- A `natron://` texture in an `image` node: goes through `image_texture`, the route M343 - Live Textures In Delegates proved.

**Assumptions:**
- Cycles' `principled_bsdf` (Blender 4.x/5.x model) is close enough to OpenPBR for a per-parameter table. P1.T2 records the fit for each parameter.

## Phase 443.1: Backend

- [ ] M443.P1.T1 — Register the Cycles backend
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesBackend.{h,cpp}`, `Engine/AppManager.cpp`, the Cycles fork's `src/hydra/render_delegate.cpp` if the note calls for it, `Tests/Scene/USDRenderCyclesMtlx_Test.cpp`
  - approach: register the backend for Cycles, with the type-bridging helpers every mapping uses: separate and combine nodes for swizzles and vector4/colour4, float to colour, and float to vector. Apply M439 - MaterialX Translation Spike's context handling; a fork change is pushed to `natron/deep-5.2` and `NATRON_CYCLES_COMMIT` is bumped in `tools/ci/local/Dockerfile`. Build the flat magenta fallback as a `principled_bsdf`.
  - verify: gtest — an `mtlx` material of constant into multiply into a direct-mapped surface renders in Cycles matching a reference crop; an untranslatable one renders magenta with the warning.
  - size: L

- [ ] M443.P1.T2 — Map surface shaders onto Principled BSDF
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesSurfaces.cpp`, `Tests/Scene/USDRenderCyclesMtlx_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: one committed table per surface listed in Scope **In**, mapping every input to a `principled_bsdf` socket with its conversion (weights times colours, coat, thin film, subsurface radius and scale, transmission depth), and flagging approximations. Use Cycles' socket names from its `NodeType` registry.
  - verify: gtest — a sphere per surface at defaults and at one non-default setting per lobe matches its reference crop; every input of each mapped surface appears in its table.
  - size: L

- [ ] M443.P1.T3 — Map pbrlib closures
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesClosures.cpp`, `Tests/Scene/USDRenderCyclesMtlx_Test.cpp`
  - approach: map `oren_nayar_diffuse_bsdf`, `burley_diffuse_bsdf`, `dielectric_bsdf`, `conductor_bsdf`, `generalized_schlick_bsdf`, `sheen_bsdf`, `subsurface_bssrdf`, `translucent_bsdf`, `uniform_edf`, `layer`, `mix`, `add` and `multiply` to Cycles BSDF, emission, mix-closure and add-closure nodes, with thin film where the BSDF allows it. `layer` is approximated by a weighted mix, and flagged as approximate. With these, a surface with no direct mapping inlines and renders.
  - verify: gtest — standard_surface rendered through inlining (direct mapping disabled in the test) stays within a stated tolerance of its direct mapping; a hand-wired dielectric over conductor layer renders without fallback.
  - size: L

- [ ] M443.P1.T4 — Map math and conditional nodes
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesMath.cpp`, `Tests/Scene/MtlxCyclesBackend_Test.cpp`
  - approach: every math nodedef (add through `transformmatrix`, `normalize`, `crossproduct`, `dotproduct`, `rotate2d/3d`, trig, power, `clamp`, `smoothstep`) and every conditional (`ifgreater`, `ifgreatereq`, `ifequal`, `switch`) onto `math`, `vector_math`, `map_range`, `clamp`, `mix` and the type-bridging helpers, for each type variant including the `FA` forms.
  - verify: gtest on translated networks — one case per node family checks the node ids and wiring; a numeric check per family through a one-pixel render matches the analytic value within 1e-4.
  - size: L

- [ ] M443.P1.T5 — Map channel, compositing and adjustment nodes
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesColour.cpp`, `Tests/Scene/MtlxCyclesBackend_Test.cpp`
  - approach: channel (`extract`, `separateN`, `combineN`, `convert`, `swizzle` forms), compositing (`premult`, `unpremult`, every blend mode, `mix`, `inside`, `outside`, `over`…), adjustment (`remap`, `smoothstep`, `luminance`, `rgbtohsv`, `hsvtorgb`, `contrast`, `range`, `hsvadjust`, `saturate`, `colorcorrect`) and colour-transform nodes, onto `mix`, `separate_color`, `combine_color`, `math` and `vector_math` subgraphs. Nodes MaterialX defines as nodegraphs are left to the core's inlining.
  - verify: gtest — one case per family checks wiring, and a one-pixel numeric check per family matches the analytic value within 1e-4.
  - size: L

- [ ] M443.P1.T6 — Map geometric, texture and application nodes
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesGeometry.cpp`, `Tests/Scene/USDRenderCyclesMtlx_Test.cpp`
  - approach: `position`, `normal`, `tangent`, `bitangent` in object or world space onto `geometry`/`texture_coordinate` plus `vector_transform`. `texcoord` and `geompropvalue` onto `uvmap`/`attribute` with the primvar names the core kept. `geomcolor` onto the display colour attribute. `image`/`tiledimage` onto `image_texture`, with colour space as the OCIO name, address modes onto extension or UV math, and filter type onto interpolation. `normalmap` onto `normal_map`, `bump` onto `bump`. `frame`/`time` become per-frame constants.
  - verify: gtest — a texture on a plane through tiledimage with a UV offset matches its reference crop; a `natron://` image renders; a world-space position node drives colour as expected on a moved object.
  - size: L

- [ ] M443.P1.T7 — Route displacement and volume
  - files: `Engine/Nodes/Scene/Mtlx/MtlxCyclesBackend.cpp`, `Tests/Scene/USDRenderCyclesMtlx_Test.cpp`, reference crops (≤256 px, half-float, zip)
  - approach: `displacement_float` and `displacement_vector3` onto `displacement` and `vector_displacement` on the displacement output. Volume closures onto `principled_volume` or `absorption_volume` plus `scatter_volume` on the volume output.
  - verify: gtest — a displaced plane and a VDB with an `mtlx` volume each match their reference crop; removing the terminal removes the effect.
  - size: M

## Phase 443.2: Proof

- [ ] M443.P2.T1 — Coverage list and pattern parity
  - files: `Tests/Scene/data/mtlx/cycles-unsupported.json`, `Tests/Scene/MtlxParityCycles_Test.cpp`, `Tests/Scene/MtlxCoverage_Test.cpp`
  - approach: register the backend in `MtlxCoverage_Test`; list the procedurals (pending M444 - Cycles MaterialX Procedurals), convolution and any other unsupported nodedef with its reason. Run the parity harness on every translated atomic node, each type variant at defaults and at one non-default input set.
  - verify: `MtlxCoverage_Test` passes with the Cycles list; `MtlxParityCycles_Test` passes for every translated atomic node, or the node is listed with a reason the user accepts.
  - size: L

- [ ] M443.P2.T2 — Viewer check and docs
  - files: `Tests/gui/scene_mtlx_cycles.py`, `Engine/Nodes/Scene/docs/MaterialX.md`
  - approach: a GUI script that opens a `.mtlx` through USDRead, renders it in the progressive viewer with Cycles, edits a parameter and sees the render update. Fill the doc's Cycles section: coverage, unsupported nodes and why, approximated parameters per surface.
  - verify: the script passes under Xvfb at 1920x1080; one screenshot is sent to the user; the doc's unsupported list matches `cycles-unsupported.json`.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDRenderCyclesMtlx_Test`, `MtlxCyclesBackend_Test`, `MtlxParityCycles_Test` and `MtlxCoverage_Test` pass; `scene_mtlx_cycles.py` passes under Xvfb. The user's sign-off on references, screenshots and the unsupported list is collected at the UAT of F26 - MaterialX Rendering.
