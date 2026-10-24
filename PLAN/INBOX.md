## Pending

### 2026-10-22T08:00:00Z — change-request
- refs: none
- New milestone requested (user, while scoping M86 - OFX 1.5 GPU Suites to the GL suite only): **"OFX CUDA And OpenCL Suites"**, epic E5 - GPU Compute, Deps M86. Also add it to M116 - GPU Region Overlay's Deps. Suggested stub:

  > Host the OpenFX 1.5 CUDA and OpenCL render suites, so third-party compute plugins join a resident region with no host round trip. Port OFX 1.5's `ofxGPURender.h` properties into the `libs/OpenFX` fork: CUDA stream support, OpenCL buffers and images, the optional `OfxOpenCLProgramSuiteV1`, and `kOfxStatGPUOutOfMemory`. Extend HostSupport's render in-args with the CUDA stream. Advertise each suite only when its backend is live. Import exported Vulkan memory and semaphores, through a `dlopen`ed `libcuda.so.1` (driver API: `cuImportExternalMemory`/`cuImportExternalSemaphore`/`cuWait|SignalExternalSemaphoresAsync`) or through OpenCL `cl_khr_external_memory_opaque_fd` + `cl_khr_external_semaphore_opaque_fd`, with the device matched by `cl_khr_device_uuid`. Hand the plugin `kOfxImagePropData` as a device pointer or `cl_mem`, pass the host stream or queue, and never wait for the plugin's work. Reuse M86's region boundary. Carries M84's hand-offs: an exportable `GpuBufferPool` variant, imported once per buffer and recycled; out-of-pool plugin memory, checked against the 1 GiB `kGpuStripOutsidePoolHeadroom` (moved here from M86); `kOfxStatGPUOutOfMemory` mapped onto the evict → serial strips → halve → CPU ladder.
  >
  > Blocked on: M86 - OFX 1.5 GPU Suites (the region boundary) and its P1.T1 spike (which compute runtimes and extensions exist on the dev host and CI); user calls on CUDA-without-NVIDIA (consultant recommends: build it, test without a driver, validate on M91's host) and on a staged OpenCL fallback (consultant recommends: zero-copy where available plus a staged RAM path outside regions, tested on CI with EPEL 9's pocl 3.1); end-to-end CUDA validation needs M91 - NVIDIA GPU Validation's host.
  >
  > Acceptance sketch: an OFX 1.5 OpenCL sample plugin (`libs/OpenFX/Examples/OpenCL`) renders inside a resident region with no host round trip wherever a zero-copy OpenCL runtime exists, and elsewhere follows the agreed fallback; the CUDA path builds in CI, is unit-tested without a driver (clean unavailability, suite not advertised), and is validated end to end on M91's NVIDIA host; a plugin returning `kOfxStatGPUOutOfMemory` takes the out-of-memory ladder, and out-of-pool memory with real plugins is measured against the headroom.

### 2026-10-22T12:00:00Z — info
- refs: none
- Pre-existing bug on main, found by M154 - Fresh Buffer Page Zeroing's first `-DNATRON_GPU=OFF` CI build (run 39770415588) and outside its scope: `StagedImageEffectGpuTest` fails in a GPU-off build because `StagedImageEffect::canRenderOnGpu` returns true for 1–4 components and for the mask/mix/channel-subset query, where the test (`Tests/Native/StagedImageEffectGpu_Test.cpp:511, 528`) expects false without `NATRON_HAVE_GPU`. Either the effect should refuse GPU without `NATRON_HAVE_GPU`, or the test's expectation is wrong. Both came from M137. Repro: `tools/ci/remote/build.sh fast --tests --no-appimage --cmake-arg -DNATRON_GPU=OFF` (available once M154 merges).

### 2026-10-23T00:15:41+00:00 — change-request
- refs: M29
- This is defunct now, cancel it. I was able to self serve leaving the fork network and this has been done.

