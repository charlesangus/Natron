# M203 - ONNX Inference Backend

A self-contained neural inference layer in `Engine/Ai/`, modelled on darktable 5.6's `src/ai/backend_onnx.c` and `backend_common.c` (PR #20322, `dev-doc/AI.md`). ONNX Runtime (MIT) is loaded with `dlopen` through its C API (`OrtGetApiBase`), so the CPU build bundled in the AppImage and a GPU build the user installs later are interchangeable. The execution provider (EP) is chosen at runtime. Auto mode tries CUDA, then MIGraphX, then ROCm, then CPU, each probed by symbol lookup as darktable does. Inference sits beside the Vulkan backend: tensors go through host memory, and every AI node is CPU-only in its `NodeSpec`, so it ends a GPU region. The layer serves every E16 - AI Tools node. M204 - AI Model Manager supplies the models.

See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- Vendoring a pinned ONNX Runtime CPU release, staged as a shared object in the AppImage, with licence entries.
- `dlopen` loading:
  - Library search order: Preferences path, then the user data dir's `onnxruntime/`, then the bundled copy.
  - Version checks against the ORT API version the code was written for.
- EP probing and selection (Auto, CPU, CUDA, MIGraphX, ROCm, OpenVINO), a GPU device index, and a thread count.
- A session cache keyed by model path, EP and device.
- Float32 and float16 inputs and outputs, with automatic conversion. Dynamic output shapes. A graph optimisation level taken from the model manifest.
- A tiled-inference helper: tile size, overlap, feathered blending, padding to a multiple the model needs, and an abort hook.
- A cap on concurrent runs per device, so parallel renders can't exhaust VRAM.
- An Abort through ORT's `RunOptionsSetTerminate`.
- Preferences: an "AI" page with Enable AI (default off), Device, GPU index, Threads, ONNX Runtime library path, and a read-only "detected providers" line.
- `tools/ai/install-ort-gpu.sh`, which installs a CUDA or ROCm/MIGraphX ONNX Runtime build into the user data dir.
- Tiny generated test models, a test suite, and a README section.

**Out:**
- Models, manifests and downloading: M204 - AI Model Manager.
- Any node: M205–M208.
- Zero-copy Vulkan↔CUDA/HIP sharing, and porting networks to Slang: no milestone owns them.
- TensorRT tuning caches and an OpenVINO-specific UI.
- darktable's remote-inference PR #22624, its Lua AI API and its MCP server.

**Edge cases:**
- **No ONNX Runtime found, or an ABI mismatch:** AI reports itself unavailable with the reason. AI nodes show a persistent error, and nothing crashes.
- **A requested EP is missing** (for example CUDA on a CPU build): fall back to CPU with a one-time warning naming the EP.
- **A CUDA EP that loads but has no cuDNN or driver:** session creation fails. The session is retried on CPU and the failure is logged.
- **Out of memory on the GPU during a run:** retry once with half the tile size, then fall back to CPU, then error.
- **An aborted render:** the run terminates, and the partial tile is discarded and never cached.
- **Enable AI off:** nothing is loaded, and AI nodes show "AI is disabled in Preferences".

**Assumptions:**
- ORT's official Linux x64 CPU tarball runs on the EL9 baseline (glibc 2.34).
- The Python `onnx` package can be pip-installed in natron-dev to generate test models. The generated `.onnx` files are committed, so the build never needs it.

## Phase 203.1: Runtime loading

