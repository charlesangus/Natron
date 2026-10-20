# M108 - Retire Engine GL Host Path

Nothing native or host-side renders through GL any more:
- **Host wrappers:** the host's wrappers around OFX GL-suite renders stop running on `eStorageModeGLTex` images: `Image::applyMaskMix` (`ImageMaskMix.cpp:216`), `copyUnProcessedChannels` (`ImageCopyChannels.cpp:483`), the premult (`ImagePremult.cpp:103`), convert, and the `fillZero` GL path (`Image.cpp:1445`), called from `EffectInstance.cpp:3278/3281`. A GL-suite plugin's output crosses to RAM (or to Vulkan, once M86 adds interop) right after the plugin call. The wrappers then run on the CPU or as `pixelwrap` kernels.
- **Shaders:** delete the mask-mix, copy-channels and fill shaders (`OSGLContext.cpp:509-645`), and with them `GLShader.*` where nothing else uses it.
- **Preferences:** the GL renderer chooser, the context count and the "OpenGL Rendering" switch (`Settings.cpp:556-600`) are replaced by a GPU page. It lists Vulkan devices (enumerated, replacing the free-text `gpuDevice` at `:425`), with the compute mode and budget moved from the Threading page. The OFX GL suite follows the compute mode and picks the GL renderer that matches the chosen Vulkan device's UUID. Hard cut: old settings are ignored.

## Scope

**In:**
- When a GL-suite plugin renders, the node's layer images (`renderMappedImage`/`downscaleImage`) are RAM. Only the plugin's own output (`LayerToRender::tmpImage`) is a temporary GL texture. It is read back to RAM right after `render_public`.
- Input images, including the mask, are pre-rendered in RAM. They are uploaded to GL only when the plugin calls `clipGetImage`, through the existing `convertRAMImageToOpenGLTexture`.
- Premult-back, copy-unprocessed-channels and mask/mix run on the CPU, on RAM images, for GL renders too.
- The GL branches are deleted from `ImageMaskMix.cpp`, `ImageCopyChannels.cpp`, `ImagePremult.cpp`, `ImageConvert.cpp`, and from `Image.cpp`'s `fill`, `fillZero`, `fillBoundsZero` and GL→GL `pasteFrom`. `convertOpenGLTextureToCachedRAMImage` is deleted.
- Deleted outright: the fill, mask-mix and copy-channels shaders, `GLShader.{h,cpp}` and `DefaultShaders.{h,cpp}`.
- A new "GPU" Preferences page holds:
  - GPU compute mode, moved from Threading.
  - GPU device: a choice that lists the enumerated Vulkan devices plus Automatic. The choice ID is the device UUID.
  - GPU memory budget, moved from Threading.
  - A read-only status line.
- Removed: the GL renderer chooser, `maxOpenGLContexts`, `enableOpenGLRendering`, the "Active OpenGL renderer" label and `--opengl`.
- The OFX GL suite is on when the resolved compute mode is not Off and a suitable GL renderer exists. It uses the GL renderer whose device UUID matches the chosen Vulkan device. When no UUID is available it matches the PCI vendor/device ID. Failing both, it uses the default renderer.
- A committed strict-GL test runner, and a GL-vs-CPU parity test of the host wrappers on a test GL effect.

**Out:**
- Vulkan interop for GL-suite output, wrappers as `pixelwrap` kernels on GL output, and the GL/Vulkan boundary at the plugin call. These belong to M86 - OFX 1.5 GPU Suites.
- Caching GL-rendered output, or allowing partial-rect renders for GL. The existing GL cache and full-RoI policy stays: `EffectInstanceRenderRoI.cpp:801-807, 1081`.
- (Un)premult-by on GL renders. It stays skipped, as today (`OfxClipInstance.cpp:993`, `EffectInstance.cpp:3154`).
- The project `gpuRendering` knob, the per-node `enableGPURendering` knob, and the per-plugin "OpenGL" column in the Preferences plugin table. All stay as they are.
- A headless EGL backend for `OSGLContext`, so that CI gets GL. This belongs to M68 - Headless GL EGL.
- Viewer/GUI GL (`ViewerGL*`, `Histogram`, `TrackerNodeInteract` textures, `GlInterop.*`). This belongs to M107 - GPU Viewer Hand-off.
- Any edit to `Engine/Gpu/GpuDevice.cpp`, `GpuContext.cpp`, `RenderScheduler.*`, `GpuRegionTask.*` (M109), `GpuWrap.*`/`pixelwrap.slang` (M137) or `GlInterop.*` (M107).

