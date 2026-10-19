# M126 - Runtime GPU Shaders

This milestone lets the Vulkan compute backend compile and run shaders generated at runtime, alongside the offline-compiled Slang kernels it has today. The first consumer is OCIO: the user decided its GPU path compiles OCIO's own generated shader at runtime (M127 - Native OCIO Nodes). That shader expects uniform buffers and sampled LUT textures, which the backend doesn't support yet, so this milestone also adds descriptor support for them. A pipeline cache keeps recompiles rare. When a compile fails, the node reports it and falls back to the CPU.

Deps: M85 - Native GPU Kernels, for the backend, kernel registry and placement this builds on.

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **Offline SPIR-V only:**
  - `Engine/Gpu/GpuKernel.cpp:65` creates the shader module from precompiled SPIR-V.
  - `cmake/FetchSlang.cmake:1-3` uses only `slangc` and the headers; libslang is never linked.
  - `Engine/Gpu/CMakeLists.txt:18-23` registers kernels at build time through `slang_add_kernel`.
- **Kernel convention** (`Engine/Gpu/GpuKernel.h:16-19`):
  - every resource is a storage buffer in set 0;
  - parameters are one push-constant block of at most 128 B;
  - there are no uniform buffers and no sampled images.
- **No runtime compiler:** neither glslang nor shaderc is linked anywhere in `Engine/Gpu`.
- **OCIO** is 2.5 (`Engine/CMakeLists.txt:31`, container 2.5.2). Its `GpuShaderDesc` supports `GPU_LANGUAGE_GLSL_VK_4_6`. The generated shader declares uniforms and 1D/2D/3D LUT textures with samplers.
- **The only OCIO GPU code in the tree** is the viewer's OpenGL path: `Gui/ViewerGLPrivate.cpp:828-1090`, `GPU_LANGUAGE_GLSL_1_2` at `:894`.

## Scope

**In:**
- A runtime GLSL→SPIR-V compiler dependency, chosen by a spike.
- Runtime kernel objects next to the registry's offline kernels.
- Descriptor support for uniform buffers and sampled 1D/2D/3D textures with samplers.
- A pipeline cache keyed by a hash of the shader source, in memory and on disk.
- Compile errors reported as the node's persistent message.
- CPU fallback when a compile fails.
- A test that compiles and runs a runtime shader with a UBO and a 3D LUT texture.

**Out:**
- OCIO nodes and their shader generation: M127 - Native OCIO Nodes.
- Viewer OCIO on Vulkan: M107 - GPU Viewer Hand-off.
- User-authored shader nodes (SeExpr, Shadertoy-style): none for now. This milestone makes them possible later.

**Edge cases:**
- **A compile fails:** report the compiler log once per node and shader hash, then render on the CPU for that shader. Never retry in a loop.
- **The same shader is requested by many nodes and threads at once:** compile once, and have the other requests wait on the in-flight compile.
- **The on-disk cache is stale, corrupt, or from another driver or compiler version:** the cache key includes the compiler version and the device UUID, and an invalid entry is recompiled and overwritten.
- **A texture format the device can't sample** (e.g. linear filtering on a 32-bit float 3D texture): fall back to nearest plus manual interpolation if the shader allows it, else to the CPU.

**Assumptions:**
- Unconfirmed: the compiler is libshaderc, or glslang directly, for GLSL→SPIR-V, since OCIO emits `GPU_LANGUAGE_GLSL_VK_4_6`. P1.T1 compares them with linking libslang on build time, AppImage size, licence and API.
- Unconfirmed: the on-disk cache lives under the user cache directory (`~/.cache/Natron/gpu-pipelines/`) and is capped in size.
- Unconfirmed: the ASWF container provides the chosen compiler or it builds from source in `fetch-assets.sh`. The spike confirms which.

## Phase 125.1: Compiler choice

- [x] M126.P1.T1 — Spike: choose the runtime compiler
  - files: `.plan/PLAN/DESIGN/2026-10-13-runtime-shader-compiler.md` (new)
  - approach:
    - Compare libshaderc, glslang and libslang (runtime API) on:
      - licence compatibility with GPL-2.0-or-later;
      - added AppImage size;
      - build integration in the container and `fetch-assets.sh`;
      - compile latency for a typical OCIO shader;
      - API for include-free single-source compilation.
    - Recommend one.
  - verify: the design note records the measurements and the recommendation, and the user confirms the choice before P1.T2 starts.
  - size: M

- [x] M126.P1.T2 — Add the chosen compiler to the build and the AppImage
  - files: `cmake/` (a new `Find` or fetch module), `Engine/Gpu/CMakeLists.txt`, `tools/ci/local/fetch-assets.sh`, `tools/package/` (bundle list; locate the AppImage library list)
  - approach:
    - Link the compiler into the GPU library behind `NATRON_HAVE_GPU`.
    - Bundle its shared libraries in the AppImage, or link it statically if the spike chose that.
  - verify: at the phase build, the GPU library links, and `package.sh` produces an AppImage whose `ldd` shows no missing compiler library.
  - size: M

