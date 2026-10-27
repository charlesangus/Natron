# AI inference

`Engine/Ai` runs neural networks for the AI nodes. It is built on ONNX Runtime (ORT), which it loads
at run time with `dlopen` and drives only through ORT's C API (`OrtGetApiBase`). The build uses
ORT's headers and never links against the library. As a result, the CPU build bundled with Natron
and a GPU build the user installs later are interchangeable, and Natron still runs when no ORT can
be found at all. The design follows darktable 5.6's `src/ai/backend_onnx.c`.

Inference sits beside the Vulkan backend, not inside it. Tensors pass through host memory, and an
AI node runs on the CPU as far as the scheduler is concerned (see "AI nodes" in
`Engine/Nodes/README.md`).

| File | Holds |
|---|---|
| `OrtLoader.*` | finding, loading and probing the library; the process-wide loader |
| `InferenceSession.*` | sessions, the session cache, execution provider selection, per-device concurrency, abort, error classes |
| `Tensor.h` | the float32 NCHW `Tensor`, and the float16 conversions |
| `TiledInference.*` | `runTiled()`: an image model run over a region in overlapping tiles |

## Loading the library

`OrtLoader::process()` is the loader the whole process shares. While "AI Enabled" is off in
Preferences it loads nothing and reports `OrtLoader::disabledReason()`, "AI is disabled in
Preferences". The first call made with AI enabled searches these candidates
(`OrtLoader::searchCandidates()`):

1. **"AI ORT Library Path" in Preferences, if set.** This can be the library file or a folder that
   holds it, directly or in `lib/`. It is *required*: if it fails, the search stops there and
   reports the failure rather than quietly using another library.
2. **`<user data dir>/onnxruntime`**, which is `~/.local/share/INRIA/Natron/onnxruntime` unless
   `XDG_DATA_HOME` is set. This is where `tools/ai/install-ort-gpu.sh` installs a CUDA or MIGraphX
   build.
3. **`<executable dir>/../lib/onnxruntime`**, the CPU build bundled in the AppImage. Its version is
   pinned in `tools/ci/local/fetch-assets.sh`.

A library loads only if it exports `OrtGetApiBase` and offers the OrtApi version the headers were
written for, or an older one no lower than `OrtLoader::kMinimumApiVersion`. A loader that fails is
unavailable, and `unavailableReason()` names every path it tried and why each one failed. A loader
never throws. A failed load is retried when the Preferences path changes. A successful load lasts
for the life of the process, so switching to another library takes a restart.

The loader lists the execution providers (EPs) the library offers. This is ORT's own list plus any
EP whose `OrtSessionOptionsAppendExecutionProvider_*` symbol the library exports but the list leaves
out. The Preferences "AI Detected Providers" line shows the result.

## Choosing the execution provider

`AiSessionOptions` carries the device (Auto, CPU, CUDA, MIGraphX, ROCm or OpenVINO), the GPU index,
the CPU thread count and a graph optimisation level. `fromPreferences()` fills in the first three,
and the environment variable `NATRON_AI_DEVICE` overrides the device. The optimisation level comes
from the model's manifest.

`InferenceSession::providerOrder()` turns the requested device into a list of EPs to try, and the
list always ends with the CPU:

- **Auto** tries CUDA, then MIGraphX, then ROCm, then the CPU. It skips any EP the library lacks.
- **A named device the library lacks** falls back to the CPU, with a one-time warning that names
  the missing EP.
- **An EP that is present but fails to open the model** moves on to the next EP. A typical case is
  a CUDA build installed on a machine without cuDNN or a matching driver. The failure is logged.
  Only a failure on the CPU becomes an error.
- **OpenVINO** is attached with `device_type` set to `GPU`, or to `GPU.<index>` for a nonzero GPU
  index.

EPs are attached without ORT's provider option structs (`OrtCUDAProviderOptions`,
`OrtROCMProviderOptions`, `OrtMIGraphXProviderOptions`), because a library older than Natron's
headers may lay them out differently. Only calls already in OrtApi version
`OrtLoader::kMinimumApiVersion` (18) are used:

- **CUDA:** `CreateCUDAProviderOptions`, then `UpdateCUDAProviderOptions` with `device_id`, then
  `SessionOptionsAppendExecutionProvider_CUDA_V2`.
- **ROCm:** `CreateROCMProviderOptions`, then `UpdateROCMProviderOptions` with `device_id`, then
  `SessionOptionsAppendExecutionProvider_ROCM`. The library allocates and fills the struct itself,
  so Natron never reads or writes its fields.