**Edge cases:**
- **GL node feeding a GL node (legacy pull):** each node now reads back and the next one re-uploads. This is slower than today's texture-to-texture chain. It is accepted until M86.
- **Plugin output components ≠ RGBA, or bit depth ≠ float:** read back into an RGBA-float RAM temp, then `convertToFormat` into the layer image.
- **Identity GL node:** the identity input is fetched in RAM. `fillZero` and `pasteFrom` run on RAM.
- **Mask input is multi-channel:** the existing RAM channel extraction (`EffectInstance.cpp:2614`) always applies, and `glMaskChannel` is deleted.
- **Caller asks `renderRoI` for `returnStorage == eStorageModeGLTex`** (a GL plugin fetching a non-pre-rendered input): the RAM→GL conversion at the end of `renderRoI` stays.
- **Compute mode Off:** the GL suite is off. `ePluginOpenGLRenderSupportYes` plugins render on the CPU. `ePluginOpenGLRenderSupportNeeded` plugins fail with a message naming the GPU compute preference. The `--opengl` hint goes.
- **Auto with only a software GL renderer (llvmpipe):** the GL suite is off, which matches Auto's "never software" rule. `NATRON_GPU=force` enables it on any renderer.
- **Background rendering (NatronRenderer):** the old "Disabled if background" default goes. The GL suite follows the compute mode. The project knob still offers "Disabled if background".
- **Saved device UUID no longer present** (card removed, or the driver update changed the UUID): restore finds no matching choice ID, so Automatic is used.
- **No Vulkan loader or no devices, or a build without `NATRON_HAVE_GPU`:** the device choice holds only Automatic. The GL suite can still be on, with the default GL renderer.
- **Vulkan device chosen but its UUID matches no GL renderer** (e.g. RADV beside llvmpipe GL under Xvfb): use the default renderer and say so in the status line and the log.
- **Old `gpuDevice` free-text value in QSettings:** it never matches a UUID ID, so Automatic is used. This is the hard cut.

**Assumptions:**
- `vkEnumeratePhysicalDevices` on a fresh instance returns devices in the same order that `GpuDevice::create` sees. A UUID can therefore be passed to `GpuDevice` as its index selector string, which is supported today at `GpuDevice.cpp:230-236`.
- A test-only `EffectInstance` that returns `ePluginOpenGLRenderSupportYes` from `supportsOpenGLRender()` and `true` from `isHostMaskingEnabled()`/`isHostMixingEnabled()`, with its last input a mask, gets the host mix knob, mask and channel selectors from `Node`, the same way an OFX plugin does.
- The default `attachOpenGLContext` needs nothing from a native effect.
- Creating one throwaway GL context per GLX renderer at startup, to read `GL_DEVICE_UUID_EXT`, costs little next to the current `getGPUInfos`.

**Scouted facts** (main `d521f111a`):
- GL render path, renderHandler:
  - The node's GL use is decided at `EffectInstanceRenderRoI.cpp:706-787`, and `storage = eStorageModeGLTex` is set at `:757`.
  - GL output is never cached (`:801-807`), and `useOpenGL` is set at `:840`. `attachOpenGLContext_public` and the detach are at `:1468-1503`.
  - At the end of renderRoI, GL↔RAM conversions run at `:1658-1677`.
  - `tiledRenderingFunctor` binds the output texture to the FBO and calls the plugin at `EffectInstance.cpp:3041-3091`.
  - The wrappers run at `:3134-3291`. The GL-context calls are `copyUnProcessedChannels(..., glContext)` at `:3285` and `applyMaskMix(..., glContext, glMaskChannel)` at `:3288`.
  - Inputs are pre-rendered as GL when `useOpenGL` (`:2590`). `glMaskChannel` is at `:2592-2617`.
  - The identity/fill GL calls are at `:2872, 2876, 2894, 2913, 2949, 2957`.
- The plugin's output clip image is `LayerToRender::tmpImage` (`OfxClipInstance.cpp:1142-1144`, assert at `:1205`).
- Input upload on fetch already exists: `EffectInstance.cpp:1323-1324, 1441-1442`. `convertRAMImageToOpenGLTexture` is at `:1898-1973`, and `convertOpenGLTextureToCachedRAMImage` at `:1871-1896`; the latter's only caller is `EffectInstanceRenderRoI.cpp:1676`.
- GL branches:
  - `ImageMaskMix.cpp:217-260`
  - `ImageCopyChannels.cpp:451-480` and `:544` (`extractChannels`)
  - `ImagePremult.cpp:103-106` (assert plus early return)
  - `ImageConvert.cpp:400` (assert only)
  - `Image.cpp:1223-1242` (GL→GL paste), `:1243-1310` (RAM→GL), `:1311-1360` (GL→RAM readback, to be kept), `:1463-1520` (fill shader), `:1525, 1571`
- Shaders:
  - The fill, mask-mix and copy-channels getters are at `OSGLContext.cpp:509-645`, with members at `:245-249`.
  - `GLShader` is used only by `OSGLContext.cpp`, `Image.cpp`, `ImageMaskMix.cpp`, `ImageCopyChannels.cpp` and `EngineFwd.h:158,355`.
  - `DefaultShaders.{h,cpp}` is used only by `OSGLContext.cpp:44`.
  - Gui uses `QOpenGLShaderProgram`, not `GLShader`.