## Phase 125.2: Runtime kernels and descriptors

- [x] M126.P2.T1 — Compile GLSL to SPIR-V at runtime behind a small interface
  - files: `Engine/Gpu/GpuShaderCompiler.{h,cpp}` (new), `Tests/GpuShaderCompiler_Test.cpp` (new)
  - approach:
    - `compileGlsl(source, stage, entry) -> expected<SpirV, CompileLog>`. It is thread-safe, with one compile per hash in flight; other callers wait on it.
  - verify: tests compile a valid compute shader and report a readable error for an invalid one. Runs at the phase build.
  - size: M

- [x] M126.P2.T2 — Support uniform buffers and sampled textures in the kernel layer
  - files: `Engine/Gpu/GpuKernel.{h,cpp}`, `Engine/Gpu/GpuDevice.{h,cpp}` (or the file that owns descriptor pools; locate at promotion), `Engine/Gpu/GpuTexture.{h,cpp}` (new)
  - approach:
    - Build each runtime kernel's descriptor-set layout from SPIR-V reflection (or a caller-supplied binding list): storage buffers, a UBO, and sampled 1D/2D/3D images with samplers.
    - `GpuTexture` uploads float LUTs with linear or nearest filtering and clamp-to-edge.
    - Offline kernels keep their current layout unchanged.
  - verify: at the phase build, the existing GPU tests pass unchanged, and P2.T4's test covers the new bindings.
  - size: L

- [x] M126.P2.T3 — Pipeline cache in memory and on disk
  - files: `Engine/Gpu/GpuPipelineCache.{h,cpp}` (new), `Engine/Gpu/GpuKernel.cpp`, `Tests/GpuPipelineCache_Test.cpp` (new)
  - approach:
    - Key on a hash of the source, the compiler version and the device UUID.
    - The in-memory LRU maps the key to the pipeline. On disk, store the SPIR-V plus a `VkPipelineCache` blob, capped in size, with invalid entries recompiled.
  - verify: tests show a second request hits memory, a restart hits disk, and a corrupt file is recompiled. Runs at the phase build.
  - size: M

- [x] M126.P2.T4 — End-to-end runtime shader test with a UBO and a 3D LUT
  - files: `Tests/GpuRuntimeShader_Test.cpp` (new)
  - approach: compile a GLSL compute shader at runtime that reads a storage-buffer image, applies a UBO gain, and samples a 3D LUT texture. Compare the result against a CPU reference.
  - verify: matches within 1e-5 on lavapipe and RADV at the phase build.
  - size: M

## Phase 125.3: Errors and fallback

