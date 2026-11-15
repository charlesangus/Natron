# M281 - Utility Materials And Lights

Give Natron Utility real shading: UsdPreviewSurface materials with textures (including M233 -
Live Node Textures' `natron://` images), native evaluation of M234 - Camera Projection Nodes'
`USDProject3D`, and every UsdLux light type with shaping, IES, light and shadow linking and
ray-traced shadows. Direct lighting only, no GI. The flat stopgap from M279 - Utility Renderer
Foundation is replaced: a scene with no lights renders emission only.

## Scope

**In:**
- Material sync: `HdMaterial` networks parsed into a compact per-material record for the UsdPreviewSurface subset — diffuseColor, emissiveColor, specularColor / useSpecularWorkflow, metallic, roughness, clearcoat, clearcoatRoughness, opacity, opacityThreshold, ior, normal; UsdUVTexture (wrap, scale, bias, fallback, sourceColorSpace), UsdPrimvarReader_float2/float3, UsdTransform2d.
- Textures loaded through `HioImage` (so `natron://` live textures, UDIM tiles and file textures share one path), mip-mapped, bilinear/trilinear filtered; GPU copies in a texture pool.
- `USDProject3D` recognised by its shader id and evaluated natively in Slang with the same parameters (camera matrix, clip, cull, outside mode) as its Storm glslfx.
- Lights: Distant (angle → soft shadows), Sphere, Rect, Disk, Cylinder, Dome (lat-long HDRI, importance-sampled); intensity, exposure, color, colorTemperature/enableColorTemperature, normalize; ShapingAPI (cone angle, softness, focus, IES profile); light-link and shadow-link collections.
- Ray-traced shadows through the tracer's occlusion query; one light sample per light per camera sample (M283 - Utility Renderer Sampling And Motion Blur adds the samples).
- Shading model: diffuse + GGX specular per the UsdPreviewSurface spec, evaluated in shared Slang on both backends.

**Out:**
- Reflection and refraction rays, transmissive shadows — M282 - Utility Reflection And Refraction.
- Indirect diffuse / GI, caustics, subsurface — none.
- MaterialX and other renderers' shaders — none (rendered magenta, see edge cases).
- Light-group AOVs — M285 - Utility Renderer AOVs.

**Edge cases:**
- Unsupported material (MaterialX, Cycles/MoonRay-only, arbitrary glslfx): rendered magenta, and one node warning lists the unsupported material paths (user, 2026-11-01).
- No lights in the scene: unlit — only emissive surfaces and the Dome's background show (user, 2026-11-01).
- Missing texture file or unresolved `natron://` URI: the UsdUVTexture `fallback` value, plus a node warning naming the asset; the render does not fail.
- Texture larger than GPU texture limits or memory: downsampled to the device limit with a warning; on out-of-memory the M280 CPU fallback applies.
- Missing `st` primvar on a textured prim: texture lookups use the fallback and the prim is named in a warning.
- Light linking collections that include nothing: the light illuminates nothing (USD semantics), no warning.
- Dome light with no texture: constant colour environment.

**Assumptions:**
- `USDProject3D`'s shader can be identified unambiguously (its `info:id` or `sourceAsset` path) and its inputs carry everything the projection needs; read M234's implementation before building.
- `HioImage` reads UDIM tiles through `<UDIM>` paths in the pinned USD as Storm does.

## Phase 281.1: Materials and textures

- [x] M281.P1.T1 — Material network sync to compact records
  - files: `Engine/Nodes/Scene/Utility/UtilityMaterial.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Engine/Nodes/Scene/Utility/Kernels/UtilityTypes.slang`, `Tests/Scene/NatronUtilityMaterial_Test.cpp`
  - approach: add the `material` Sprim; walk `HdMaterialNetwork2` from the surface terminal, accept only the UsdPreviewSurface subset nodes listed in Scope and fold them into a fixed-layout record (constants plus texture slot indices). Anything else marks the material unsupported. Mesh rprims resolve their material binding id each sync.
  - verify: gtest on authored materials — constants land in the record; a texture-driven diffuse gets a slot; a MaterialX network is flagged unsupported.
  - size: L

- [x] M281.P1.T2 — Texture pool through HioImage
  - files: `Engine/Nodes/Scene/Utility/UtilityTextures.{h,cpp}`, `Engine/Nodes/Scene/Utility/Kernels/Texture.slang`, `Tests/Scene/NatronUtilityMaterial_Test.cpp`
  - approach: load each asset once via `HioImage` (UDIM expansion as Storm does), build mips, keep CPU copies for the C++ path and `GpuTexture`s for the GPU path; filtered lookup with wrap/scale/bias in shared Slang. Live `natron://` assets reload when their `?v=` hash changes.
  - verify: gtest — a 4×4 known texture samples to expected values at texel centres on both backends; a `natron://` texture served by the live-texture store loads; a missing file returns the fallback and records a warning.
  - size: L

- [x] M281.P1.T3 — PreviewSurface shading and the magenta fallback
  - files: `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/PreviewSurface.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Tests/Scene/NatronUtility_Test.cpp`
  - approach: replace ShadeFlat with ShadeSurface: evaluate the material record (diffuse + GGX specular, metallic/specular workflows, opacityThreshold cutout, normal map) and emission; no lights yet means emission only. Unsupported materials shade magenta (1,0,1) and the pass collects their paths into one node warning.
  - verify: gtest — an emissive quad renders its emission; a non-emissive quad in a lightless scene renders black; a MaterialX-bound quad renders magenta and the warning names it; CPU and GPU agree within tolerance.
  - size: M

- [x] M281.P1.T4 — Native USDProject3D evaluation
  - files: `Engine/Nodes/Scene/Utility/UtilityMaterial.cpp`, `Engine/Nodes/Scene/Utility/Kernels/Project3D.slang`, `Tests/Scene/NatronUtilityProject3D_Test.cpp`
  - approach: read M234's USDProject3D implementation for its shader id and inputs; recognise it in material sync and fold its parameters into a projection record; `Project3D.slang` projects the hit's world position through the camera matrix and applies clip, back-face cull and outside mode, sampling the live texture.
  - verify: gtest — a plate projected onto a card from the render camera reproduces the plate within tolerance; Natron Utility's frame matches Storm's for M234's test scene on interior pixels.
  - size: L

## Phase 281.2: Lights and shadows

- [x] M281.P2.T1 — Light sprims and light sampling
  - files: `Engine/Nodes/Scene/Utility/UtilityLight.{h,cpp}`, `Engine/Nodes/Scene/Utility/Kernels/Lights.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Tests/Scene/NatronUtilityLights_Test.cpp`
  - approach: support the six UsdLux sprims; sync intensity, exposure, colour, temperature, normalize and shaping (cone, softness, focus, IES via `HioImage`/an IES parser if the pinned USD has one, else a small in-tree parser). Lights.slang samples a point on each light, returns radiance and pdf; the Dome builds a CDF for importance sampling.
  - verify: gtest — a distant light at normal incidence on a white diffuse quad gives the analytic irradiance/π; a spot cone cuts off at its angle; an IES profile scales intensity by its table.
  - size: L

- [x] M281.P2.T2 — Shadow rays and direct lighting in ShadeSurface
  - files: `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Engine/Nodes/Scene/Utility/WavefrontCpu.cpp`, `Engine/Nodes/Scene/Utility/WavefrontGpu.cpp`, `Tests/Scene/NatronUtilityLights_Test.cpp`
  - approach: the shading stage emits one shadow ray per light into an occlusion batch; a second shading stage accumulates unoccluded light contributions. Wavefront stays backend-agnostic.
  - verify: gtest — a cube on a ground plane under a distant light casts a shadow whose interior is black and whose lit region matches the analytic value; CPU and GPU agree within tolerance.
  - size: L

- [x] M281.P2.T3 — Light and shadow linking
  - files: `Engine/Nodes/Scene/Utility/UtilityLight.cpp`, `Engine/Nodes/Scene/Utility/UtilityMesh.cpp`, `Engine/Nodes/Scene/Utility/Kernels/Lights.slang`, `Tests/Scene/NatronUtilityLights_Test.cpp`
  - approach: resolve each light's light-link and shadow-link collections (the pinned USD's light-linking scene index / `HdLightTokens` collection names; cite them) into per-instance bitmasks; lighting skips unlinked lights and shadow rays ignore unlinked casters.
  - verify: gtest — a light linked to one of two spheres lights only it; a shadow-link exclusion removes one sphere's shadow while it still receives light.
  - size: M