- [x] M203.P1.T1 — Vendor ONNX Runtime CPU and stage it in the bundle
  - files: `tools/ci/local/fetch-assets.sh`, `CMakeLists.txt`, `tools/release/stage-bundle.sh`, `tools/license/components/README.md`, new `tools/license/components/LICENSE-onnxruntime.txt`
  - approach:
    - Pin `ONNXRUNTIME_VERSION` plus a sha256 of the official `onnxruntime-linux-x64-<ver>.tgz`. Follow the rawspeed pin and stamp recipe (`fetch-assets.sh:254-273`) into `build/assets/onnxruntime`.
    - Only the headers are used at compile time: the code includes `onnxruntime_c_api.h` and never links the library. Expose the header path as `NATRON_ONNXRUNTIME_ROOT` (like `NATRON_RAWSPEED_ROOT`, root `CMakeLists.txt:~96-105`).
    - `stage-bundle.sh` copies `libonnxruntime.so*` into the bundle's `lib/onnxruntime/`.
    - Licence: MIT, plus ORT's ThirdPartyNotices.
  - verify:
    - A second fetch run is a no-op.
    - After `tools/release/package.sh`, the extracted AppImage contains `lib/onnxruntime/libonnxruntime.so.<ver>`.
    - The licence README lists it.
  - size: M

- [x] M203.P1.T2 — dlopen loader and provider probe
  - files: new `Engine/Ai/OrtLoader.h`, `Engine/Ai/OrtLoader.cpp`, `Engine/CMakeLists.txt`
  - approach:
    - A process-wide singleton loads the library in the Scope's search order with `dlopen(RTLD_NOW|RTLD_LOCAL)`, gets `OrtGetApiBase()->GetApi(ORT_API_VERSION)`, and creates one `OrtEnv`.
    - It exposes `available()`, `unavailableReason()`, `versionString()` and `providers()`. `providers()` is the list from `GetAvailableProviders`, and each GPU EP's append function is probed by `dlsym` (the logic is in darktable `backend_onnx.c`'s provider discovery).
    - The loader never runs at startup. The first use loads it, and only when the Preferences "Enable AI" is on.
  - verify: `Tests/Ai/OrtLoader_Test.cpp` (P3.T1) loads the bundled build-tree copy and lists `CPUExecutionProvider`. A bogus library path yields `available()==false` with that path in the reason.
  - size: M

- [x] M203.P1.T3 — AI preferences page
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`
  - approach:
    - Add an "AI" tab with these knobs: `aiEnabled` (bool, off), `aiDevice` (Auto, CPU, CUDA, MIGraphX, ROCm, OpenVINO), `aiGpuIndex` (int 0), `aiThreads` (0 = auto), `aiOrtLibraryPath` (`KnobPath`, single path, like `_diskCachePath` at `Settings.cpp:1384-1396`), and a read-only `aiDetectedProviders` string refreshed on open. A `NATRON_AI_DEVICE` env override applies to farm use.
    - Accessors go beside `isDevelopRawOnReadEnabled` (`Settings.cpp:3308`).
  - verify: gtest `Tests/Ai/AiSettings_Test.cpp`: defaults, env override, and serialisation round trip.
  - size: M

## Phase 203.2: Sessions and tiled inference

- [x] M203.P2.T1 — Inference session wrapper
  - files: new `Engine/Ai/InferenceSession.h`, `Engine/Ai/InferenceSession.cpp`, new `Engine/Ai/Tensor.h`
  - approach:
    - Write `InferenceSession::open(modelPath, SessionOptions{ep, device, threads, optLevel})`. EP fallback follows the Scope's edge cases.
    - `Tensor` is an owning NCHW float buffer with shape. Implement `run(inputs) -> outputs` by name, and convert float16 on input and output when the model's types say so (darktable's float16 conversion).
    - Dynamic output shapes are read after the run.
    - A process-wide `SessionCache` is keyed by (path, mtime, EP, device) and evicted LRU at a fixed count. ORT `Run` is thread-safe, so a session is shared.
    - A per-device counting semaphore (default 1 for GPU, `aiThreads`-bounded for CPU) gates `run`.
  - verify: P3.T1's identity and scale models run with float32 and float16 IO and produce exact outputs. Two threads sharing a session give identical results.
  - size: L

- [x] M203.P2.T2 — Abort and OOM handling
  - files: `Engine/Ai/InferenceSession.cpp`, `Engine/Ai/InferenceSession.h`
  - approach:
    - `run` takes an abort predicate. A watcher thread polls it every 20 ms and calls `RunOptionsSetTerminate`.
    - An ORT out-of-memory status maps to `AiOutOfMemory`, which the tiler (P2.T3) handles. Any other failure maps to `AiError` with ORT's message.
  - verify: a test model with a long loop (`Loop` op, P3.T1) is aborted within 200 ms of the predicate flipping. A forced OOM status (test hook) surfaces as `AiOutOfMemory`.
  - size: M

- [x] M203.P2.T3 — Tiled inference helper
  - files: new `Engine/Ai/TiledInference.h`, `Engine/Ai/TiledInference.cpp`
  - approach:
    - `runTiled(session, image RoI, TileSpec{tile, overlap, multipleOf, scale, inChannels, outChannels}, pack, unpack, abort)`.
    - Tiles are padded by reflection to `multipleOf`. Overlap is blended with a raised-cosine weight. An output `scale` ≠ 1 serves the upscalers.
    - Implement the OOM policy from the Scope: halve the tile once, then fall back to CPU, then error.
    - `pack`/`unpack` are caller callbacks that convert the node's pixels to and from the model's tensor layout, so each node owns its colour handling.
  - verify: an identity model run tiled over an odd-sized 1000×700 image with overlap 32 reproduces the input exactly. A 2× nearest model gives the exact 2× image. The OOM hook triggers the halving path.
  - size: L

## Phase 203.3: Tests, tooling and docs

- [x] M203.P3.T1 — Generated test models
  - files: new `tools/ai/make-test-models.py`, new `Tests/fixtures/ai/` (`identity.onnx`, `scale2.onnx`, `identity-fp16.onnx`, `upscale2-nearest.onnx`, `slow-loop.onnx`, README), new `Tests/Ai/OrtLoader_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - The script uses the `onnx` helper API to write the models. Each is a few hundred bytes, so they're committed outright, not through LFS.
    - Register an `Ai` gtest suite through the per-suite process scheme from M155 - Per-Suite Test Processes.
    - The tests find ORT in `build/assets/onnxruntime/lib`.
  - verify: `ctest -R Ai` passes, and re-running the script reproduces byte-identical files.
  - size: M

