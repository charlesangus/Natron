# M440 - MaterialX Translation Core

The renderer-neutral half of F26 - MaterialX Rendering: a Hydra scene index filter, registered for each renderer per M439 - MaterialX Translation Spike's design note, that takes each MaterialX material network and hands the renderer a native one. The core reads the network into a MaterialX document, normalises it, and inlines every nodegraph-implemented node until only the backend's atomic set is left. It then maps those nodes through a backend table and routes the surface, displacement and volume terminals. A material it can't translate whole renders flat magenta with a warning. Backends come later: M442 - MoonRay MaterialX Backend and M443 - Cycles MaterialX Backend. Re-check this brief against the design note at claim (§5a): class names and the insertion point follow it.

## Scope

**In:**
- `Engine/Nodes/Scene/Mtlx/`: the translator, the backend interface and the filtering scene index, registered for each renderer that has a backend.
- Inlining of every nodedef implemented as a nodegraph, the standard libraries and custom libraries found on USD's `PXR_MTLX_PLUGIN_SEARCH_PATHS` alike.
- Terminal routing for `surfaceshader`, `displacementshader` and volume closures.
- Colour: constant colours tagged with a colour space are converted into the project's OCIO working space. Texture colour spaces pass to the backend as OCIO names.
- Diagnostics: an untranslatable material renders flat magenta and gets a warning naming the node and material. Approximated parameters set away from their default get an info note per material.
- `.mtlx` files: opened by USDRead and referenced from USD layers through `usdMtlx`, in the build tree and the AppImage.
- A coverage test and a reference-parity harness for the backends to fill in.
- User docs for MaterialX support.

**Out:**
- Backend mapping tables: M442 - MoonRay MaterialX Backend, M443 - Cycles MaterialX Backend.
- Storm: keeps USD's own hdSt MaterialX path, untouched.
- OSL: none (decision `2026-11-05-no-cycles-osl`).
- Authoring MaterialX in Natron: M445 - USDOpenPBR Node, M446 - MaterialX Builder Nodes.

**Edge cases:**
- A material with both a native network (`outputs:cycles:surface`, `outputs:moonray:surface`) and an `mtlx` one: the native network wins in its renderer and the filter leaves the material untouched.
- A colour space that OCIO can't resolve: an error naming it, and the material falls back to magenta, as project OCIO rules require.
- A custom nodedef with no nodegraph implementation and no backend mapping: untranslatable, so it falls back with a warning.
- A MaterialX document from an older MaterialX version: MaterialX's own upgrade on load applies. Nothing extra.
- Animated parameters and edits: only dirtied materials are re-translated. Unchanged networks come from a cache keyed by the network's hash, so an edit never re-translates every material.
- Many materials (thousands in a heavy scene): translation is per material and on demand, so the cost tracks the number of materials Hydra syncs.

**Assumptions:**
- M439 - MaterialX Translation Spike found a working registration point and context handling. This brief's P2 follows whatever its note recorded.

## Phase 440.1: Translator