### 2026-10-22T23:30:00Z — change-request
- refs: none
- New milestone requested (user, 2026-10-22): **"Incremental Bezier Edits"**, a performance fix, deps `-`. Found while building M16 - Project Format Redesign's heavy benchmark project: building 30 beziers × 24 points × 200 keys through Python took 2664 s. Every single point edit on a `Bezier` does work proportional to the whole shape's animation, so building or editing a heavily keyed shape is quadratic. This is inherited upstream code in `Engine/Bezier.cpp`.

  > Two costs per edit:
  > 1. `copyInternalPointsToGuiPoints()` (`Bezier.cpp:1007`) clones the full curve set (x, y, both tangents) of **every** control point and feather point into the GUI copies after each edit. That is O(points × keys) for a change to one point at one time. It is called from about a dozen edit paths (`:1102`, `:1181`, `:1387`, `:1723`, `:2032`, …), including `setPointAtIndex`. `addControlPoint` has the same cost.
  > 2. `refreshPolygonOrientation()` (`:3499`) recomputes the clockwise/counter-clockwise orientation at **every** keyframe time, not just the one edited. Each recompute walks all points and evaluates their curves, so about O(keys × points × log keys) per edit.
  >
  > Building a P-point, K-key shape key by key is therefore about O(P² × K²). The same cost hits interactive work: dragging a point on a heavily keyed roto shape, and a tracker writing keys onto a linked bezier.
  >
  > Suggested approach: copy to the GUI incrementally (only the edited point's curves, or only the edited time), recompute orientation only at the edited time, and cache orientation per time with invalidation per key. Check what the GUI copy is actually for: it lets the GUI edit while a render reads the internal curves. Keep that guarantee, and drop the copy where it isn't needed.
  >
  > Acceptance sketch:
  > - A bench builds a 24-point × 200-key feathered bezier through the API in time roughly linear in keys.
  > - The per-edit cost of `setPointAtIndex` stays flat as keys grow.
  > - Roto render output and the saved project are bit-identical to before on the existing roto tests.
  > - Interactive point drags on a heavily keyed shape stay responsive. Measure with the GUI bench once M117 - GUI Perf Harness And Baseline lands.

### 2026-10-23T22:19:49Z — change-request
- refs: none
- New milestone requested (user, 2026-10-23): **"Viewer Node Knobs"**, epic E3 - Native Nodes And I/O, Deps M16 - Project Format Redesign and M107 - GPU Viewer Hand-off. It follows from the M16 - Project Format Redesign decision to drop `gui.yaml`, which leaves viewer state unsaved until this lands. Suggested stub:

  > Per-viewer state becomes knobs on the Viewer node, so it is saved in `project.yaml` like any other node data and is scriptable, linkable and undoable. Today it is `ViewerTab` widget state saved through `ViewerDataSerialization` (`Serialization/ProjectGuiSerialization.h`), and `Engine/ViewerInstance` has no knobs. In scope: gain, gamma, display/view/look, channels, layer and alpha layer, inputs A/B, wipe compositing operator, fps and fps lock, playback frame bounds, user RoI and its enable flag, clip-to-project, full-frame processing, auto-contrast, render scale and proxy. The viewer toolbar widgets become views of those knobs. RB-3 (`origin/master`'s `Engine/ViewerNode*.cpp`, especially `ViewerNodeKnobs.cpp`) is a reference to port from selectively, not wholesale.
  >
  > Not in scope (transient, never saved): zoom/pan and `zoomOrPanSinceLastFit`, toolbar visibility (app layout), histograms.
  >
  > Blocked on: M16 - Project Format Redesign (no `gui.yaml`, node schema settled) and M107 - GPU Viewer Hand-off (both touch the viewer render path and `ViewerTab`).
  >
  > Acceptance sketch: set gain, gamma, display/view and wipe on two viewers, then save, close and reopen; both viewers restore their own values from `project.yaml`, and zoom does not. Changing gain from Python or by dragging the toolbar slider moves the same knob, and undo reverts it. Changing a viewer knob doesn't invalidate the upstream render cache, only the viewer's own processing.

### 2026-10-23T23:40:00Z — info
- refs: none
- Pre-existing race, found by M80 - Native RAW Support's debug build (run 40033444348, ctest legacy leg; the serial rerun passed): `SchedulerEquivalence.NodeFeedingSourceAndMaskIsTwoEdges` segfaulted in `QEventDispatcherGlib::wakeUp` ← `QObject::deleteLater` ← a destructor run while a `shared_ptr` was released, on a pooled thread. Likely cause: `AbortableRenderInfo::~AbortableRenderInfo()` (`Engine/AbortableRenderInfo.cpp:107-113`) calls `deleteLater()` on a `QTimer` owned by the creating pool thread, which may be exiting (the test changes `QThreadPool::globalInstance()->setMaxThreadCount` across 16/4/1). The queued `startTimerInOriginalThread` never runs on pool threads (no event loop), so the abort timeout is already inert there. A fix (timer owned by the main thread) changes behaviour; worth its own milestone or a M31 entry.

### 2026-10-24T01:22:11+00:00 — change-request
- refs: M119
- Also for the trackball/global mode control: 
  
  - global mode should make the cursor a crosshair (pitch some designs)
  - the hud can follow the cursor, offset below and to the right
  - would like to try out some various other UI designs for global mode, like the trackball appearing in C mode when you click, and similar images for the other modes that give feedback on what you're doing
  - could also try making the cursor change to that UI (trackball etc) 
  
  Pitch some designs.

### 2026-10-23T23:59:00Z — change-request
- refs: none
- New milestone requested (user, 2026-10-23): **"Rich Viewer Overlays"** — let nodes draw rich, arbitrary viewer overlays: overlaid images (per-pixel data such as masks, drawn over the viewer image, never into renders), handles, points, lines, text and free-form drawing from native nodes, not just the stock Transform/CornerPin/Position/Rectangle interacts. Found while porting darktable's darkroom-only aids in M80 - Native RAW Support, which were deferred to it: the highlights mask views (clipped, candidates, combine, strength), the dual-demosaic mask, the capture-sharpen sharpened-areas and radius views, and hot-pixel Mark Fixed (rawprepare/demosaic/highlights/hotpixels at darktable `release-5.6.2`). Scouted facts: `EffectInstance::drawOverlay` (`Engine/EffectInstance.h:2552`) is an empty virtual no native node overrides; native nodes only call `Node::addRectangleInteract` (`Engine/NodeOverlay.cpp:317`); host primitives live in `Gui/HostOverlay.cpp` (points `:1172`, handles `:1246`, line loops `:1312`, text `:1036`); OFX overlays get GL via `Engine/OfxOverlayInteract.cpp`; no overlay path receives image data (viewer pixels go only to the viewer texture, `Engine/ViewerInstance.cpp:2037-2112`); the viewer-render flag exists as `isRenderUserInteraction` in `ParallelRenderArgs` (`Engine/ParallelRenderArgs.h:308`) but nodes can't query it. Suggested deps: M80 (so the deferred raw aids can be added as its first consumers). Acceptance sketch: a native node draws handles, points and text with no OFX; a node publishes a per-pixel overlay image that the viewer composites over its image without affecting any render or cache; the deferred darktable aids are added on top.