- [x] M203.P3.T2 — GPU runtime installer script
  - files: new `tools/ai/install-ort-gpu.sh`
  - approach:
    - Write `--cuda` and `--rocm` modes. Each downloads the matching official ONNX Runtime GPU release at the pinned version, checks its sha256, and unpacks it into `$XDG_DATA_HOME/<Natron data dir>/onnxruntime/`. Mirror darktable's `tools/ai/install-ort-gpu.sh` and `install-ort-amd-build.sh`.
    - The script prints the CUDA and cuDNN versions it needs.
  - verify: `--dry-run` prints the URL and target. A real `--cuda` run on the GPU host followed by Natron with Device = CUDA lists `CUDAExecutionProvider`, and the result goes in `## Decisions`. Without an NVIDIA host, record that it's unverified.
  - size: M

- [x] M203.P3.T3 — README section
  - files: `Engine/Nodes/README.md`, new `Engine/Ai/README.md`
  - approach:
    - `Engine/Ai/README.md` covers the library search order, EP selection, the session cache, tiled inference and the abort and OOM policy.
    - The Nodes README gains "AI nodes": `NodeSpec` CPU-only, the persistent error when AI is unavailable, and `pack`/`unpack` owning colour conversion.
  - verify: reviewed at the gate.
  - size: S

**Verification gate:**
- The full build and ctest pass, including `ctest -R Ai`.
- The AppImage contains the ORT CPU library.
- Under Xvfb, the Preferences "AI" page shows the detected providers with AI enabled, and "AI is disabled" behaviour with it off. Screenshots are shared with the user before sign-off.

## Decisions

- 2026-10-27 — **Phase 2-3 choices (bosley-native-nodes):** CPU runs share the cores by each session's thread count; GPU one run per device; the session cache key includes threads, optimisation and library; OpenVINO attaches as GPU / GPU.<index>; fixed-size models clamp and pad tiles and skip halving; `--rocm` builds ORT with MIGraphX from source (no official AMD build at 1.31.1); GPU provider options go through library-owned option objects, never header struct layouts. Release gate 550/550; PR #132.