- [ ] M440.P1.T1 — Read, normalise and inline networks
  - files: `Engine/Nodes/Scene/Mtlx/MtlxTranslator.{h,cpp}`, `Engine/CMakeLists.txt`, `cmake/NatronUsd.cmake`, `Tests/Scene/MtlxTranslator_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: link `hdMtlx`, `usdMtlx`, `MaterialXCore` and `MaterialXFormat`. Build a MaterialX document from an `HdMaterialNetworkInterface` with `hdMtlx`'s helpers, load the standard libraries plus `PXR_MTLX_PLUGIN_SEARCH_PATHS` once per process, and resolve each node to its nodedef. Inline nodegraph implementations recursively until every node is in the atomic set the backend declares, keeping input defaults and `geompropvalue`/`texcoord` primvar names. A backend may also declare nodes it maps directly (main surface shaders) so they are not inlined.
  - verify: gtest — standard_surface inlines to atomic nodes when the test backend doesn't map it, and stays whole when it does; a custom nodegraph nodedef in a temporary search path inlines; a node with no implementation is reported untranslatable.
  - size: L

- [ ] M440.P1.T2 — Map nodes through a backend table
  - files: `Engine/Nodes/Scene/Mtlx/MtlxBackend.{h,cpp}`, `Engine/Nodes/Scene/Mtlx/MtlxTranslator.cpp`, `Tests/Scene/MtlxTranslator_Test.cpp`
  - approach: a `MtlxBackend` interface with the atomic set, direct-mapped nodes, a per-node mapping (target node id, input renames, scale, swizzle, constants, and an `expand` hook for a node that becomes a small native subgraph), terminal output names for surface, displacement and volume, and a `fallback()` network. The translator writes the mapped network back through `HdMaterialNetworkInterface`. An unmapped node marks the material untranslatable. A mapping flagged approximate records the parameter when its value differs from the nodedef default.
  - verify: gtest with a test backend — a two-node graph maps with renamed inputs and an `expand` subgraph; an unmapped node yields the fallback network and a diagnostic naming it; an approximate parameter at default records nothing, and away from default records one note.
  - size: L

- [ ] M440.P1.T3 — Convert colour into the working space
  - files: `Engine/Nodes/Scene/Mtlx/MtlxColour.{h,cpp}`, `Engine/Nodes/Scene/Mtlx/MtlxTranslator.cpp`, `Tests/Scene/MtlxColour_Test.cpp`
  - approach: resolve each colour value's colour space (its own, else its nodegraph's, else the document's) and convert constants into the project's OCIO working space through the project config (decision `2026-10-02-project-ocio-full-replacement`). Map MaterialX colour-space names to the config's names. Image nodes keep their space as an OCIO name for the backend. A name OCIO can't resolve marks the material untranslatable with an error naming it.
  - verify: gtest — an `srgb_texture` constant converts to the working space within 1e-5 of OCIO's own transform; a `lin_rec709` constant in an ACEScg working space converts; an unknown space gives the fallback and an error.
  - size: M

## Phase 440.2: Hydra

- [ ] M440.P2.T1 — Filter materials per renderer
  - files: `Engine/Nodes/Scene/Mtlx/MtlxSceneIndex.{h,cpp}`, `Engine/AppManager.cpp`, `Tests/Scene/MtlxSceneIndex_Test.cpp`
  - approach: an `HdMaterialFilteringSceneIndexBase` subclass per M439 - MaterialX Translation Spike's note, registered for each renderer that has a registered backend, at the insertion point the note chose. It skips materials that have a native network for that renderer, translates the `mtlx` network otherwise, and presents it under the context the note settled on. It handles dirtiness: only dirtied material prims re-translate, with results cached by network hash. Translation is safe to run from several sync threads.
  - verify: gtest on a retained scene index with a test backend — an `mtlx` material comes out mapped; a material with a native network is untouched; editing one parameter re-translates that material only (counted); two threads translating different materials agree with a serial run.
  - size: L

- [ ] M440.P2.T2 — Report fallbacks and approximations
  - files: `Engine/Nodes/Scene/Mtlx/MtlxDiagnostics.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/DelegateSupport.cpp`, `Tests/Scene/MtlxSceneIndex_Test.cpp`
  - approach: the filter writes each material's diagnostics to a per-render-index sink. `HydraRenderImpl` reads it after each sync and shows warnings in the same place as `DelegateSupport`'s lines (`DelegateSupport.cpp:256`): "<renderer> cannot translate MaterialX node <id> in materials: …" as a warning, approximations at info level. A material that translates cleanly on a later edit clears its line.
  - verify: gtest — an untranslatable material yields one warning line naming node and material; fixing it clears the line on the next sync; an approximation yields an info line only.
  - size: M

- [ ] M440.P2.T3 — Open and reference `.mtlx` files
  - files: `Engine/Nodes/Scene/USDRead.cpp`, `tools/ci/local/stage-bundle.sh`, `Tests/Scene/USDReadMtlx_Test.cpp`, small `.mtlx` and `.usda` fixtures under `Tests/Scene/data/`
  - approach: make sure `usdMtlx`'s file format plug-in is on the plug-in path in the build tree and the AppImage. Add `.mtlx` to USDRead's accepted extensions and file dialog filter. A `.mtlx` opened directly becomes a stage with its materials under `/MaterialX`.
  - verify: gtest — USDRead opens a `.mtlx` and lists its material prims; a `.usda` referencing the same `.mtlx` composes the material; both render with Storm without error.
  - size: M

## Phase 440.3: Test harnesses and docs

- [ ] M440.P3.T1 — Coverage test per backend
  - files: `Tests/Scene/MtlxCoverage_Test.cpp`, `Tests/Scene/data/mtlx/`
  - approach: walk every nodedef in the loaded libraries and, for each registered backend, decide whether it translates (directly or after inlining). Write a per-backend report under the build tree. Each backend has a committed known-unsupported JSON listing nodedefs with a reason; the test fails on any unsupported nodedef not in the list, and on a listed one that now translates. Lands with the test backend only; M442 - MoonRay MaterialX Backend and M443 - Cycles MaterialX Backend register theirs.
  - verify: gtest passes for the test backend with its list; removing one entry from the list makes it fail naming that nodedef.
  - size: M

- [ ] M440.P3.T2 — Reference-parity harness
  - files: `Tests/Scene/MtlxParity.{h,cpp}`, `Tests/Scene/MtlxParity_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: wrap M439 - MaterialX Translation Spike's reference evaluator: for an atomic nodedef and input values, evaluate it over a fixed grid of positions and UVs into a float image. Add a helper that renders the same node through `USDRender` as an unlit card's emission at matching positions, and compares the two with a per-node tolerance. Here it is tested only on the reference side; the backends call it.
  - verify: gtest — the reference side evaluates noise3d and add_color3 deterministically twice in a row, and an add_color3 of known constants matches its analytic value.
  - size: M