**Verification gate:** full build and ctest green on both backends; a GUI script renders a lit, textured, projected scene in the 3D viewer under Xvfb and its screenshot is shared with the user.

## Decisions

- 2026-11-01 — **Native USDProject3D (user):** camera projection is evaluated natively, not ignored.
- 2026-11-01 — **Unknown materials render magenta with a warning (user).**
- 2026-11-01 — **Shaping, IES, light and shadow linking are in (user).**
- 2026-11-01 — **No default light (user):** a lightless scene is emission only.
- 2026-11-14 — Claimed by bosley-native-nodes (user request).
- 2026-11-14 — Phase 1 implemented (eedb6717d). Textures are float4 storage buffers rather than GpuTextures, so CPU and GPU share one Slang path; mips come from the pixel's ray-cone footprint. Gaps: textures over the device limit are not downsampled; Project3D facing comes from winding (negative-scale transforms may differ from Storm); renaming a material's st primvar doesn't re-sync bound meshes. The NatronUtility goldens need re-recording (a displayColor-only scene is black when lightless), and `natron_utility_viewer.py` waits on Phase 2. Building; Phase 2 started.
- 2026-11-14 — Phase 2 implemented (8f21e4425), F7 merged in. All six UsdLux types with IES (LM-63 in-tree; `ies:normalize` divides by the sphere average); one shadow ray per light, batched; light/shadow linking through the pinned `HdsiLightLinkingSceneIndex` with per-instance bitmasks (native-instance categories not read yet). The dome lights but isn't seen by camera rays. Open: whether the viewer headlight reaches the delegate as a light sprim (`natron_utility_viewer.py` has no stage light); the gate shows it. Phase 1's build cancelled as superseded; gating.
- 2026-11-14 — Gate on Phase 2: product fixes — a lightless shade divided by a zero pixel count (SIGFPE); textures authored raw decoded as sRGB (26.08 moves `sourceColorSpace` to `colorSpace:file`); USDProject3D's glslfx shaders arrived without an identifier and shaded magenta (a Utility-only scene-index fills it in). Viewer: UsdImaging gives a non-Storm delegate the headlight as a `distantLight` with the `GlfSimpleLight` in its params; the Utility light now maps it to a camera-space distant light (brightness from the simple light, like Storm), so the viewer lights while lightless offline renders stay emission-only (a distant light along the view axis approximates Storm's eye point light). The reference golden renders with the Headlight knob on rather than the shared fixture gaining a light. Gating with `--record-goldens`.
- 2026-11-14 — The viewer stayed unlit because non-Storm viewers draw through the progressive engine, which never took the headlight. It now does, with scene lights off while the headlight is on (Storm parity; also gives Cycles and MoonRay the headlight in Headlight mode and in Auto with a lightless stage). Utility goldens re-recorded lit. The Lights tests were timeouts (first GPU frame compiles kernels on llvmpipe in 21–25 s), not a missing GPU. Project3D's Storm match passes in an offline probe; the test gained diagnostics. Re-gating.
- 2026-11-14 — Gate on 374c1b04e: the viewer is lit (natron_utility_viewer 84/84), Lights tests pass; only NatronUtilityProject3DTest fails — Storm's primId plane has no hits in that test while its alpha covers the frame (Utility hits all 2048). Being fixed.
- 2026-11-14 — Project3D test: Storm writes no primId for materials tagged translucent (natronProject3D.glslfx is; Storm's OIT passes write colour only), so the test now takes Storm's coverage from full alpha. Product gap found (not M281's): projected cards have no primId under Storm, so primId/picking layers miss them — raised to the user as a follow-up. Re-gating.
- 2026-11-14 — Gate on b5ac481c3 (run 43637262978): green apart from USDRenderCyclesTest's restart race (fixed by #284). PR #290 into F7 opened; Codex review running.
- 2026-11-14 — Review round 1 (Codex, 11 findings): 10 fixed in b4d874a92 (dome background, Storm PreviewSurface port, device-limit texture levels with CPU fallback, per-instance links, unsupported primvar readers and multiple UV sets flagged, constant normals, scale/bias on fallbacks, wrap from file metadata, abs focus). GUI check gains a lit textured projected scene. Cutout continuation (primary and shadow rays) is in scope — a binary cutout is absent surface, not transmission — and is being implemented before the gate.
- 2026-11-14 — Cutout continuation (be6754aee): a shared `surfaceAbsent` test and a ResolveHits pass re-trace camera and shadow rays past opacityThreshold and Project3D discard hits on CPU and GPU; opaque scenes keep the old path. Bound: 16 holes per ray (then miss/unblocked). Gating with natron_utility_viewer and m234_projection.
- 2026-11-14 — **Configurable cutout limit (user):** "configurable cutout limit, 16 is a reasonable default." A Utility renderer setting `maxCutouts` (Max Cutouts, default 16) replaces the constant on CPU and GPU.
- 2026-11-14 — Correction: run 43659041571 was Lint only, not a gate on be6754aee. Max Cutouts setting (min 1, default 16) in 3f74d6517 (Utility has no Pixel Samples setting; it follows Device's path); gating it.
- 2026-11-14 — Gate on 3f74d6517 (run 43660540407), the first real ctest/GUI run of b4d874a92 and be6754aee: no product bugs. Misses now clear on the first light pass (test updated); the cutout scenes named mesh and material alike; the golden drift (max 0.0038, colour only) is the exact Storm PreviewSurface port, so goldens are re-recorded; the viewer check's "magenta" was red/blue checker filtering (checker now red/cyan). Re-gating with --record-goldens.
- 2026-11-15 — Gate on a7c8fed07 (run 43671507607): both GUI scripts pass (lit textured projected scene included), the task-graph leg passes, goldens re-recorded and committed (c59c96e06, only reference_f1/f24). Legacy leg: GpuShadowsAndLightsAsTheCpuDoes timed out a 256-ray GPU batch after 30 s on llvmpipe (device lost) — treated as a possible non-terminating GPU loop; being diagnosed. USDRenderCyclesTest timeout in the build job is main's Cycles race (fixed on main).
- 2026-11-15 — GPU timeout diagnosed (not proven): no unbounded GPU loop found (scene has no cutouts; every kernel loop is count-bounded; traversal is driver ray queries; ray setup rejects NaN/zero rays). Likely the first batch of a cold process on loaded llvmpipe compiles the kernels inside the watchdogged submit (>30 s → device lost). PM: warm the Utility pipelines once per executor outside the watchdog, cancel-aware without declaring the device lost.
- 2026-11-15 — Kernel warm-up (8df04244a): nine Utility pipelines compile once per GPU executor on their own thread with no watchdog; render() waits in 20 ms slices on the stop flag and returns Stopped without losing the device; every frame batch is still watchdogged. Known: destroying an executor mid-warm-up waits for the kernel being compiled (only slow on software Vulkan); the compile still occupies the shared queue. Re-gating.
- 2026-11-15 — Gate on 8df04244a (run 43684963551) fully green: ctest both legs (no GPU timeout after the warm-up), natron_utility_viewer and m234_projection. PR #290 then conflicted with F7 (M283 and main merged there): merging F7 in conflicts in 18 Utility files; an implementer is integrating sampling/motion blur with lights, cutouts and the dome. Re-gate and a second review round follow.
- 2026-11-15 — F7 merged in (4ca825e3c): lights, cutouts and the dome run per pixel sample under M283's sampling; film weights include dome misses; the area-light seed varies per sample (unchanged at one sample, seed 0); NatronUtilityLights pins one pixel sample like NatronUtility_Test. Gate with --record-goldens and review round 2 (Codex) running.
- 2026-11-15 — Review round 2 (Codex, 5 findings) fixed in ff923cf58: GPU light/cutout work in submissions capped at 8× the batch with stop checks between; CPU stop before each light batch; dome CDF covers the whole image; display primvar readers shade their fallback (default 0, as USD — a scene without an authored fallback now shades black, not 0.18 grey, where the primvar is missing); TILT=INCLUDE IES profiles refused with a warning (applying tilt needs lamp mounting and up axis). Threads answered; review closed. Stale gate cancelled; gating ff923cf58 with --record-goldens.
- 2026-11-15 — Gate on ff923cf58 (run 43701201924): both GUI scripts pass; NatronUtilitySampling.EachFilterReconstructsAOnePixelFeatureToItsResponse renders a white feature black on all backends (regression from the primvar-fallback change), and one huge-mesh stop took 517 ms vs 500 ms (passed on rerun). Being fixed.
- 2026-11-15 — %h: the sampling test came in with the F7 merge and assumed M283's unlit ShadeFlat; under M281 a lightless scene is emission only (user decision), so its stripes now emit their colour. Huge-mesh cancel: shading-table copies reserve up front and check stop every 65,536 triangles/points (the copy grew with points and texcoords). If the bound still fails, Embree's BVH build progress callback is next. Re-gating.
- 2026-11-15 — Gate on bccb09b9e (run 43714651925) fully green: ctest both legs, natron_utility_viewer and m234_projection; no golden changes. Review round 2 closed. PR CI pending, then merge into F7.
