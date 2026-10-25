# M127 - Native OCIO Nodes

Native versions of Nuke's OCIO node set replace openfx-io's OCIO plug-ins: `OCIOColorSpace`, `OCIODisplay`, `OCIOCDLTransform`, `OCIOFileTransform` and `OCIOLookTransform`. They share an OCIO node base built on the project colour management from M50 - Project OCIO Support and the space knobs from M75 - Native Read and M76 - Native Write. OCIOLogConvert is folded into OCIOColorSpace as a role preset.

Each node runs on the CPU through `CPUProcessor::apply` per row. On the GPU, each node takes OCIO's generated `GPU_LANGUAGE_GLSL_VK_4_6` shader and compiles it at runtime on M126 - Runtime GPU Shaders. LUT textures and uniforms are bound through that milestone's descriptors, and dynamic properties such as exposure are uniforms.

Deps: M120 - Native Node Port Kit, M126 - Runtime GPU Shaders and M107 - GPU Viewer Hand-off (its shared OCIO→Vulkan helper).

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **OFX sources:** `build/assets/plugin-src/openfx-io/OCIO/`, with the shared helper `IOSupport/GenericOCIO.{h,cpp}` (1740 lines) and `GenericOCIOOpenGL.cpp`.
  - **OCIOColorSpace** (`fr.inria.openfx.OCIOColorSpace`, `OCIOColorSpace.cpp:54`): input and output space, plus Context.
  - **OCIODisplay** (`OCIODisplay.cpp:56`): `ocioInputSpace` (default `ROLE_REFERENCE`, `:1173`), display, view, `gain` (`:1242`), `gamma` (`:1256`), `channelSelector` and Context. It uses `DisplayViewTransform` (`:580, :665`) and `getDefaultLumaCoefs` (`:652`).
  - **OCIOCDLTransform** (`OCIOCDLTransform.cpp:63`): slope, offset, power, saturation and direction, plus `readFromFile`, `file`, `cccId` and `export`. `CDLTransform::Create`/`CreateFromFile` are at `:564, :1032`.
  - **OCIOFileTransform** (`OCIOFileTransform.cpp:69`): `file`, `cccId`, direction and interpolation (`:489-515`).
  - **OCIOLogConvert** (`OCIOLogConvert.cpp:54`): `scene_linear` ↔ `compositing_log` roles (`:441-449`).
  - **OCIOLookTransform** (`OCIOLookTransform.cpp:66`): spaces, look choice, append, `singleLook` (`:1009-1011`), combination, direction and Context. It uses `LookTransform` at `:501-518`.
  - **Shared plumbing:**
    - The `GenericOCIO` knob pattern is `ocioInputSpace` + `…Index` + hidden `…Set`, Context `key1..4`/`value1..4`, and hidden `ocioConfigFile`/`ocioWorkingSpace` (`GenericOCIO.h:80-120`).
    - The CPU path is at `GenericOCIO.cpp:968`.
    - The GL path is `GenericOCIOOpenGL.cpp:218-223`, using `GPU_LANGUAGE_GLSL_1_2`.
- **Native infrastructure:**
  - **`Engine/Nodes/IO/OcioSpaceKnobs.{h,cpp}`:**
    - `create(holder, page, Description, AutomaticSpace, ChoiceMade)` builds the whole GenericOCIO set for one space. Its members include `knobChanged`, `refreshMenu`, `space()`, `workingSpace()` and `contextVariables(time)`.
    - `NativeRead.cpp:370-389, 865-871` is the usage example.
  - **`Engine/Nodes/IO/IoColorSpace.{h,cpp}`:**
    - `processor(project, from, to, ctx, &cpu, &err)` (`.cpp:~193`) returns null when no conversion is needed.
    - `convertRow` (`:249`) handles RGB and leaves alpha alone.
    - `nameInConfig` resolves names.
  - **`Engine/ProjectColorManagement.{h,cpp}`:**
    - `getConversionProcessor` (`:732`) is cached per (src, dst, ctx); the cache clears on config load (`:501`).
    - `getDisplayProcessor` (`:642`) has no gain or gamma.
    - Listing helpers: `getColorSpaces`, `getRoles`, `getDisplays`, `getViews`, `getLooks` and `getWorkingLuma` (`.h:114-169`).
    - Config-change callbacks: `addConfigChangedCallback`/`removeConfigChangedCallback` (`:1017-1034`), fired from `Project.cpp:1707, 2378`.
  - **Config push:** `Project::pushOCIOConfigToNode(s)` (`Engine/Project.cpp:343-364`) writes the hidden `ocioConfigFile`/`ocioWorkingSpace` knobs. Their names are hard-coded at `Project.cpp:300-302`, along with `ocioInputSpace` and `ocioOutputSpace`.
