# M126.P1.T1 - Runtime GLSL to SPIR-V compiler for the Vulkan compute backend

## Question
OCIO 2.5 `GpuShaderDesc` (GPU_LANGUAGE_GLSL_VK_4_6) emits GLSL (uniform block, 1D/2D/3D LUT
samplers). The GPU backend (`Engine/Gpu/`, library `NatronGpu`) today only loads SPIR-V that
`slangc` precompiled offline (`cmake/FetchSlang.cmake`, `slang_add_kernel`); `libslang` is never
linked. Which library compiles GLSL to SPIR-V at runtime, how is it shipped, and how do we get
descriptor-set layouts out of the result?

## Candidates
1. **glslang** (direct API, `glslang::TShader/TProgram` + `GlslangToSpv`), BSD-3 core.
2. **libshaderc** (Google wrapper over glslang + SPIRV-Tools; C/C++ API).
3. **libslang** runtime API (`slang.h`, `SLANG_SOURCE_LANGUAGE_GLSL`).
4. (added by the spike) **Qt6ShaderTools / QShaderBaker** - Qt's private glslang copy.

## Facts found in the environment
- `natron-dev` (Rocky 9.8, gcc 14.2): NO glslang, shaderc or SPIRV-Tools headers/devel;
  only runtime `spirv-tools-libs 2025.4` .so files and `vulkan-headers 1.4.328`. dnf cannot reach
  repos offline here, so availability of an EL9/EPEL glslang package was not checked.
  No spirv-cross / SPIRV-Reflect installed. Qt 6.8.3 (custom, /usr/local) has libQt6ShaderTools.
- `/usr/lib64/libslang.so.2` in the container is the unrelated S-Lang text library.
- `tools/release/stage-bundle.sh` bundles the **ldd closure** of the binaries: any DT_NEEDED .so is
  auto-bundled; a statically linked compiler needs no packaging change at all.
- `tools/ci/local/fetch-assets.sh` only builds OFX plugin bundles; toolchain-ish deps (Slang) are
  fetched by CMake into `.natron-deps` (pinned URL + SHA256). A compiler dep belongs there, not
  in fetch-assets.sh.
- `NatronGpu` is deliberately engine-free (no Qt), `tools/` also add_subdirectory's it.
- Baseline AppImage (m90-bench `base-e93abc91`): 88.9 MB.

