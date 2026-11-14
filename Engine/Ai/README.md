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
| `ModelRegistry.*` | the model manifest, models by task, and where a model is installed |
| `ModelPackage.*` | `.dtmodel` unpacking, `config.json` parsing into `ModelConfig`, sha256 |
| `ModelDownloader.*` | downloading, checking and installing a model's package |
| `ModelKnob.*` | the node-side `model` menu, Download Model button and model resolution |
| `data/ai_models.json` | the pinned manifest, compiled in as `:/Ai/ai_models.json` |

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
and the environment variable `NATRON_AI_DEVICE` overrides the device. The optimisation level is the
caller's, as in darktable: the node passes it to `fromPreferences()`, and neither the manifest nor
`config.json` carries one.

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

`runTiled(session, roi, spec, pack, unpack, abort, finish)` runs a one-input, one-output image model over a
region of any size. The `AiTileSpec` gives the tile edge, the overlap, the multiple the model's input
must be padded to, the output scale and the channel counts.

- **Tiles.** `tileStarts()` spreads tiles evenly across each axis. The last tile is flush with the
  edge, and neighbouring tiles share at least `overlap` pixels.
- **Padding.** Each tile is padded up to `multipleOf` by reflection (numpy's `reflect`, which does
  not repeat the edge pixel). The padding is cropped off the result.
- **Blending.** Tiles are blended at output resolution with raised-cosine weights, or linear ones
  when `linearBlend` is set. Across a shared span, the weights sum to one.
- **Fixed-size models.** If the model's input has a fixed height or width, tiles are clamped to that
  size, and smaller tiles are padded up to it.
- **pack and unpack.** These callbacks belong to the caller. `pack` fills each tile's tensor from
  the image, and `unpack` receives the whole blended result once, after every tile has run.
  Colour conversion into and out of the model's space happens there, so each node owns it.
- **finish.** An optional callback that gets each tile's input tensor and its output before the
  output is blended, and may change the output. It is for a correction that needs the whole tile,
  such as RawNeuralDenoise matching each tile's output level to its input's, as darktable does.
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

## Models

Models are downloaded on demand from the `.dtmodel` release assets of
[darktable-ai](https://github.com/darktable-org/darktable-ai), never bundled. `data/ai_models.json`
pins one release: its tag is under `source`, and each model has its task, the package (asset) it
comes from, the package files it runs, the asset's URL, sha256 and size, its weight and
training-data licences, darktable-ai's OSAID classification, and a link to its model card.
`tools/ai/update-manifest.py` regenerates the file from a release tag; run with no arguments, it
reproduces the committed file. It refuses a model whose weight licence is not GPL-3-compatible.

darktable-ai ships no optical-flow model, so the `optical-flow` task's model, SEA-RAFT M
(`optical-flow-searaft-m`, BSD-3-Clause), is a package of Natron's own, listed in the script's
`NATRON_MODELS` and hosted on the fork's releases. `tools/ai/export-flow-model.py` builds it from the
published SEA-RAFT checkpoint and a SEA-RAFT checkout at the pinned commit: it exports the network
to ONNX with the image size and the refinement iteration count as run-time inputs, packs it with
fixed timestamps so a second run with the same torch and onnx gives the same sha256, and with
`--verify` checks it against SEA-RAFT's PyTorch forward and on a shifted 1024x576 pair under ONNX
Runtime on the CPU.

A package can hold several models: RealPLKSR and BSRGAN each ship their 2x and 4x models in one,
and RawNIND its Bayer and linear models. The manifest lists each model on its own, so a node picks
by task, and the models of a package share its download and its install folder,
`<user data dir>/models/<package>/`. `ModelRegistry::resolve()` reports a model installed when that
folder holds `config.json` and every file of the model.

`ModelDownloader` streams a package into `models/.tmp-<package>/`, checks its size and sha256,
unpacks it, parses `config.json` for each model of the package, and renames the result into place.
A failure at any step deletes the temporary folder and leaves what was installed untouched. A second
request for a package being downloaded joins the first.

Packages come over HTTPS, which needs Qt's OpenSSL TLS backend (`plugins/tls/libqopensslbackend.so`,
linking OpenSSL 3). The bundle stages it, and `tools/release/check-startup.sh` checks that the
packaged Natron loads it and OpenSSL from inside the bundle. When no backend works,
`ModelDownloader::start()` refuses an HTTPS package with `tlsUnavailableReason()`, which names the
backends found and the folders searched. A failed download says whether the certificate was refused,
the TLS handshake failed or the server answered with an HTTP error.

`config.json` is darktable-ai's: `ModelConfig` takes the task, tiling, input sizes (the tile size),
input kind and colour space from it, looking up a model's attributes by its file stem
(`model_x4`) over the package-wide ones. Tensor names and shapes are not in it; the session reads
them from the model.

A node picks its model with `ModelKnob::create()`, which takes one task or a list of them. The
`model` menu lists the models of each task in the order given, so a node that runs both
`denoise-linear` and `denoise` models shows the linear ones first. The default is the Preferences
default of the first task that has one, else the registry's default for the first task that lists
a model. The menu refreshes when a model of any of its tasks is installed or removed.
`ModelKnob::resolve()` returns the selected model's files and `ModelConfig`, or posts why it
can't and returns nothing.

## Object segmentation