- **MIGraphX:** the key/value `SessionOptionsAppendExecutionProvider("MIGraphX", device_id)`. If the
  library's generic append doesn't know MIGraphX, Natron falls back to the exported
  `OrtSessionOptionsAppendExecutionProvider_MIGraphX(options, device_id)`. That function leaves every
  other option at the library's default.
- **OpenVINO:** the key/value `SessionOptionsAppendExecutionProvider("OpenVINO", device_type)`.

`session->device()` reports the EP the session actually runs on.

## Sessions and the cache

`InferenceSession::open(modelPath, options)` returns a shared session. The process-wide cache is
keyed by:

- the model's path and modification time
- the full options
- the loader

So rewriting a model on disk, or changing the device, threads or optimisation level, opens a new
session. The cache keeps the `kCacheCapacity` most recently used sessions. A session that is evicted
lives on for as long as a caller still holds it. If several threads open the same key at once,
exactly one session is created.

A session's `run()` may be called from many threads at once. Inputs and outputs are float32
`Tensor`s. If the model takes or returns float16, the session converts at its boundary, rounding to
nearest even, so callers never see float16. Output shapes come from each run, so models with dynamic
output sizes work. A model whose inputs or outputs aren't float32 or float16 is refused when it
opens.

### Concurrency

Every run first passes a per-device gate, so parallel renders queue up rather than exhausting the
device:

- **A GPU** allows one run at a time.
- **The CPU** shares its cores. Each run reserves as many cores as its session's thread count, or
  every core when threads is 0, since ORT then takes them all. A run waits while it would push the
  reservations past the core count, unless nothing else is running. With one thread count in use,
  that is `max(1, cores / threads)` runs at once.

Each run reserves its own session's count, so changing "AI Threads" in Preferences applies to
sessions opened after the change (the count is part of the cache key). Runs already in flight keep
their reservation until they finish.

## Tiled inference

`runTiled(session, roi, spec, pack, unpack, abort)` runs a one-input, one-output image model over a
region of any size. The `AiTileSpec` gives the tile edge, the overlap, the multiple the model's input
must be padded to, the output scale and the channel counts.

- **Tiles.** `tileStarts()` spreads tiles evenly across each axis. The last tile is flush with the
  edge, and neighbouring tiles share at least `overlap` pixels.
- **Padding.** Each tile is padded up to `multipleOf` by reflection (numpy's `reflect`, which does
  not repeat the edge pixel). The padding is cropped off the result.
- **Blending.** Tiles are blended at output resolution with raised-cosine weights. Across a shared
  span, the weights sum to one.
- **Fixed-size models.** If the model's input has a fixed height or width, tiles are clamped to that
  size, and smaller tiles are padded up to it.
- **pack and unpack.** These callbacks belong to the caller. `pack` fills each tile's tensor from
  the image, and `unpack` receives the whole blended result once, after every tile has run.
  Colour conversion into and out of the model's space happens there, so each node owns it.
- **Row order.** Tensor row 0 is the top row of the image, `y2 - 1` in Natron's bottom-up
  coordinates.

## Abort and running out of memory

- **Abort.** `abort` is checked before each run, and before each tile in `runTiled()`. While a run
  is in progress, a watcher polls it every 20 ms and calls ORT's `RunOptionsSetTerminate`, which
  stops even a long run within a fraction of a second. An aborted run throws `AiAborted`.
  `runTiled()` never calls `unpack` after an abort or failure, so a partial result can never reach
  a node's output or the cache.
- **Out of memory.** ORT failure messages that describe the device or host running out of memory
  (`isOutOfMemoryMessage()`) throw `AiOutOfMemory`. `runTiled()` recovers in up to three steps:
  1. It restarts the whole pass once with half the tile edge. Fixed-size models skip this step.
  2. If the session is on a GPU, it reopens the model on the CPU and runs again.
  3. If both fail, the `AiOutOfMemory` propagates.

  Each step is logged.
- **Other failures** throw `AiError`, whose message names the model.

## Testing

`Tests/Ai` runs these classes against the real ORT that `fetch-assets.sh` unpacks into the build
tree (`NATRON_TESTS_ONNXRUNTIME_DIR`). The tests use the tiny models in `Tests/fixtures/ai`, which
`tools/ai/make-test-models.py` generates.

`InferenceSession::setFailureInjectorForTesting()` makes chosen runs fail with a given ORT message.
With it, the tests drive the out-of-memory and error paths without a device that actually fails.
The timing-sensitive abort test is in the serial suite `AiAbortTiming`.