- Settings:
  - Compute mode, free-text device and budget are on the Threading page at `Settings.cpp:407-437`, with defaults at `:1567-1569`.
  - The GL page is at `:548-592`, with defaults at `:1579-1585`.
  - `populateOpenGLRenderers`, `isOpenGLRenderingEnabled`, `getMaxOpenGLContexts` and `getActiveOpenGLRendererID` are at `:486-544`.
  - The renderer-info label is restored at `:2167-2207`, and the knob-changed hook is at `:2411-2415`.
  - Choice settings restore by option ID and ignore unknown IDs (`:2112-2127`).
- Startup: `AppManager.cpp:781-806` enumerates GL renderers, populates Settings, restores them, then builds `GpuContextOptions` from `getGpuComputeMode()`, `getGpuDevice()` and the budget.
- Readers of the GL settings:
  - `GPUContextPool.cpp:115-129, 204-228`
  - `Project.cpp:1399, 2039, 2446`
  - `EffectInstanceRenderRoI.cpp:776-777`
  - `CLArgs.cpp:317-322, 893-916` (`--opengl`)
  - `Tests/GLScheduler_Test.cpp:113-190` (guards toggle the `enableOpenGLRendering` and `maxOpenGLContexts` knobs)
  - `Tests/OSGLContext_Test.cpp:65`
- Vulkan device selection:
  - There is no public enumeration API. Enumeration and the selector live inside `GpuDevice::create` (`Engine/Gpu/GpuDevice.cpp:211-262`), where the selector is an index or a name substring and an empty one falls back to `NATRON_GPU_DEVICE`.
  - `Engine/Gpu/*.cpp` sources are listed explicitly (`Engine/Gpu/CMakeLists.txt:49-66`), while `Engine/*.cpp` is globbed without CONFIGURE_DEPENDS (`Engine/CMakeLists.txt:36-37`). Deleted or new Engine files therefore need the phase build's fresh configure.
- GL renderer enumeration (`OSGLContext_x11.cpp:912-1030`):
  - With `GLX_MESA_query_renderer`, it lists only accelerated renderers and reads PCI vendor/device IDs.
  - Without it, it lists the single default renderer.
  - A GL device-UUID query exists only in `Engine/Gpu/GlInterop.cpp:231-248` (an M107 file, and it needs a live `GpuDevice`).
- Bundled GL-suite plugins:
  - openfx-io `OCIOColorSpace` and the other OCIO plugins (`setSupportsOpenGLRender(true)`, with mask/mix done inside the plugin by `ofxsMaskMix`).
  - openfx-misc `Shadertoy` (in `Misc.ofx.bundle`).
  - `TestOpenGL` is not built into the bundle.
  - None of the bundled plugins sets `kNatronOfxImageEffectPropHostMasking`/`HostMixing`, so host mask/mix on a GL OFX plugin cannot be exercised with bundled plugins. A registered test effect can exercise it (`Tests/wmain.cpp` `registerTestBuiltInPlugin<T>`).
- GL in tests:
  - CI has no GLX under xvfb-run and no EGL (`.plan/PLAN/DECISIONS/2026-09-02-no-glx-under-xvfb.md`), so GL tests skip there unless `NATRON_TEST_REQUIRE_GL=1`.
  - The strict runner `build/m63-gui/run-gl-tests.sh` that `GLScheduler_Test.cpp:72-76` names does not exist in the tree.
  - `Tests/gui/run-gui-test.sh:116-140` has the working natron-dev Xvfb+GLX recipe (a GL lib symlink dir, plus Xvfb against `/usr/lib64`).
- Tests layout: root `Tests/*_Test.cpp` files are listed explicitly in `Tests/CMakeLists.txt`. `Tests/Native/*_Test.cpp` files are globbed (`:132`).
- The GUI bench rule is **not triggered**, because no `Gui/NodeGraph*`, `NodeGui*`, `Edge*`, knob-expression or node-naming code is touched. `Gui/PreferencesPanel.cpp` builds pages generically from Settings knobs.

## Phase 108.1: GL output crosses to RAM; host wrappers CPU-only

- [x] M108.P1.T1 — Commit a strict GL ctest runner for natron-dev
  - files: `Tests/gui/run-gl-tests.sh` (new)
  - approach: Factor the Xvfb+GLX setup of `Tests/gui/run-gui-test.sh:116-140` into a runner for the gtest binary: the `/usr/lib64` GL symlink dir, Xvfb against `/usr/lib64`, and `xset` readiness. Usage: `Tests/gui/run-gl-tests.sh <gtest-filter> [Tests binary, default build/release/Tests/Tests]`. Export `NATRON_TEST_REQUIRE_GL=1`, `LIBGL_ALWAYS_SOFTWARE=1`, `__GLX_VENDOR_LIBRARY_NAME=mesa`, and `OFX_PLUGIN_PATH=build/assets/Plugins` (the same plugin path `tools/ci/local/test.sh` uses), and keep the ASWF library path for everything else. Exit with the gtest status.
  - verify: `bash -n` and `shellcheck` are clean. In natron-dev on the current main build, `Tests/gui/run-gl-tests.sh 'GLScheduler.*'` runs the three GLScheduler cases, and none of them prints `[  SKIPPED ]`.
  - size: M