- **References:**
  - Docs: `Engine/NodeDocumentation.cpp:48-61, 163-170, 229-231` (hides display, view and cccId). `Gui/DocumentationManager.cpp:161-164, 179` (OCIO `.rst` pages, `startsWith("fr.inria.openfx.OCIO")`) and `:65` (the group "Color/OCIO").
  - Tests: `Tests/ProjectOCIO_Test.cpp:65-69, 288+`, `Tests/ProjectOCIOPlugins_Test.cpp:46, 72` and `Tests/GLScheduler_Test.cpp:69, 296`.
  - Not referenced by PyPlugs, menus or the viewer.
- **OCIO version:** 2.5 (`Engine/CMakeLists.txt:31`).

## Scope

**In:**
- An OCIO node base: space knobs, processor caching through `ProjectColorManagement`, Context variables, and refresh on config change.
- Native `OCIOColorSpace` (with a LogConvert role preset), `OCIODisplay` (display, view, gain, gamma, channel view), `OCIOCDLTransform` (inline values or a file/CCC id, export), `OCIOFileTransform` and `OCIOLookTransform`.
- CPU per-row processing.
- GPU through runtime-compiled OCIO shaders on M126 - Runtime GPU Shaders.
- Docs and tests updated.
- The six openfx-io OCIO IDs retired, and the dead GenericOCIO take-over pieces removed.

**Out:**
- The runtime compiler, descriptors and pipeline cache: M126 - Runtime GPU Shaders.
- Viewer display transform on Vulkan: M107 - GPU Viewer Hand-off.
- Native log conversion with custom Cineon/Pines parameters: none. Log2Lin and PLogLin are dropped (user, 2026-10-13 grill of M122 - Native Colour Utilities).
- A native CDL inside Grade: none.

**Edge cases:**
- **The project config changes or fails to load:** every OCIO node refreshes its menus. A node whose space no longer exists shows a persistent error and passes the input through.
- **The identity transform** (same space in and out, or a CDL at defaults): reports identity, and no processor is built.
- **A processor whose GPU shader fails to compile:** the node falls back to the CPU through M126 - Runtime GPU Shaders's fallback.
- **Alpha:** processed as RGB only. Alpha passes through, and mask/mix/premult come from the base.
- **A CDL or file transform with a missing file:** persistent error, input passed through. The reload button re-reads the file.
- **Two space knobs on one node:** the second `OcioSpaceKnobs` instance must not create a second hidden `ocioConfigFile` or `ocioWorkingSpace`.

**Assumptions:**
- Unconfirmed: the knob script names follow Nuke's OCIO nodes (`in_colorspace`, `out_colorspace`, `display`, `view`, `slope`, `offset`, `power`, `saturation`, `file`, `cccid`, `direction`, `interpolation`, `look`). `Project.cpp:300-302`'s by-name config push moves to the hidden knobs only.
- Unconfirmed: OCIODisplay's gain and gamma, and the channel selector, are applied as uniforms around the display processor, as the viewer does, so dynamic changes don't recompile the shader.
- Unconfirmed: OCIO's dynamic properties (exposure, contrast, gamma) are bound as UBO members and not baked into the shader hash.

## Phase 126.1: OCIO node base

- [x] M127.P1.T1 — Shared OCIO node base with space knobs, processor and config refresh
  - files: `Engine/Nodes/OCIO/OcioNodeBase.{h,cpp}` (new), `Engine/Nodes/IO/OcioSpaceKnobs.{h,cpp}`, `Engine/Project.cpp`, `Tests/Native/NativeOcioBase_Test.cpp` (new)
  - approach:
    - A `NativeImageEffect` subclass whose RGB-only kernel calls an `OCIO::CPUProcessor` per row through `IoColorSpace::convertRow`.
    - It builds one or two `OcioSpaceKnobs` instances, keeping a single pair of hidden config knobs (check `Project.cpp:300-364`). It also owns the Context knobs.
    - Processors come from `ProjectColorManagement` and are cached by a key of transform plus context.
    - It subscribes to `addConfigChangedCallback` to refresh menus and invalidate the cache.
  - verify: tests cover a two-space instance (one hidden config pair), a processor rebuild after a config change, and identity for equal spaces. Runs at the phase build.
  - size: L