- [ ] M440.P3.T3 — Document MaterialX support
  - files: `Engine/Nodes/Scene/docs/MaterialX.md`, `Engine/Nodes/Scene/docs/USDRead.md`
  - approach: user docs — how MaterialX materials reach Cycles and MoonRay, native-wins precedence, colour handling, the magenta fallback and its warning, info notes for approximations, `.mtlx` input, custom libraries through `PXR_MTLX_PLUGIN_SEARCH_PATHS`. Leave per-renderer coverage sections for the backends to fill.
  - verify: the doc covers each Scope **In** item, and every behaviour it states is exercised by one of this milestone's tests.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `MtlxTranslator_Test`, `MtlxColour_Test`, `MtlxSceneIndex_Test`, `USDReadMtlx_Test`, `MtlxCoverage_Test` and `MtlxParity_Test` pass. User acceptance is at the UAT of F26 - MaterialX Rendering.

## Decisions

- 2026-11-15 — **Untranslatable material falls back whole, flat magenta (user, grilling):** a half-translated look must never pass as correct; the warning names the node and material.
- 2026-11-15 — **Native network wins over `mtlx` (user):** USD's render-context convention; MaterialX is used where a renderer has no network of its own.
- 2026-11-15 — **`.mtlx` files in this milestone (user):** imported libraries render from the first milestone that renders anything.
- 2026-11-15 — **Colour converts into the working space (user):** constants convert through project OCIO, texture spaces pass as OCIO names, unknown spaces are errors.
- 2026-11-15 — **Custom libraries through `PXR_MTLX_PLUGIN_SEARCH_PATHS` (user):** no new preference.
- 2026-11-15 — **Surface, displacement and volume terminals (user).**
- 2026-11-15 — **Approximations render with an info note (user):** the fallback is reserved for nodes with no translation.
