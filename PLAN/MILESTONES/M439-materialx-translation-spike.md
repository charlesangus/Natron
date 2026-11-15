# M439 - MaterialX Translation Spike

Prove the mechanism F26 - MaterialX Rendering is built on before anything lands: a renderer-specific Hydra scene index filter that rewrites MaterialX material networks into each renderer's native nodes. Cycles stays without OSL (decision `2026-11-05-no-cycles-osl`), so translation into native nodes is the only route. hdCycles accepts native `cycles_<type>` node ids and remaps UsdPreviewSurface itself (`src/hydra/material.cpp:612` in the Cycles fork), reading only the `cycles` render context (`render_delegate.cpp:380`). hdMoonray creates each rdl2 object straight from the Hydra node id (`lib/hydramoonray/Material.cc:145`), so `ND_*` map shaders from `materialx_shaders` load as they are, and it reads only the `moonray` context (`RenderDelegate.cc:198`). The spike settles the questions below and records them in a design note that M440 - MaterialX Translation Core and the backends build on.

## Scope

**In:**
- A filter registered per renderer through `HdSceneIndexPluginRegistry` that is seen to run for Cycles and MoonRay, in both `USDRender` and the progressive viewer.
- The exact shape of a MaterialX material when it reaches the filter: render contexts, node ids, flattened nodegraphs, connections, colour-space metadata. Shown for MaterialX authored in USD, and for a `.mtlx` file referenced through `usdMtlx`.
- How a MaterialX material reaches a delegate that lists no `mtlx` context: whether the filter can remap the `mtlx` network into the delegate's own context, or the forks must add `mtlx` to `GetMaterialRenderContexts` after their native context.
- Inlining nodegraph-implemented nodedefs through the MaterialX API, and the resulting atomic node set for the standard library.
- A reference evaluator for per-node parity tests, prototyped on one node.
- Whether Storm already renders OpenPBR and Standard Surface through hdSt's MaterialX support in our build.
- A design note with every result, and the user's confirmation of it.

**Out:**
- Landing the translator: M440 - MaterialX Translation Core.
- Building `materialx_shaders`: M441 - MoonRay MaterialX Shaders Build.
- Any backend mapping: M442 - MoonRay MaterialX Backend, M443 - Cycles MaterialX Backend.

**Edge cases:**
- The registry callback never fires under `UsdImagingGLEngine`'s scene-delegate path: the note records it, and the spike tests the alternative of inserting the filter where `HydraRenderImpl` builds the engine, before recommending one.
- USD's MaterialX support is missing from the image (no `usdMtlx` or `hdMtlx`): this stops F26. It is put to the user with what a rebuild of the image's USD would need.

**Assumptions:**
- The image's USD is built with MaterialX (`cmake/NatronUsd.cmake:23` finds it as a USD dependency), so `usdMtlx` and `hdMtlx` exist. P1.T2 confirms.
- hdMoonray builds its rdl2 scene in Natron's process and only rendering runs in `execComp`, so a filter in Natron's process sees MoonRay's materials. P1.T1 confirms.

## Phase 439.1: Prove the mechanism

- [ ] M439.P1.T1 — Run a renderer-specific scene index filter
  - files: scratch filter and registration in `Engine/Nodes/Scene/` (not landed), `Engine/Nodes/Scene/docs/materialx-translation.md`
  - approach: register a pass-through `HdMaterialFilteringSceneIndexBase` subclass with `HdSceneIndexPluginRegistry::RegisterSceneIndexForRenderer` for the display names that `RenderDelegates.cpp:77` reports for Cycles and MoonRay. Register it at startup beside `SceneCaches::registerLiveTextures` (`Engine/AppManager.cpp:700`). Log each material it sees. Run one material through `USDRender` and through the progressive viewer, with both renderers. If it never fires, try inserting the filter where `HydraRenderImpl` creates its engine and record which works.
  - verify: the log shows the filter seeing the material in all four cases (two renderers, USDRender and viewer); the note records the registration that works and the insertion phase used.
  - size: L

- [ ] M439.P1.T2 — Capture MaterialX networks at the filter
  - files: scratch USD and `.mtlx` fixtures under `build/`, `Engine/Nodes/Scene/docs/materialx-translation.md`
  - approach: author an `outputs:mtlx:surface` material in USD that uses an open_pbr_surface fed by an image, a noise3d and a nodegraph. Reference the same material from a `.mtlx` file through `usdMtlx`, and open the `.mtlx` with USDRead. Dump the network the filter receives: contexts, node ids, parameters, connections, `colorSpace` metadata. Record the image's MaterialX version, whether `ND_open_pbr_surface_surfaceshader` exists in it, and whether `usdMtlx` is on the plug-in path in the build tree and the AppImage. Test whether the filter can hand a delegate the `mtlx` network under the delegate's own context, or whether the forks need `mtlx` added to `GetMaterialRenderContexts` after their native one.
  - verify: the note holds the dumped network for both sources and a recommendation on context handling, with the evidence for it.
  - size: L

- [ ] M439.P1.T3 — Inline nodegraph-implemented nodes
  - files: `Tests/Scene/MtlxInlineSpike_Test.cpp` (not landed), `Engine/Nodes/Scene/docs/materialx-translation.md`
  - approach: load the standard libraries with `MaterialX::loadLibraries`, and for each nodedef follow `NodeDef::getImplementation` to its NodeGraph, recursively, until reaching nodedefs that have only per-language implementations. Expand `ND_standard_surface_surfaceshader`, `ND_UsdPreviewSurface_surfaceshader` and `ND_checkerboard_color3` as worked examples. List the resulting atomic nodedefs grouped by category, and count them.
  - verify: the gtest expands the three examples to atomic nodes only; the note lists the atomic set with its count.
  - size: M

- [ ] M439.P1.T4 — Prototype a reference evaluator
  - files: scratch script under `tools/` (not landed), `Engine/Nodes/Scene/docs/materialx-translation.md`
  - approach: find whether the image has OSL's `testshade` and MaterialX's OSL source libraries. If it does, generate OSL for one atomic node (`ND_noise3d_float`) with MaterialX's OSL generator and evaluate it on a 64×64 grid of positions into a float EXR. If not, use MaterialXRender's GLSL path offscreen under Xvfb. Record the commands, run time, and what each route needs.
  - verify: a 64×64 float EXR of noise3d over a known position grid exists, and the note states the chosen route and how a test would call it.
  - size: M

- [ ] M439.P1.T5 — Check Storm's MaterialX path
  - files: scratch USD under `build/`, `Engine/Nodes/Scene/docs/materialx-translation.md`
  - approach: render an open_pbr_surface and a standard_surface material on `outputs:mtlx:surface` through `USDRender` with Storm, and in the 3D viewer under Xvfb (recipe in `build/deeprepro/run-gui.sh`).
  - verify: the note records whether both render with their base colour and specular, with one Xvfb screenshot each sent to the user as separate files.
  - size: M

- [ ] M439.P1.T6 — Finish the design note
  - files: `Engine/Nodes/Scene/docs/materialx-translation.md`
  - approach: bring T1–T5's results together: registration and insertion point, context handling (filter remap, or the fork change each backend makes), atomic set, reference evaluator, Storm status. Add an outline of M440 - MaterialX Translation Core's classes. Put any result that changes F26's plan to the user.
  - verify: the note answers every question in this milestone's Scope **In** list, and the user has confirmed it.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; the design note `Engine/Nodes/Scene/docs/materialx-translation.md` answers every Scope **In** question; the user has read it and confirmed the design before M440 - MaterialX Translation Core starts. Only the design note lands; scratch code is not merged.