- [x] M108.P1.T2 — Render GL-suite plugins into a temporary texture and read back to RAM before the wrappers
  - files: `Engine/EffectInstanceRenderRoI.cpp`, `Engine/EffectInstance.cpp`, `Engine/EffectInstancePrivate.h`, `Engine/EffectInstance.h`
  - approach:
    - In `renderRoI`, separate "renders via GL" from storage. `storage` stays `eStorageModeRAM`, and `layersToRender->useOpenGL` carries the GL decision. Replace every `storage == eStorageModeGLTex` test (`:757, 801, 1469, 1497`) with `useOpenGL`, and keep the existing GL policies: no cache, full RoI, render scale and the max-texture-size fallback. Delete the GL→RAM branch at `:1672-1677`, and keep the RAM→GL branch for GL-storage callers.
    - In `tiledRenderingFunctor`, pre-render inputs in RAM: pass `false` for GL at `:2590`. Delete `glMaskChannel` (`:2592-2617`, its parameter at `:2700/2784`, and `EffectInstancePrivate.h:429`). The identity and fill path (`:2872-2957`) requests and fills RAM.
    - When `useOpenGL`, allocate `tmpImage` as a GL-texture image over the roi, with RGBA float in the same params form `convertRAMImageToOpenGLTexture` uses. Bind that texture instead of the layer image at `:3049-3055`. After `render_public`, read it back while the context is still bound: `pasteFrom(GL→RAM, glContext)` into the RAM layer image, or into an RGBA-float RAM temp followed by `convertToFormat` when components or depth differ.
    - The wrappers then run on RAM with no GL context. Enable `reUnPremult` only for non-GL renders, as today. Delete `convertOpenGLTextureToCachedRAMImage`.
  - verify: Phase build. ctest `GLScheduler.*`, `RenderScheduler*`, `ViewerScheduler*` and `ChannelSetRender*` are green on CI (GLScheduler skips there). `Tests/gui/run-gl-tests.sh 'GLScheduler.*'` is green in natron-dev.
  - size: L

- [x] M108.P1.T3 — Delete the GL branches of the mask-mix and copy-channels wrappers
  - files: `Engine/Image.h`, `Engine/ImageMaskMix.cpp`, `Engine/ImageCopyChannels.cpp`
  - approach: Remove the `eStorageModeGLTex` blocks (`ImageMaskMix.cpp:217-260`, `ImageCopyChannels.cpp:451-480` and the `extractChannels` check at `:544`), the `GLShader.h`/`OSGLContext.h` includes, and the `glContext`/`glMaskChannel` parameters of `applyMaskMix` and `copyUnProcessedChannels` in `Image.h`. P1.T2 stopped passing them, so the callers already compile without them. Delete the mask assert's GL clause.
  - verify: Phase build. `grep -n 'eStorageModeGLTex\|getGLTexture\|glContext\|GLShader' Engine/ImageMaskMix.cpp Engine/ImageCopyChannels.cpp` is empty. ctest `ChannelSetRender*` and `Native*` mask/mix cases are green.
  - size: M

- [x] M108.P1.T4 — Delete GL fill, GL→GL paste and the premult/convert GL checks
  - files: `Engine/Image.h`, `Engine/Image.cpp`, `Engine/ImagePremult.cpp`, `Engine/ImageConvert.cpp`
  - approach:
    - Delete the GL branches of `fill`/`fillZero`/`fillBoundsZero` (`Image.cpp:1463-1520, 1525, 1571`) and their `glContext` parameters.
    - Delete the GL→GL branch of `pasteFrom` (`:1223-1242`). Keep RAM↔GL (`:1243-1360`) and its `glContext` parameter, since that is the plugin-boundary readback and upload.
    - Delete the `ImagePremult.cpp:100-106` GL assert and early return, and the `ImageConvert.cpp:400` assert. Drop the now-unused includes.
  - verify: Phase build. `grep -n 'eStorageModeGLTex\|GLShader' Engine/ImagePremult.cpp Engine/ImageConvert.cpp` is empty. `Image.cpp` has no `getOrCreateFillShader`. ctest `Image_Test`, `ImageConvert_Test` and `ImagePasteBands_Test` are green.
  - size: M