## Phase 126.2: Nodes on the CPU

- [x] M127.P2.T1 — Native OCIOColorSpace with the LogConvert role preset
  - files: `Engine/Nodes/OCIO/OCIOColorSpace.{h,cpp}` (new), `Engine/Nodes/OCIO/OCIOColorSpace.reg.inc` (new), `Tests/Native/NativeOCIOColorSpace_Test.cpp` (new)
  - approach:
    - Input and output space, plus Context.
    - A "preset" menu sets the spaces to the `scene_linear`/`compositing_log` roles in either direction, replacing OCIOLogConvert.
  - verify: tests check pixels against a direct OCIO processor for two spaces and for both log presets. Runs at the phase build.
  - size: M

- [x] M127.P2.T2 — Native OCIODisplay
  - files: `Engine/Nodes/OCIO/OCIODisplay.{h,cpp}` (new), `Engine/Nodes/OCIO/OCIODisplay.reg.inc`, `Tests/Native/NativeOCIODisplay_Test.cpp` (new)
  - approach:
    - Input space, display, view, gain, gamma and the channel view (RGB, R, G, B, A, luminance via `getWorkingLuma`).
    - It uses `getDisplayProcessor`, with gain applied before it and gamma after, as the viewer does.
  - verify: tests check pixels against OCIO for the default display and view, with gain and gamma applied. Runs at the phase build.
  - size: M

- [x] M127.P2.T3 — Native OCIOCDLTransform and OCIOFileTransform
  - files: `Engine/Nodes/OCIO/OCIOCDLTransform.{h,cpp}` (new), `Engine/Nodes/OCIO/OCIOFileTransform.{h,cpp}` (new), `Engine/Nodes/OCIO/{OCIOCDLTransform,OCIOFileTransform}.reg.inc`, `Tests/Native/NativeOCIOFileTransforms_Test.cpp` (new)
  - approach:
    - CDL takes inline slope, offset, power and saturation, plus direction. Optionally it reads from a `.cc`/`.ccc`/`.cdl` file with a CCC-id menu, and an export button writes a `.cc`.
    - FileTransform takes file, CCC id, direction and interpolation. Both have a reload button.
  - verify: tests check a CDL against OCIO's `CDLTransform` and a `.cube` file transform against OCIO. A missing file gives an error and passes the input through. Runs at the phase build.
  - size: M

- [x] M127.P2.T4 — Native OCIOLookTransform
  - files: `Engine/Nodes/OCIO/OCIOLookTransform.{h,cpp}` (new), `Engine/Nodes/OCIO/OCIOLookTransform.reg.inc`, `Tests/Native/NativeOCIOLookTransform_Test.cpp` (new)
  - approach: input and output space, a look menu from `getLooks` with an append/combination string, direction, and Context, built on `LookTransform`.
  - verify: tests check pixels against OCIO for a look from the test config, forward and inverse. Runs at the phase build.
  - size: M

## Phase 126.3: GPU

- [ ] M127.P3.T1 — Generate and bind OCIO's Vulkan shader in the base
  - files: `Engine/Nodes/OCIO/OcioNodeBase.{h,cpp}`, `Engine/OcioVulkanShader.{h,cpp}` (from M107 - GPU Viewer Hand-off, extended only if the node path needs it), `Tests/Native/NativeOcioGpu_Test.cpp` (new)
  - approach:
    - Reuse M107 - GPU Viewer Hand-off's shared helper `Engine/OcioVulkanShader.{h,cpp}` (created by M107.P3.T1). It builds OCIO's `GPU_LANGUAGE_GLSL_VK_4_6` shader, holds LUTs as `GpuTexture`s and dynamic properties in a UBO, and caches LUT textures per processor on `cacheID`. Do not write a second OCIO shader generator.
    - Wrap the helper's OCIO function in a compute entry that reads and writes the storage-buffer images.
    - Supply the result through M126 - Runtime GPU Shaders's runtime-shader `GpuQuery` hook. The shader hash excludes dynamic values.
  - verify: the GPU parity harness matches the CPU within 1e-4 for ColorSpace (an ACES config), Display, CDL, a `.cube` FileTransform and a Look, on RADV. Changing exposure doesn't trigger a recompile. Runs at the phase build.
  - size: L