- [x] M126.P3.T1 — Report compile failures and fall back to the CPU
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/Nodes/Image/GpuRender.h`, `Tests/GpuRuntimeShader_Test.cpp`
  - approach:
    - Add a `GpuQuery` hook so a node can supply a runtime shader instead of a registry kernel ID.
    - On a compile failure, `canRenderOnGpu` returns false for that shader hash, the node's persistent message shows the first lines of the log, and the render runs on the CPU.
  - verify: a test node with a broken shader renders correctly on the CPU, shows the message once, and doesn't retry the compile. Runs at the phase build.
  - size: M

**Verification gate:**
- In one build, ctest passes, including `GpuShaderCompiler_Test`, `GpuPipelineCache_Test` and `GpuRuntimeShader_Test` on lavapipe and RADV.
- The AppImage from `package.sh` runs the runtime-shader test on the target host.
- The compiler choice is recorded and confirmed by the user.

## Decisions

- 2026-10-13 — **OCIO's GPU path compiles OCIO's own shader at runtime** (user). This was chosen over translating OCIO ops into our kernels or baking a shaped 3D LUT. This milestone is split out from M127 - Native OCIO Nodes because it is backend infrastructure in E5 - GPU Compute that other runtime-generated shaders can reuse.
- 2026-10-16 — **Runtime compiler: glslang, called directly, linked statically into `NatronGpu`** (user-confirmed, from M126.P1.T1's spike; see `PLAN/DESIGN/2026-10-13-runtime-shader-compiler.md`). Fetched by a new `cmake/FetchGlslang.cmake` on the `FetchSlang.cmake` pattern, built with `ENABLE_OPT=OFF` and no tools, installs or tests. A static link needs no change to `fetch-assets.sh` or the AppImage bundle. Descriptor layouts come from vendored SPIRV-Reflect. Rejected: shaderc, which only wraps glslang, and Slang's GLSL mode, which takes 182–254 ms per compile, uses a deprecated flag and adds 12.6 MB. Measured: about 63 ms for the first compile in a process, then 2.3 ms for a 317-line OCIO-style shader. Not yet confirmed: whether OCIO emits the `layout(set, binding)` qualifiers we need, and the Vulkan and SPIR-V target versions.
- 2026-10-16 — **The GPU compiler test lives in `Tests/Gpu/GpuShaderCompiler_Test.cpp`**, inside the existing `GpuTests` target, rather than in `Tests/`. Compile results, failures included, are cached for the process lifetime per stage, entry and source, so a late caller never recompiles. The result is a plain struct (`ok`, `spirv`, `log`), since `std::expected` is C++23. glslang 16.6.0 is pinned, along with SPIRV-Reflect at tag `vulkan-sdk-1.4.363.0`, vendored under `libs/SPIRV-Reflect/`.
- 2026-10-18 — **M126.P2.T2: reflected kernels reject `WorkgroupSize` defined by spec constants, and `GpuRuntime::registerRuntimeKernel` re-registers runtime kernels after a device-loss rebuild.** SPIRV-Reflect ignores the `WorkgroupSize` built-in, so such a shader would dispatch at 1×1×1. Runtime registrations are not yet deduplicated and keep their SPIR-V forever, so M126.P2.T3's cache should key and reuse them. Reflected kernels require push data that exactly matches the reflected block size.
- 2026-10-18 — **M126.P2.T3: `GpuPipelineCache` is per `GpuDevice` and returns `shared_ptr<GpuKernel>`, so LRU eviction never destroys a kernel in use.** Its default directory is `$XDG_CACHE_HOME/Natron/gpu-pipelines`, without Qt, because NatronGpu stays Qt-free. Defaults: 64 kernels in memory, 64 MB on disk. The registry's runtime registrations are still not deduplicated. M126.P3.T1 should route node shaders through the cache rather than `registerRuntimeKernel`, and should rebuild the cache after a device-loss `recreate()`. The `glslang::GetGlslVersionString()` signature is unconfirmed until the phase build.
- 2026-10-18 — **M126.P2.T4: the test lives at `Tests/Gpu/GpuRuntimeShader_Test.cpp`**, not under `Tests/`. Its inputs sit on a 1/32-texel grid so that fixed-point hardware filter weights are exact and 1e-5 holds on RADV; off-grid sampling is not covered. It dispatches through `recordBindings` on a raw command buffer, because `GpuRecordContext` can't dispatch the cache's `shared_ptr<GpuKernel>`. M126.P3.T1 may need a record-context entry for cached kernels. The branch was rebased onto `93339c413` after this task, so the earlier SHAs in this file changed: P2.T2 is now `ee3bed7c0` and P2.T3 is `c0f8340fe`.
- 2026-10-18 — **Phase 2 build (run 39168347084, `2b415f7f3`):** 2369 of 2370 tests pass in each scheduler mode. The one failure was `GpuPipelineCacheTest.CorruptEntriesAreRecompiledAndOverwritten`, a test bug: it chose its victim file by mtime, and a tie damaged the previous entry. Fixed in `5ad42f50c`.
- 2026-10-18 — **M126.P3.T1: nodes supply GLSL through `NativeImageEffect::gpuRuntimeShader()`, with uniforms and textures matched to bindings by name.** Binding 0 is the divided source and binding 1 the output, so mask, mix and premult reuse the existing divide and finish passes. `GpuRuntime` owns the `GpuPipelineCache` and rebuilds it on `recreate()`. Node tests are in `Tests/Native/NativeRuntimeShaderGpu_Test.cpp`. Known limits, left for M127 - Native OCIO Nodes and the review: textures are uploaded again, blocking, on every `renderGpu()`, so OCIO's LUTs will want a texture cache. Every `GpuRuntime` creates `~/.cache/Natron/gpu-pipelines`, tests included. `NativeGpuParity::runRenderGpu`, which uses its own runtime, refuses runtime-shader nodes.
- 2026-10-18 — **Gate:** lavapipe ctest is green in both modes (run 39173055192). On RADV, a local build of `5ad42f50c` passes the full `GpuTests` (111 tests, 8 GL-interop skips), and the runtime-shader, native-GPU and residency tests pass in both modes. **Deviation:** the "AppImage runs the runtime-shader test" item was checked with an AppImage packaged locally from the same commit, because the sandbox can't reach the Azure artifact host. It has no missing libraries across 283 ELF files and renders headless. No node in the AppImage uses a runtime shader yet, so the RADV test run covers that path instead.
- 2026-10-18 — **Review round 1 (Codex, PR #68):** three findings, all fixed in `9b35b62cc`. The compile memo is now a 128-entry LRU kept apart from in-flight dedup; the per-node rejection set still prevents a retry loop. Disk entries are size-checked before they are read. An entry larger than the disk budget is not written, and a budget of 0 disables disk caching.
- 2026-10-18 — **Merged as `2410277ee` (PR #68).** PR CI's first run failed on format: our new files, plus vendored SPIRV-Reflect, which is now excluded like VMA. It also showed two one-off ctest failures that did not recur on `ef5a85673`: the known project-save temp-file collision, and a teardown SIGSEGV after `DeepNodesTest.DeepReformatAsksItsInputForATranslatedRegion` had passed.