## Comparison
| | glslang direct | shaderc | libslang runtime | QShaderBaker |
|---|---|---|---|---|
| Licence | BSD-3/BSD-2/MIT/Apache-2.0 (LICENSE.txt checked; no GPL parts); SPIRV-Tools Apache-2.0 | Apache-2.0 (+ glslang, SPIRV-Tools) | Apache-2.0 WITH LLVM-exception | Qt GPL/LGPL + bundled glslang |
| GPL-2.0-or-later fit | OK via GPLv3 path (Apache-2.0 is GPLv3-compatible, not GPLv2-only) | same | best (LLVM exception makes it GPLv2-compatible) | OK |
| Accepts GLSL | yes, native | yes (via glslang) | yes: `-allow-glsl -lang glsl` compiled ocio.comp, but deprecated flag and Slang's GLSL support is a compatibility layer | yes |
| Size added (stripped, raw / xz) | static, full incl. optimizer: ~8.4 MB / 2.4 MB (MEASURED, test exe); no-optimizer build ~3-4 MB (ESTIMATED, ENABLE_OPT=OFF not built) | shared libshaderc+glslang+SPIRV-Tools ~7-9 MB (ESTIMATED; Debian libshaderc.so.1 is 5.6 MB and still needs glslang) | libslang-compiler 31.4 MB + libslang-glslang 10.1 MB + glsl-module 1.3 MB + rt 0.3 MB = 43 MB raw; xz 10.1 + 2.5 MB (MEASURED). slang-llvm (152 MB) not needed | libQt6ShaderTools.so.6.8.3 6.5 MB (MEASURED), not currently in the AppImage |
| Build integration | no package in container: pinned source fetch + static build (spike built it with Ninja, plain cmake, gcc, no external deps with BUILD_EXTERNAL off for tests) | same source build (needs glslang+SPIRV-Tools+headers checkouts, git-sync-deps) | already fetched; just link libslang + ship libs | needs find_package(Qt6ShaderTools) + Qt in engine-free NatronGpu |
| Cold compile (first call) | 62-65 ms, one-time builtin symbol tables (MEASURED, in-process) | ~same engine (ESTIMATED) | slangc process 728 ms cold, 182-254 ms warm incl. process start (MEASURED CLI) | 46-105 ms (MEASURED) |
| Warm compile, 317-line / 15.8 KB OCIO-like shader | **2.3 ms** (MEASURED); 15.5 ms with spirv-opt performance passes, spv 37.6 -> 22.1 KB | ~2-3 ms (ESTIMATED) | not measured in-process (CLI 182-254 ms) | 4 ms (MEASURED) |
| Small shader (62 lines, UBO+sampler) warm | 0.5 ms (MEASURED) | - | - | <1 ms |
| CLI proxy | glslangValidator 57 ms/run, glslc 50 ms/run (-O 66) (MEASURED, dominated by process start + builtin init; not representative of in-process cost) | | | |
| Thread safety | `InitializeProcess()` once per process (ref-counted); each thread uses its own TShader/TProgram; 4 threads x 3 compiles ran clean (MEASURED, 9.3 ms total). Do not share TShader/TProgram. Pool allocator is thread-local | shaderc_compiler_t is per-thread; compiler objects not shared; thread-safe across instances | `IGlobalSession` shareable; sessions/modules not | one QShaderBaker per thread |
| Include-free single source | `setStrings` + no `Includer` passed to `parse()` (#include simply errors unless GL_GOOGLE_include_directive enabled) | `shaderc_compile_into_spv`, no include callback set | `loadModuleFromSourceString` | `setSourceString` |

Test shaders: the generated OCIO-style compute shader (`spike/ocio.comp`, 317 lines, 2 sampler1D
LUTs, push constants, SSBOs) and a UBO variant (`ocio2.comp`). Neither has a sampler3D after the
generator; compile cost is dominated by function bodies/builtins, not sampler dimensionality, so
3D LUTs are not expected to change the result (UNMEASURED).

## Reflection
- **SPIRV-Reflect** (KhronosGroup, Apache-2.0, one `.c`+`.h` pair, ~6k lines, no deps, vendorable
  into `libs/`): gives descriptor sets/bindings/types/stage flags/block sizes directly. Cost: ~100 KB code,
  sub-millisecond parse (ESTIMATED). Maps 1:1 onto `VkDescriptorSetLayoutBinding`.
- **SPIRV-Cross**: ~1-2 MB, a cross-compiler we do not need (ESTIMATED).
- **QShaderDescription** (Qt) reflects too (spike confirmed ubos/sampled images/storage with
  set+binding in 6.8.3) but drags Qt into NatronGpu.
- **Caller-supplied binding list**: OCIO already knows its LUT textures and uniform block, but a
  hand-written list can drift from the compiled SPIR-V; fine as an assertion, poor as source of truth.

## Recommendation
- **Compiler: glslang, called directly**, linked **statically** into `NatronGpu` (PRIVATE).
  Rationale: smallest footprint (est. 3-4 MB without the optimizer, 8.4 MB with it; vs 43 MB
  for libslang), fastest warm path (2.3 ms vs CLI-scale hundreds of ms), simplest licence
  story, no change to the ldd-closure bundler, no new shared lib to track in excludelist.
  libslang rejected: 5-10x the size, GLSL input marked deprecated/compat-layer, nothing else
  in the product needs the runtime API. shaderc rejected: it is glslang plus a wrapper and
  needs the same source checkouts. QShaderBaker rejected: Qt in engine-free NatronGpu, private-ish RHI headers.
- **Optimiser: off by default** (build glslang with `ENABLE_OPT=OFF` to avoid shipping the 10 MB
  SPIRV-Tools-opt archive; the driver's own compiler optimises). Measured cost of spirv-opt
  is 13 ms/shader for a 40% smaller module, so it is not worth the bytes. Revisit only if
  pipeline-cache misses on huge shaders become measurable. Compile once per (OCIO cache-id), run
  on a worker thread, cache SPIR-V to disk keyed by the same id.
- **Entry into the build:** new `cmake/FetchGlslang.cmake`, modelled on `FetchSlang.cmake`
  (pinned tag + SHA256 of the GitHub source tarball in `.natron-deps`, `GLSLANG_ROOT`/env override,
  extract-to-partial-then-rename), consumed from `Engine/Gpu/CMakeLists.txt` via
  `add_subdirectory(... EXCLUDE_FROM_ALL)` or ExternalProject with: `BUILD_SHARED_LIBS=OFF`,
  `ENABLE_GLSLANG_BINARIES=OFF`, `ENABLE_CTEST=OFF`, `ENABLE_OPT=OFF`, `GLSLANG_ENABLE_INSTALL=OFF`,
  `BUILD_EXTERNAL=OFF`, `-fPIC`. Link `glslang`, `glslang-default-resource-limits` (SPIRV is merged into
  `libglslang.a` at the commit the spike built, 4b626a0, Sep 2026) plus `SPIRV-Tools` core.
  Pin a tagged release at implementation time; the spike used a main-branch checkout.
  No change to `fetch-assets.sh`, `package.sh`, `stage-bundle.sh` or the excludelist.
- **Reflection: vendor SPIRV-Reflect** (`libs/SPIRV-Reflect`, two files, no build dependency) and
  build `VkDescriptorSetLayout`s from the compiled module. OCIO's names (texture/uniform names in
  OpName) are still the key used to bind data; keep a unit test comparing OCIO's declared texture
  count against reflected image bindings.
- **Target env:** `EShClientVulkan`/`EShTargetVulkan_1_3`, SPIR-V 1.6 worked for the OCIO-style
  shader; must be no higher than the device API actually requested by `GpuContext`. (Check the
  version Natron's instance creates before fixing 1_3.)

## Risks
- Build time: glslang+SPIRV-Tools from source is a one-time ~400-TU build (measured only as a
  partial interrupted build in the spike; no wall-clock figure). Mitigate by caching in
  `.natron-deps` / build cache the way Slang is cached, and ENABLE_OPT=OFF cuts ~30% of TUs (ESTIMATED).
- Apache-2.0 components mean the combined binary is effectively GPLv3-or-later, not v2. Check
  against the project's licensing stance; Slang's LLVM-exception would avoid this but is 5-10x larger.
- Ship the glslang/SPIRV-Tools/SPIRV-Reflect notices with the AppImage (third-party-notices).
- glslang is a security-relevant parser of untrusted-ish text; here the input is OCIO-generated from
  user config files, so a malformed OCIO config could hit compiler bugs. Keep the compile on a
  worker with error propagation, never abort.
- GLSL emitted by OCIO uses `texture()` on sampler1D/2D/3D; Vulkan GLSL requires explicit
  `layout(set, binding)` on each. OCIO's `setDescriptorSetIndex` must be configured; if not, we have to
  inject the layout qualifiers by text rewriting (not tested).
- Measured only with glslang 12 CLIs and the spike-built newer glslang; shaderc not run in-process
  (Debian libshaderc.so.1 is unlinkable without sibling glslang libs, no headers).
- 3D LUT sampler variant not benchmarked; warm numbers were from a Release build at -O2 on the
  current transient bench host (same-boot comparisons only).

## Artifacts
Throwaway sources and binaries in `.../scratchpad/spike/`: `g.cpp` (glslang in-process timing),
`q.cpp` (QShaderBaker), `t.cpp` (shaderc attempt), `ocio.comp`, `ocio2.comp`, glslang build in `src/gb`.
