# M645 - Aurora Delegate Evaluation

Evaluate Autodesk Aurora (https://github.com/Autodesk/Aurora, Apache-2.0), a real-time GPU path tracer with the HdAurora Hydra delegate, as a possible fourth delegate. Deliver an eval AppImage with Aurora selectable in USDRender and the viewer, plus a capability and cost report, so the user can decide whether to adopt or drop it. This is evaluation only: nothing merges to main.

Research (cat-plan, 2026-11-27):
- HdAurora creates its own ray-tracing Hgi with `Hgi::CreatePlatformDefaultHgi(HgiDeviceCapabilitiesBitsRayTracing)` (`Libraries/Aurora/Source/HGI/HGIRenderer.cpp:43`). It does not use the app's Hgi for rendering, so Storm stays on HgiGL (`Engine/Nodes/Scene/HydraRenderImpl.cpp:1097`).
- That extension exists only in `autodesk-forks/USD` branch `adsk/feature/hgiraytracing-metal`, which is exactly OpenUSD v26.08 + 68 commits (merge base ee47c679ab, 93 files). The commits add `hgi/` acceleration-structure, ray-tracing pipeline and cmd classes, extend `hgiVulkan`, and add Metal parts plus small cmake and OpenEXR fixes. Our USD is the prebuilt ASWF 26.08 in `/usr/local` (`tools/ci/local/Dockerfile:31`); the repo patches no USD today.
- HdAurora supports `mesh` rprims, `camera`/`material`/`domeLight`/`distantLight` sprims, and the `color` (half4) and `depth` AOVs. Materials are Standard Surface through MaterialX documents, plus UsdPreviewSurface. The denoiser is DirectX-only. Its `HdAuroraRenderBuffer::Map()` reads pixels back to the CPU, which matches `HydraRenderImpl.cpp:1611-1624`. Shared GL/DX texture handles are unsupported on Linux.
- Upstream main is "v26.08 Preview" (2026-09-16) and already carries the 26.08 `IsSupported(HdRendererCreateArgsSchema)` signature (`HdAuroraPlugin.cpp:81-89`). Its externals pin Slang 2026.14.1 (we ship slangc 2026.19 only, `Dockerfile:182`) and `autodesk-forks/MaterialX` `v1.39.5.20260603.dev_adsk`.
- `RenderDelegates::list()` (`Engine/Nodes/Scene/RenderDelegates.cpp:119-160`) probes every registered Hydra plugin, so a registered HdAurora shows up with no list code. Bundled delegates take plugInfo priority 99 (lowest wins; `build-cycles.sh:63-66`). MoonRay's start-up registration in `Engine/Nodes/Scene/MoonRayRuntime.cpp:242-277` is the pattern for a private prefix.

## Scope

**In:**
- A separate eval image that rebuilds USD 26.08 with Autodesk's HGI ray-tracing commits (HgiVulkan on) over the ASWF copy, then builds Aurora/HdAurora and Natron. It leaves out Cycles and MoonRay.
- Natron glue: start-up registration of the Aurora prefix, a guarded probe, and a small Aurora settings page.
- An eval AppImage with a UAT steps file. Aurora is selectable in USDRender and the viewer.
- A capability and cost report sent to the user, and the user's adopt-or-drop decision recorded.

**Out:**
- Merging anything to main: all code lives on the pushed `eval/aurora` branch, with a draft PR that is never readied or merged. Adoption would be a later milestone, filed after the decision.
- Changes to main's dev image, the published ghcr image or the release AppImage: none.
- Cycles and MoonRay in the eval image. Their comparison stills come from main's AppImage on the same scenes.
- Patching Aurora to add missing prims, lights, AOVs, deep output or projection. The report lists these gaps rather than closing them.
- Every Aurora setting: the denoise and shared-handle keys are left off the page.
- Remote/PR CI gates for the eval branch: none (the image is local-only).

**Edge cases:**
- No ray-tracing Vulkan device (this dev host's iGPU, a machine without RT): `Aurora::createRenderer` asserts if Hgi creation fails, so the probe must check for a ray-tracing-capable device before instantiating it. Aurora is then listed as unavailable, with `RenderDelegates::unavailableMessage`, and Natron does not abort.
- lavapipe: used for the headless load-and-render test only (slow, correctness smoke). Timings come only from the GPU host's NVIDIA GPU.
- Autodesk's commits don't apply cleanly or build against the ASWF USD configuration, or the image build runs past about 2 h: stop and report numbers to the user before continuing.
- Aurora rejects the image's MaterialX: build the autodesk-forks MaterialX tag in the eval image instead, and report the conflict as an adoption cost.
- A scene uses prims or lights Aurora ignores: it renders without them, and the report says so. No error is expected.

**Assumptions:**
- Unconfirmed: the ASWF USD build has HgiVulkan off, so the eval image needs a full USD source build, not just patching the hgi libraries.
- Unconfirmed: Mesa lavapipe in the image exposes `VK_KHR_ray_tracing_pipeline`/`acceleration_structure`, which Aurora's README says works for Linux smoke tests.
- Unconfirmed: the GPU host (NVIDIA) runs the eval AppImage for timings. A cloud VM (cloud-gpu skill) is the fallback.
- Unconfirmed: which scenes go into the comparison. The user picks them with the eval AppImage in hand (user, 2026-11-27).

## Phase 645.1: Eval image

- [ ] M645.P1.T1 — Inventory Aurora, the USD patch and the image's USD build
  - files: report only (no commits)
  - approach: clone Aurora main and `autodesk-forks/USD` `adsk/feature/hgiraytracing-metal`, and pin both by commit. List the 68 commits over v26.08 and separate the Linux-relevant ones (hgi, hgiVulkan, cmake, OpenEXR) from the Metal-only ones. In natron-dev, read `/usr/local/lib/cmake/pxr` and `pxrConfig.cmake` for the ASWF build flags (Vulkan, imaging, Python, MaterialX), and the MaterialX version. Check whether Aurora builds against stock MaterialX 1.39 and whether it needs the libslang runtime (Transpiler.cpp). Write down Aurora's own CMake options for a Linux, HGI-only build without Plasma.
  - verify: each claim cites a file and line in the cloned trees or the image. Any claim from recollection is flagged as such.
  - size: M

- [ ] M645.P1.T2 — Eval Dockerfile with a patched USD 26.08
  - files: `tools/ci/eval/aurora.Dockerfile`, `tools/ci/eval/build-usd-hgirt.sh` (new, on `eval/aurora`)
  - approach: start FROM the same `aswf/ci-vfxall:2027-clang21.1` base and replicate `tools/ci/local/Dockerfile`'s layers except the Cycles and MoonRay builds (Slang, glslang, mesa checks). Then build OpenUSD at the T1-pinned fork commit, with PXR_ENABLE_VULKAN_SUPPORT on and every other option matching the ASWF flags from T1. Install it over `/usr/local` so one USD is on the path, and provide the Vulkan SDK pieces USD needs: headers ≥1.3.286, VMA ≥3.2 at `vma/vk_mem_alloc.h`, vulkan-utility-libraries, and shaderc with glslang bundled.
  - verify: the image builds; `ldd /usr/local/lib/libusd_hgiVulkan.so` resolves, with no undefined `glslang::`/`spvtools::` symbols; `nm -DC` shows `HgiVulkanRayTracingPipeline`; the build time and the size against natron-dev are recorded.
  - size: L

- [ ] M645.P1.T3 — Build Aurora and HdAurora in the eval image
  - files: `tools/ci/eval/build-aurora.sh`, `tools/ci/eval/aurora.Dockerfile`
  - approach: build Aurora at the T1-pinned commit, Release, HGI backend, `ENABLE_APPLICATIONS=OFF`, against the image's USD, TBB, OIIO and OpenEXR. Use the image's MaterialX, or the autodesk-forks tag if T1 found it is needed. Install into `/opt/aurora`, with HdAurora's plugInfo priority set to 99 and libslang plus the MaterialX libraries copied in. Apply small build fixes as guarded seds in the script, each with the reason in a comment.
  - verify: `/opt/aurora` contains `hdAurora.so`, `plugInfo.json` (priority 99) and the Aurora libraries; `ldd` shows no unresolved or duplicate public libraries; `usdcat` or a python `HdRendererPluginRegistry` query inside the image lists HdAurora.
  - size: L

## Phase 645.2: Natron integration

- [ ] M645.P2.T1 — Register Aurora at start-up with a guarded probe
  - files: `Engine/Nodes/Scene/AuroraRuntime.cpp`, `Engine/Nodes/Scene/AuroraRuntime.h`, `Engine/AppManager.cpp`, `Engine/Nodes/Scene/RenderDelegates.cpp`, the Engine CMake source list
  - approach: mirror `MoonRayRuntime`. Find `<bin>/../aurora`, or `NATRON_AURORA_PREFIX`, and `PlugRegistry::RegisterPlugins` it next to the MoonRay call in AppManager. Set `PXR_MTLX_STDLIB_SEARCH_PATHS` to its MaterialX libraries. Before `probe()` instantiates HdAurora, check with a Vulkan instance query for a physical device that has `VK_KHR_ray_tracing_pipeline` and `VK_KHR_acceleration_structure`. If there is none, report it unavailable with a message naming the missing capability.
  - verify: in the eval image, Natron lists Aurora (available on lavapipe if T1's assumption holds); with `VK_ICD_FILENAMES` pointing at a non-RT ICD it is listed as unavailable and Natron does not abort.
  - size: M

- [ ] M645.P2.T2 — Aurora load-and-render test
  - files: `Tests/Scene/AuroraDelegate_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: follow `MoonRayDelegate_Test.cpp`: skip unless `NATRON_AURORA_PREFIX` is set, and fail on skip when `NATRON_TEST_REQUIRE_AURORA=1`. Instantiate HdAurora headless through the M248 registry and render a lit UsdPreviewSurface cube with a dome light on lavapipe through USDRender. Assert non-black colour pixels where the cube is, and a finite depth there. Use the bundled gtest only (`INSTANTIATE_TEST_CASE_P`, no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`).
  - verify: passes in the eval image with `NATRON_TEST_REQUIRE_AURORA=1` under lavapipe; existing Storm and Utility Scene tests still pass there.
  - size: M

- [ ] M645.P2.T3 — Aurora settings page
  - files: `Engine/Nodes/Scene/AuroraSettings.h`, `Engine/Nodes/Scene/AuroraSettings.cpp`, the call sites that wire `Engine/Nodes/Scene/CyclesSettings.cpp` into the settings flow, `Tests/Scene/RenderSettingsKnobs_Test.cpp`
  - approach: hand-written page keyed by HdAurora's tokens: `maxSamples` "Max Samples", `traceDepth` "Trace Depth", `isAlphaEnabled` "Alpha", `useEnvironmentImageAsBackground` "Use Environment As Background", and the ground-plane settings "Ground Plane". Defaults match HdAurora's constructor. Labels are the readable script names. Tooltips do not mention other products.
  - verify: the RenderSettingsKnobs test covers the Aurora page; changing Max Samples in the viewer restarts sampling (checked by Xvfb GUI script on lavapipe, screenshot sent to the user).
  - size: M

- [ ] M645.P2.T4 — Eval AppImage and UAT steps
  - files: `tools/release/stage-bundle.sh`, `tools/release/check-startup.sh` (as MoonRay's bundling does), `.local/uat/` steps file
  - approach: stage `/opt/aurora` as `aurora/` beside `moonray/`'s slot, with RPATHs, libslang and the bundled Vulkan loader. Build the `release` preset AppImage in the eval image locally. Write `<basename>.md` UAT steps: select Aurora in the viewer and USDRender, change settings, the no-RT message, and a note that the user picks the comparison scenes. Push `eval/aurora` and open a draft PR into main titled `M645 - Aurora Delegate Evaluation (do not merge)`.
  - verify: extracted AppImage bins run with `LD_LIBRARY_PATH` unset and list Aurora; on the GPU host the viewer renders the T2 cube scene through Aurora.
  - size: M

## Phase 645.3: Report and decision

- [ ] M645.P3.T1 — Capability and cost report
  - files: report sent to the user (`build/aurora-eval/report.md`, not committed)
  - approach: after the user names the scenes at UAT, render them on the GPU host's NVIDIA GPU through Aurora, Storm and Utility in the eval AppImage, and through Cycles and MoonRay in main's AppImage, with the same camera and resolution. Measure time to first pixel and samples per second at the viewer size and at the USDRender size, interactivity while orbiting, and VRAM. List what Aurora drops on each scene (prims, lights, AOVs, materials). Cost section: eval image build time and size against natron-dev, the USD patch's size and upstream status, version pins (Slang, MaterialX), and what adoption would take (a patched USD in main's image, CI ray-tracing coverage, upkeep). Send the images one file per screenshot with the report.
  - verify: the user has the report and the per-scene images; every number names the host, GPU and build it came from.
  - size: M

**Verification gate:** the eval image builds; `AuroraDelegate_Test` passes with `NATRON_TEST_REQUIRE_AURORA=1` on lavapipe; the eval AppImage lists and renders Aurora on the GPU host and degrades cleanly without a ray-tracing GPU; the report is delivered and the user's adopt-or-drop decision is recorded in `## Decisions`. The draft PR is then closed unmerged (or kept as the adoption base, if adopted) and the row set `done` without a merge, a deliberate exception to §9 for an evaluation.

## Decisions

- 2026-11-27 — Evaluation only (user): Aurora is evaluated, not adopted; shipping it is decided after the report.
- 2026-11-27 — Separate eval image (user): the patched USD changes Hgi's ABI under Storm, Cycles and MoonRay, so it lives in its own Dockerfile and main's image is untouched until adoption.
- 2026-11-27 — Deliverable is an eval AppImage plus a comparison report (user).
- 2026-11-27 — Unmerged `eval/aurora` branch (user): every change, glue included, stays off main. An adopted Aurora is re-ported properly in a later milestone.
- 2026-11-27 — No Cycles or MoonRay in the eval image (user): this keeps the image build short; their comparison stills come from main's AppImage.
- 2026-11-27 — Small settings page with the core keys (user): Max Samples, Trace Depth, Alpha, Use Environment As Background and Ground Plane are enough to trade quality for speed fairly. Denoise is DirectX-only and inert on Linux.
- 2026-11-27 — Comparison scenes chosen at UAT (user): the user picks the scenes with the eval AppImage in hand, so P3.T1 waits on that answer (`Needs: response` at that point).