- [x] M108.P1.T5 — Delete the engine GL shaders and GLShader
  - files: `Engine/OSGLContext.h`, `Engine/OSGLContext.cpp`, `Engine/GLShader.h` (delete), `Engine/GLShader.cpp` (delete), `Engine/DefaultShaders.h` (delete), `Engine/DefaultShaders.cpp` (delete), `Engine/EngineFwd.h`
  - approach: Remove `getOrCreateFillShader`, `getOrCreateMaskMixShader` and `getOrCreateCopyUnprocessedChannelsShader` (`OSGLContext.cpp:509-645`, the declarations at `OSGLContext.h:176-178`), the shader members (`:245-249, 266-268`), and the `DefaultShaders.h`/`GLShader.h` includes. Delete the four files and the `GLShader` forward declaration and typedef (`EngineFwd.h:158, 355`). This is a mechanical deletion and needs the phase build's fresh configure, because the Engine GLOB has no CONFIGURE_DEPENDS.
  - verify: Phase build from a fresh configure. `grep -rn 'GLShader\|DefaultShaders\|getOrCreate.*Shader' Engine Gui Tests` is empty.
  - size: S

- [x] M108.P1.T6 — GL-vs-CPU parity test for host mask, mix and channel subset
  - files: `Tests/Native/GlHostWrapTestEffect.h` (new), `Tests/Native/GlHostWrap_Test.cpp` (new), `Tests/wmain.cpp`
  - approach:
    - The test effect (registered in `wmain.cpp` with one `registerTestBuiltInPlugin` line) returns `ePluginOpenGLRenderSupportYes`, enables host masking and mixing, and makes its last input a mask.
    - In `render()`, when `args.useOpenGL` is set, it does `glClearColor(C) + glClear` into the bound FBO (the host already set the scissor to the roi) and counts GL renders. Otherwise it fills the output with C.
    - Cases, each rendered twice (GL with the guards from `GLScheduler_Test`, then CPU with the node's `enableGPURendering` set to Disabled), compared exactly:
      - (a) mix 0.5, no mask
      - (b) a ramp mask with mix 1
      - (c) process R and A only, so copy-unprocessed restores G and B from the input
      - (d) all three together
    - The source is `ParitySourceTestEffect`, so values include negatives and super-whites.
    - Assert that the GL pass rendered through GL (counter > 0) and that the output is `eStorageModeRAM`. Skip through the `skipWithoutOpenGL` pattern unless `NATRON_TEST_REQUIRE_GL=1`.
  - verify: Phase build. `Tests/gui/run-gl-tests.sh 'GlHostWrap.*'` passes in natron-dev, with no skip. On CI the cases print a skip and pass.
  - size: M

## Phase 108.2: GPU preferences page and the GL suite following the compute mode

- [x] M108.P2.T1 — Enumerate Vulkan devices without touching GpuDevice
  - files: `Engine/GpuDeviceList.h` (new), `Engine/GpuDeviceList.cpp` (new), `Tests/Native/GpuDeviceList_Test.cpp` (new)
  - approach:
    - `struct GpuDeviceSummary { int index; std::string name; VkPhysicalDeviceType type; std::array<uint8_t,16> uuid; uint32_t vendorID, deviceID; }`.
    - `std::vector<GpuDeviceSummary> enumerateGpuDevices()` creates a throwaway `VkInstance` (API 1.3, no layers), reads `VkPhysicalDeviceIDProperties` for each device, and destroys the instance.
    - It returns an empty list without `NATRON_HAVE_GPU`, with no loader, or when instance creation fails.
    - Helpers: `std::string uuidHex(uuid)` (32 lowercase hex characters), and `std::string gpuSelectorForDeviceId(list, id)`, which returns the decimal index or `""` for `"auto"` or an unknown ID.
  - verify: Phase build. On CI (lavapipe), `GpuDeviceList.*` checks that the list holds at least one device, that a CPU-type entry exists, that each UUID is 32 hex characters, and that the selector round-trips. Opening a `gpu::GpuDevice` with the selector built from lavapipe's index must report lavapipe's name, which checks the index-order assumption.
  - size: M

- [x] M108.P2.T2 — Give GL renderers a device identity and a pure renderer pick
  - files: `Engine/OSGLContext.h`, `Engine/OSGLContext.cpp`, `Engine/OSGLContext_x11.cpp`, `Engine/OSGLContext_wayland.cpp`, `Tests/Native/GlRendererPick_Test.cpp` (new)
  - approach:
    - Add the fields `std::array<uint8_t,16> deviceUuid; bool hasUuid; uint32_t pciVendorID, pciDeviceID; bool accelerated` to `OpenGLRendererInfo`.
    - In `getGPUInfos` (x11 and wayland), stop dropping non-accelerated renderers and record `accelerated` instead. Fill the PCI IDs from `GLX_RENDERER_VENDOR_ID_MESA`/`DEVICE_ID_MESA`. Read `GL_DEVICE_UUID_EXT` (index 0) on a context made current per renderer, loading `glGetUnsignedBytei_vEXT` through the platform's getProcAddress, but only when `GL_EXT_memory_object` or `GL_EXT_semaphore` is present.
    - Add `static GLRendererPick OSGLContext::pickRenderer(const std::list<OpenGLRendererInfo>&, const GpuDeviceSummary* chosenOrNull, bool allowSoftware)`, returning `{enabled, GLRendererID, reason}`. It matches by UUID first, then by PCI IDs, then falls back to the first renderer allowed under `allowSoftware`. It is disabled when no renderer is allowed. `reason` explains the result for the status line.
  - verify: Phase build. The `GlRendererPick.*` table tests (pure data, no GL) cover a UUID match, a PCI match, the default fallback, software refused under Auto and accepted under Force, and an empty list.
  - size: L

- [ ] M108.P2.T3 — ⚑ GPUContextPool owns the GL-suite state, set once at startup (after M137 - Native Base Class Flexibility merges: `AppManager.cpp`)
  - files: `Engine/GPUContextPool.h`, `Engine/GPUContextPool.cpp`, `Engine/AppManager.cpp`
  - approach:
    - Add `GPUContextPool::setRenderingState(bool enabled, GLRendererID, std::string status)`, `isRenderingEnabled()`, `getRendererStatus()`, and the constant `kMaxRenderGLContexts = 2` (the old default). Add the test overrides `setRenderingEnabledForTests(std::optional<bool>)` and `setMaxContextsForTests(std::optional<int>)`.
    - `attachGLContextToRender` and `getOrCreateContextForCurrentThread` use these in place of the Settings accessors.
    - In `AppManager` startup (`:781-806`): call `enumerateGpuDevices()` and resolve the mode with `GpuContext::resolveMode(setting, NATRON_GPU)`.
    - Map the chosen device ID to a selector with `gpuSelectorForDeviceId`, and pass it as `GpuContextOptions.device`.
    - Call `pickRenderer` with the chosen device's summary and `allowSoftware = (mode == Force)`, setting `enabled = mode != Off && pick.enabled && isOpenGLLoaded()`. Store the result in the pool.
    - Log one info line naming the Preferences device and the GL renderer picked.
    - For now, keep reading `Settings::getGpuDevice()`; P2.T5 changes its type.
  - verify: Phase build. ctest `GpuContext_Test` and `GpuDeviceList.*` are green. Starting `NatronRenderer -h` in natron-dev logs the device/renderer line.
  - size: M

- [ ] M108.P2.T4 — Switch the GL-enable readers and the GL tests to the pool
  - files: `Engine/Project.cpp`, `Engine/EffectInstanceRenderRoI.cpp`, `Tests/GLScheduler_Test.cpp`, `Tests/OSGLContext_Test.cpp`
  - approach:
    - `Project.cpp:1399, 2039, 2446` read `appPTR->getGPUContextPool()->isRenderingEnabled()`.
    - `EffectInstanceRenderRoI.cpp:776-777`: the message for a disabled `…Needed` plugin names the GPU compute preference and `NATRON_GPU`, not `--opengl`.
    - `GLScheduler_Test`:
      - `OpenGLRenderingGuard` uses `setRenderingEnabledForTests(true)` plus the project knob, and `MaxOpenGLContextsGuard` uses `setMaxContextsForTests`. Both restore on exit.
      - Fix the runner path in `kNoOpenGLMessage` to `Tests/gui/run-gl-tests.sh`.
      - Drop the `Settings::eEnableOpenGL*` and settings-knob uses.
    - `OSGLContext_Test.cpp:65` uses the pool's renderer ID.
  - verify: Phase build. CI ctest is green. `Tests/gui/run-gl-tests.sh 'GLScheduler.*:GlHostWrap.*'` is green in natron-dev.
  - size: M

- [ ] M108.P2.T5 — ⚑ Replace the GL preferences with a GPU page and remove `--opengl` (after M137 - Native Base Class Flexibility merges: `AppManager.cpp`)
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Engine/AppManager.cpp`, `Engine/AppManager.h`, `Engine/CLArgs.cpp`
  - approach:
    - Delete the following from Settings, together with the renderer-info restore block (`:2167-2207`), the knob-changed hook (`:2411-2415`) and the GL defaults (`:1579-1585`):
      - the knobs `_openglRendererString`, `_availableOpenGLRenderers`, `_nOpenGLContexts` and `_enableOpenGL`
      - `EnableOpenGLEnum`
      - `populateOpenGLRenderers`, `isOpenGLRenderingEnabled`, `getMaxOpenGLContexts` and `getActiveOpenGLRendererID`
    - `initializeKnobsGPU` creates the page "GPU" holding:
      - `gpuComputeMode`, moved off Threading
      - `gpuDevice`, now a `KnobChoice`: an "auto" (Automatic) entry first, then one entry per device with id = `uuidHex`, label = "name (discrete/integrated/CPU…)", filled by a new `populateGpuDevices(list)`
      - `gpuMemoryBudgetMB`, moved off Threading
      - a read-only `gpuStatus` label showing the pool's renderer status
    - Each tooltip says the change takes effect at the next launch. `getGpuDevice()` returns the active entry's ID ("auto" maps to "").
    - `AppManager` calls `populateGpuDevices` before `restoreSettings`, in place of `populateOpenGLRenderers` (`:791`).
    - Delete `refreshOpenGLRenderingFlagOnAllInstances` (`AppManager.h:636`, `.cpp:3537`).
    - Delete `--opengl` and its help text (`CLArgs.cpp:317-322, 893-916`), and change the `--setting` example to `gpuComputeMode`.
  - verify: Phase build. `Tests/Native/GpuPreferences_Test.cpp` (new) is green on CI. It checks that:
    - the "GPU" page holds the three knobs, and Threading does not
    - `gpuDevice` is a choice whose first entry is "auto", with one entry per enumerated device
    - no knob is named `enableOpenGLRendering`, `maxOpenGLContexts`, `chooseOpenGLRenderer` or `activeOpenGLRenderer`
    - `CLArgs` rejects `--opengl` (if `CLArgs_Test` covers parsing, extend it instead)
  - size: L

- [ ] M108.P2.T6 — UAT script for the GPU Preferences page
  - files: `Tests/gui/m108_gpu_prefs.py` (new)
  - approach: In the `guitest.py` style, following the Preferences opening in `m56_visual.py:1329`:
    - Open Preferences and select the "GPU" page.
    - Check that it shows GPU compute, GPU device and GPU memory budget, and that the device combo lists Automatic plus each Vulkan device. Under lavapipe that means an entry containing "llvmpipe".
    - Check that Threading no longer shows those knobs, and that nothing reads "OpenGL renderer", "No. of OpenGL Contexts" or "OpenGL Rendering".
    - Pick the llvmpipe entry, save, and assert that `natron.getSettings().getParam("gpuDevice")` holds its UUID ID.
    - Screenshot the page.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m108_gpu_prefs.py build/remote/fast/<name>.AppImage` (or the local release binary) exits 0 in natron-dev. Then the next-launch check, done by hand: with `HOME` holding a `Natron.conf` that has `gpuDevice=<lavapipe uuid>` and `VK_ICD_FILENAMES` listing RADV and lavapipe, `NatronRenderer -h` logs lavapipe as the Preferences device.
  - size: M

**Verification gate:**
- Both phase builds are green on remote CI (`tools/ci/remote/build.sh fast --tests`, lavapipe, both scheduler legs), including `GpuDeviceList.*`, `GlRendererPick.*` and `GpuPreferences.*`.
- In natron-dev (local `tools/ci/local/build.sh release`, RADV + llvmpipe), `Tests/gui/run-gl-tests.sh 'GLScheduler.*:GlHostWrap.*'` passes with `NATRON_TEST_REQUIRE_GL=1` and no skips. This shows that a GL-suite render with mask, mix and a channel subset matches its CPU render exactly.
- `grep -n 'eStorageModeGLTex\|getGLTexture\|GLShader\|OSGLContext' Engine/ImageMaskMix.cpp Engine/ImageCopyChannels.cpp Engine/ImagePremult.cpp Engine/ImageConvert.cpp` is empty.
- `grep -rn 'GLShader\|DefaultShaders\|getOrCreate.*Shader\|enableOpenGLRendering\|maxOpenGLContexts\|chooseOpenGLRenderer' Engine Gui Tests` is empty.
- `Tests/gui/m108_gpu_prefs.py` passes under Xvfb, and the next-launch log check shows the chosen device.

## Decisions

- 2026-10-20 — **Claimed by the gpu PM as the third lane, overlap with M64 accepted (user).** M108 will touch `Engine/Settings.cpp` and `Engine/EffectInstance.cpp`, which M64 - Tiled Rendering (blocked, dropped for now) also lists. The user chose to run M108 anyway; M64 rebases over it if it resumes. The M86 boundary blocker resolves as the stub already says: M108 runs first, and a GL-suite plugin's output crosses to RAM right after the plugin call, leaving Vulkan interop to M86 - OFX 1.5 GPU Suites. M85 is merged, so the wrappers may run as `pixelwrap` kernels.
- 2026-10-20 — **The wrappers on GL output run on the CPU, not as `pixelwrap` kernels (planner).** The output is a RAM image the size of the render window, so moving it to Vulkan would add an upload and a download for a point op. `GpuWrap.*`/`pixelwrap.slang` are M137's files. Kernels on GL output come with M86's interop.
- 2026-10-20 — **Vulkan devices are enumerated in a new `Engine/GpuDeviceList.cpp`, and the choice reaches `GpuDevice` as its existing index selector (planner).** This keeps M109's `GpuDevice.cpp` and the explicit `Engine/Gpu/CMakeLists.txt` list untouched. The setting stores the device UUID, which stays stable across reboots, while the index does not.
- 2026-10-20 — **The parity test uses a registered test GL effect, not a bundled OFX plugin (planner).** No bundled GL-suite plugin declares host masking or mixing, and CI has no GLX, so the case is strict only through `Tests/gui/run-gl-tests.sh` in natron-dev.
- 2026-10-20 — **Elaborated at claim by a consultant on main `d521f111a`.** No task touches a file of M107 - GPU Viewer Hand-off, M109 - GPU Region Engine Hardening or M78 - Metadata In Expressions. The one overlap with a running milestone is `Engine/AppManager.cpp` with M137 - Native Base Class Flexibility (another PM's; M137 edits `loadBuiltinNodePlugins` ~1620-1697, M108 the startup block ~781-806 and ~3537). M137 is later on the board, so by the claim rule it would be the one to wait; rather than stall another PM's running milestone, the two tasks that edit `AppManager.cpp`, P2.T3 and P2.T5, are flagged ⚑ to start after M137 merges. `Tests/wmain.cpp` gets one registration line (P1.T6).
- 2026-10-20 — **Three design calls (user, on the consultant's recommendations):** GL-rendered output keeps today's no-cache, full-RoI policy, revisited in M86 - OFX 1.5 GPU Suites; under Auto the GL suite never uses a software GL renderer, and only `NATRON_GPU=force` enables llvmpipe; `--opengl` is removed outright (hard cut).
- 2026-10-20 — **M108.P1.T1 (`ec34cf6dc`):** `Tests/gui/run-gl-tests.sh` uses display 78 (override `GL_TEST_DISPLAY`) so it never clashes with the GUI runner's 79, and takes `OFX_PLUGIN_PATH` from `run-gui-test.sh`. Only `bash -n` has run; shellcheck isn't on the host. The `GLScheduler.*` no-skip run is owed to the Phase 108.1 build.
- 2026-10-20 — **M108.P1.T2 (`a8167fb7d`), syntax-checked only.** `renderRoI` carries a separate `useOpenGL` and keeps `storage` RAM, so the cache lookup, input pre-render and layer allocation are all RAM. Readback is the static `readBackOpenGLRender` (`EffectInstance.cpp` ~2740). It pastes straight into a float layer with matching components; otherwise it reads into a float temp with the plugin's components and calls `convertToFormat`. Float, because `pasteFrom` sizes its buffer from the destination depth while reading `GL_FLOAT`. Layers are matched by image pointer, not key. The `useOpenGL` argument at ~2590 was left as is: it only feeds `isDoingOpenGLRender`, which nothing reads. The GL branches of `getImageFromCacheAndConvertIfNeeded` are now dead but stay, since `RotoContext.cpp` shares the signature. Phase build to check: the texture is freed after the render's own attacher detaches, relying on renderRoI's `glContextLocker` keeping the context current.
- 2026-10-20 — **M108.P1.T3 and P1.T4 landed as one commit (`514bde9c2`)** because both edit the same `Image.h` declarations; syntax-checked only (NDEBUG, so asserts unexercised). `applyMaskMix` now takes a one-channel mask, and its `maskChannel` parameter is gone. `Image.cpp` keeps its `GLShader.h`/`OSGLContext.h`/`GPUContextPool.h` includes, because the kept RAM↔GL `pasteFrom` branches use `getFBOId()`/`getPBOId()` and `glCheckError`; P1.T5 must leave `OSGLContext.h` available to it. No caller needed changes.
- 2026-10-20 — **M108.P1.T6 (`7a021513f`), syntax-checked only.** The test effect is a `NativeEffectBase` with a "Source" input and an optional "Mask" input. Host knobs come from Node: the mask selectors from `isInputMask`, `hostMix` from `isHostMixingEnabled`, `channels` from `getLayerKnobSpec`, and `enableGPURendering` from `registerTestBuiltInPlugin`. The GL pass goes through the scheduler, because the legacy direct path passes no GL context. The mask is the parity source's alpha (exact 0, exact 1 and a ramp). Phase build to check: the scheduler actually creates a GL context for a root node (the GL-render counter assertion shows it), and `glClear` into float32 keeps negatives and values above 1.
- 2026-10-20 — **M108.P2.T1 (`15a78afbe`), syntax-checked only.** `GpuDeviceList.h` includes no Vulkan header and has its own `GpuDeviceType` enum, with the same values as `VkPhysicalDeviceType`. Enumeration calls the linked Vulkan loader directly, as `GpuDevice.cpp` does (no volk, no dlopen). The index-order test skips any device that `GpuDevice::create` refuses for missing features. It needs the phase build's fresh configure, because `Engine/*.cpp` is globbed.
- 2026-10-20 — **M108.P2.T2 (`7844fa423`), syntax-checked only (the Wayland branch couldn't be compiled here; `__NATRON_WAYLAND__` is off in release).** `Engine/Settings.cpp` was also edited, though not in the task's file list. Its chooser, `getActiveOpenGLRendererID` and the info panel index into the renderer list, which now also includes software renderers. A file-static `chooserRenderers()` keeps `accelerated || renderID == -1`, exactly the old list; P2.T5 deletes it with the chooser. The UUID is read on the context already current per renderer; no extra context is created. A device ID of `0xFFFFFFFF` now means "no PCI identity" (stored as 0) instead of ending enumeration. A software renderer is refused without `allowSoftware`, even when its UUID matches. Display-only lists (the AppManager log, the About window) may now show llvmpipe.