- [ ] M127.P3.T2 — Cost seeds and placement for the OCIO nodes
  - files: `Engine/Nodes/OCIO/{OCIOColorSpace,OCIODisplay,OCIOCDLTransform,OCIOFileTransform,OCIOLookTransform}.costseed.inc` (new), `Tests/GpuPlacement_Test.cpp`
  - approach:
    - Seed GPU and CPU cost rows for the five nodes.
    - Check that Read → OCIOColorSpace → Grade → OCIODisplay places as one region.
  - verify: the placement test shows one region with no download between members. Runs at the phase build.
  - size: M

## Phase 126.4: References and retirement

- [x] M127.P4.T1 — Update the documentation hooks for the native OCIO nodes
  - files: `Engine/NodeDocumentation.cpp`, `Gui/DocumentationManager.cpp`, `Engine/Nodes/OCIO/docs/{OCIOColorSpace,OCIODisplay,OCIOCDLTransform,OCIOFileTransform,OCIOLookTransform}.md` (new, per M120 - Native Node Port Kit)
  - approach:
    - Point the hidden-knob and `.rst` mapping (`NodeDocumentation.cpp:48-61, 163-170, 229-231`; `DocumentationManager.cpp:161-164, 179, 65`) at the native IDs, and drop the `fr.inria.openfx.OCIO` prefix check.
    - Document the node set.
  - verify: at the phase build, each OCIO node's documentation page renders in the GUI. Check under Xvfb.
  - size: M

- [x] M127.P4.T2 — Retire the openfx-io OCIO plug-ins and repoint tests
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (six new markers), `Tests/ProjectOCIO_Test.cpp`, `Tests/ProjectOCIOPlugins_Test.cpp`, `Tests/GLScheduler_Test.cpp`, `tools/ci/local/fetch-assets.sh` (comments at `:120-121`)
  - approach:
    - Add an `Engine/OfxRetired/<ofx-plugin-id>` marker for each of the six `fr.inria.openfx.OCIO*` IDs.
    - Repoint the tests at the native nodes, and delete cases that only tested OFX config hand-off.
    - Remove GenericOCIO take-over code left dead once no OCIO OFX node loads (the `ocioInputSpace` by-name branches in `Project.cpp:300-302`, if unused).
  - verify: at the phase build, ctest passes, the retired IDs don't load, and `git grep fr.inria.openfx.OCIO` finds only the `Engine/OfxRetired/` markers.
  - size: M

**Verification gate:**
- In one build, ctest passes, including GPU parity for all five nodes on RADV.
- An Xvfb GUI check passes: switch the project config to ACES and see menus refresh; chain ColorSpace → Display; load a CDL from a file. Screenshots are shared and OK'd before sign-off.
- An AppImage from `package.sh` is handed to the user for UAT.
- The six retired OFX IDs don't load.

## Decisions

- 2026-10-25 — **GPU contract departures (user):** agreed as proposed. (1) The OCIO shader's source and destination buffers are found by reserved name or declared binding, not bindings 0/1, since OCIO puts its uniform block at set 0 binding 0. (2) `GpuShaderTexture` gains an optional cache key; the GPU context keeps device textures by key, shared by the viewer and nodes. (3) CDL slider changes may compile new pipelines; "no recompile" is limited to OCIODisplay's gain, gamma and channel.
- 2026-10-13 — **Nuke's node set, with LogConvert folded into OCIOColorSpace as a role preset** (user).
- 2026-10-13 — **GPU through runtime compilation of OCIO's own shader** (user). This was chosen over translating ops into precompiled kernels or baking a shaped 3D LUT. The compiler and descriptor work is M126 - Runtime GPU Shaders.
- 2026-10-19 — **Reuse M107 - GPU Viewer Hand-off's `Engine/OcioVulkanShader` helper** (gpu PM, via inbox): the viewer and the nodes then share one OCIO shader generator and LUT cache. M107 joins Deps.