`Segmenter` (`Segmentation.h`) runs darktable-ai's two-graph `mask-object` models, SegNext and SAM
2.1. The encoder sees the image once per (image hash, frame, model, inference resolution) key, and
the segmenter keeps the last `kCacheCapacity` encodings. The decoder turns click points (and, for SAM, boxes) into a
probability mask, refined over two passes as darktable's object tool does. `MaskTrace` turns that
mask into closed Bezier outlines and holes: marching squares, Douglas-Peucker simplification, then
a Schneider curve fit. Without smoothing, it keeps straight segments between corners.

`Segmentation.cpp` is ported from darktable's `src/common/ai/segmentation.c` (commit
`fee19b421fdb`), which is GPL-3.0-or-later, so that file is GPL-3.0-or-later too. The Natron tree
is GPL-2.0-or-later, which allows it, but a build that links the file is distributed under GPLv3.
`MaskTrace` is written fresh: it does not port darktable's vectorizer or potrace.

The SAM 2.1 models are trained on SA-1B, whose licence is research-only. The model licence notice
says so before a download, as it does for every model with such training data.

The Roto node's Object Select tool (`Engine/RotoObjectSelect.h`, shortcut O) is the only user of
both for now. In the viewer:

- A click adds a point on the object, and a Shift-click adds a point to leave out.
- A drag draws a box (SAM models only).
- The outline preview follows each prompt and shows the Beziers a commit makes, control points
  included.
- Object Density, a slider in the viewer toolbar, sets how many control points they get, and
  re-traces the preview as it moves. The value stays for the next shape. Corners where the outline
  turns sharply get broken tangents, so few points follow a sharp silhouette.
- Backspace removes the last prompt, and Esc clears them all.
- Return commits the outlines as Beziers keyed on the current frame, as one undo step. Holes
  become Stencil shapes.
- Undoing a commit removes the shapes, moves to the commit's frame and brings the selection back
  (its prompts and the decoder's mask, kept at the inference resolution and upsampled again, so
  the outline and a new commit match the undone one) to commit again. Adjusting it encodes the
  Source again if its encoding is gone. Redo commits the undone shapes again.
- The prompts belong to one frame: changing frame drops them.
- When the Source cannot be encoded, the error is shown, the outline is cleared and Return commits
  nothing until a prompt changes.
- Object Inference Resolution (Full, 50%, 25%, 1K, 2K or 4K; K caps the long edge at 1024, 2048
  or 4096 pixels, and a smaller image is never enlarged) sets the size of the image the model
  segments. The viewer, the prompts, the outline and the committed shapes stay at full resolution.
  Changing it with prompts placed keeps them where they are and segments again, encoding the
  smaller image unless its encoding is cached. The default is 2K.

Until the tool is picked, Roto pays nothing for it: no model file is looked at, the model menu lists
the models by name only and does not follow downloads, the segmenter and its encoding cache do not
exist, and its viewer knobs are left out of the render hash. Picking the tool marks the models that
are not installed and follows downloads from then on; a saved project keeps its model either way.
The segmenter is made on the first prompt. Putting the tool down stops any encoding and frees the
prompts, the segmenter, its encodings and the masks, so picking it again encodes the Source again.
The ONNX Runtime sessions themselves may stay in the process-wide session cache
(`InferenceSession`). Closing the project or deleting the node aborts an encoding in progress,
including the render of the Source, and waits for its thread to let go of the node graph.
It renders the Source on a worker thread at full resolution, then area-averages it, still scene
linear, to square pixels at the inference resolution (`SegmentationScaling`). It converts that to
the project's default display and view as 8-bit sRGB, not the viewer's, and encodes it. Prompts and
decoding run on the main thread: prompts are kept in canonical coordinates and mapped to the
inference image for each decode, and the probability mask the decoder returns is upsampled
bilinearly to full resolution before it is thresholded and traced. Object Density's tolerance and
Object Min Area are therefore in full-resolution pixels whatever the inference resolution.

Where the time goes on a large plate: the encoders take a fixed input (a 1024-pixel long edge for
SegNext and SAM 2.1), so their cost does not grow with the image, and neither does the decoder's.
What grows with the pixel count is everything around them. Once per encoding: reading the rendered
Source, the display transform and the copy kept as the guide of the mask upsampling. On every
click: the joint-bilateral upsampling of each decoder pass's mask to the encoded image, the
interior-point distance transform and box between passes, the IoU check and the seed-area flood
fill, which all run at the encoded size. A smaller inference resolution moves all of these to the
small image and leaves one bilinear upsampling and the trace at full resolution. Rendering the
Source itself stays at full resolution. Since the encoders shrink their input to 1024 anyway, 2K
costs the model nothing; the area average even gives the encoder a less aliased image than its own
bilinear resize of an 8K plate would. Below 2K the mask's edge follows a coarser image, so 1K and
25% trade edge accuracy for speed.

## Testing

`Tests/Ai` runs these classes against the real ORT that `fetch-assets.sh` unpacks into the build
tree (`NATRON_TESTS_ONNXRUNTIME_DIR`). The tests use the tiny models in `Tests/fixtures/ai`, which
`tools/ai/make-test-models.py` generates. The downloader tests install `identity.dtmodel`, which
`tools/ai/make-test-dtmodel.py` packs, through a `file://` URL in a manifest they write, so they never
touch the network. The HTTP error test serves a 404 from a local `QTcpServer`, and the TLS test
asserts the tree's Qt has a working OpenSSL backend.

`InferenceSession::setFailureInjectorForTesting()` makes chosen runs fail with a given ORT message.
With it, the tests drive the out-of-memory and error paths without a device that actually fails.
The timing-sensitive abort test is in the serial suite `AiAbortTiming`.
