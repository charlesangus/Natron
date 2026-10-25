

### 2026-10-24T01:22:11+00:00 — change-request
- refs: M119
- Also for the trackball/global mode control: 
  
  - global mode should make the cursor a crosshair (pitch some designs)
  - the hud can follow the cursor, offset below and to the right
  - would like to try out some various other UI designs for global mode, like the trackball appearing in C mode when you click, and similar images for the other modes that give feedback on what you're doing
  - could also try making the cursor change to that UI (trackball etc) 
  
  Pitch some designs.

### 2026-10-24T11:00:00Z — info
- refs: none
- Pre-existing race on main, found while diagnosing a `NativeWriteReadBackTest` flake in M138 - GUI Papercuts (reproduced about 58 failures in 480 suite runs with 8 parallel copies on 4 cores). `BlockingBackgroundRender::blockingRender` (`Engine/BlockingBackgroundRender.cpp:53-72`) returns when `notifyRenderFinished()` fires inside the scheduler thread's `threadLoopOnce` (`Engine/OutputSchedulerThread.cpp:2823`, `Engine/OutputEffectInstance.cpp:356`), but the thread only marks itself idle after that returns (`Engine/GenericSchedulerThread.cpp:523-526`). In the gap, `RenderEngine::isDoingSequentialRender()` is still true, so `NativeWrite::readBackActive()` (`Engine/Nodes/IO/NativeWrite.cpp:1396`) reports false and `EffectInstance.cpp:521` aborts renders on the Write. M138 worked around it in the test (wait for `isDoingSequentialRender()` to go false). An engine fix — `blockingRender` also waiting for the scheduler to go idle — needs care: if `notifyRenderFinished` starts the next queued sequence, the thread stays active and the wait could hang. Other callers that inspect a Write right after `startWritersRendering` may hit the same race.

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

### 2026-10-23T23:59:00Z — change-request
- refs: none
- New milestone requested (user, 2026-10-23): **"Rich Viewer Overlays"** — let nodes draw rich, arbitrary viewer overlays: overlaid images (per-pixel data such as masks, drawn over the viewer image, never into renders), handles, points, lines, text and free-form drawing from native nodes, not just the stock Transform/CornerPin/Position/Rectangle interacts. Found while porting darktable's darkroom-only aids in M80 - Native RAW Support, which were deferred to it: the highlights mask views (clipped, candidates, combine, strength), the dual-demosaic mask, the capture-sharpen sharpened-areas and radius views, and hot-pixel Mark Fixed (rawprepare/demosaic/highlights/hotpixels at darktable `release-5.6.2`). Scouted facts: `EffectInstance::drawOverlay` (`Engine/EffectInstance.h:2552`) is an empty virtual no native node overrides; native nodes only call `Node::addRectangleInteract` (`Engine/NodeOverlay.cpp:317`); host primitives live in `Gui/HostOverlay.cpp` (points `:1172`, handles `:1246`, line loops `:1312`, text `:1036`); OFX overlays get GL via `Engine/OfxOverlayInteract.cpp`; no overlay path receives image data (viewer pixels go only to the viewer texture, `Engine/ViewerInstance.cpp:2037-2112`); the viewer-render flag exists as `isRenderUserInteraction` in `ParallelRenderArgs` (`Engine/ParallelRenderArgs.h:308`) but nodes can't query it. Suggested deps: M80 (so the deferred raw aids can be added as its first consumers). Acceptance sketch: a native node draws handles, points and text with no OFX; a node publishes a per-pixel overlay image that the viewer composites over its image without affecting any render or cache; the deferred darktable aids are added on top.

### 2026-10-23T12:00:00Z — change-request
- refs: none
- Investigation requested (user, 2026-10-23, from M110 - Remaining Native GPU Ports): **how non-float image planes should be handled.** Today the channel nodes disagree. Shuffle and ShuffleCopy read a non-float source plane as 0 (`Engine/Nodes/Channel/Shuffle.cpp:870`), while AddLayers and RemoveLayers convert byte and short planes through `ChannelCopy`'s depth table (`ChannelCopy.cpp:101-114`; `NativeEffectBase.cpp:512-517`). The staged pipeline is float-only (`StagedImageEffect.cpp:63-66`), and GPU regions are float. The user suggests non-float planes may be a different *kind*, as deep is beside image, rather than a per-node depth policy. Suggested: a stub milestone (epic E3 - Native Nodes And I/O or E14 - Architecture) that surveys where non-float planes arise (readers, channel nodes, OFX plugins, the cache) and how each node treats them, then proposes a model. M110 keeps current behaviour per node meanwhile.

### 2026-10-24T05:30:00Z — info
- refs: none
- Found by M138 - GUI Papercuts while fixing the file-dialog ▶ crash: a GL widget that moves to another top-level window gets a new GL context, and Qt 6 destroys the old one. M138 made `ViewerGL` free its GL objects and its `TextRenderer` atlases on `aboutToBeDestroyed` (`Gui/ViewerGLPrivate.cpp`, `releaseViewerGLResources`). Five other GL widgets hold their own `TextRenderer` by value with no reset on context loss: `Gui/Histogram.cpp:201`, `Gui/HostOverlay.cpp:1015`, `Gui/DopeSheetView.cpp:291`, `Gui/CurveWidgetPrivate.h:158`, `Gui/TimeLineGui.cpp:161`. If one of them is re-parented or floated out of a pane, its cached atlas IDs are stale: text draws blank or samples another texture (`Gui/TextRenderer.cpp:323`), glyph uploads write into an unrelated texture (`:196-198`), and its destructor deletes IDs in whatever context is current (`:118`), which can delete live textures. The same likely applies to their other GL objects. Suggested: one small milestone (or an entry in an existing GUI-robustness milestone) giving every GL widget the same context-loss release, plus a GUI script that floats each pane and back.

### 2026-10-24T08:30:00Z — info
- refs: none
- From M113 - Native Warp Nodes, the first plain-ID native nodes (`Mirror`, `STMap`, `IDistort`, `CornerPin`, per DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md). `Engine/CMakeLists.txt` now validates an OfxRetired marker's replacement with a plain-or-dotted regex (the file name stays a dotted OFX ID). Points for M107 - Native Node Identity Cleanup's mass rename: (1) the node-menu tree (`Gui/GuiApplicationManagerPrivate.cpp:146-160`) keeps groups and plug-ins in one list per level matched by `getID()`, so a plain ID equal to a sibling subgroup name under the same parent would be taken for that group; impossible with dotted IDs. (2) `Engine/AppManager.cpp:2251-2259` `getPluginBinaryFromOldID` maps `Viewer`, `Dot`, `DiskCache`, `Backdrop`; it only runs after an exact lookup fails, but the renames should check it. (3) `Engine/Nodes/README.md:971-990` ("IDs and versions") still says native nodes keep the OFX ID at a higher major.

### 2026-10-24T13:00:00Z — info
- refs: none
- Found by M113 - Native Warp Nodes (dfb04cdbe): `fma(a, b, -p)` as an exact product-error term is not single-rounded on lavapipe (Vulkan doesn't require it), so double-float tricks built on it silently lose their low half. M113 fixed `resamplecore.slang` with a Dekker split under `precise` (as `blur_lines.slang:132` already does). `Engine/Gpu/Kernels/curvesnapshot.slang:71,166,196` still uses the fused-fma form and will lose precision the same way on lavapipe; outside M113's scope.

### 2026-10-24T14:55:00Z — change-request
- refs: none
- From M110 - Remaining Native GPU Ports (gpu PM), on the user's ruling "Node metadata should absolutely not be the same at every frame, there's no reason that should be the case" (M110 `## Decisions`, 2026-10-24). Add a new milestone in E14 - Architecture, sized XL by the scoping consultant, with M144 - Render-Thread Metadata Refresh absorbed by it or blocked on it (a planning call). M110.P3.T4 already fixes the Switch's per-frame layers through the components path, ahead of this. The consultant's stub, with premult removed from the field list per the user (see the next entry):

  # M<i> - Per-Frame Node Metadata

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Make a node's metadata (colour layout and components, bit depth, PAR, fps, format, field order) a function of time and view rather than one value per refresh. Each node computes it on request at (time, view) and caches it keyed by node hash, time and view, as M77 does for key/value metadata. Routing nodes (Switch, integer Dissolve, TimeOffset, FrameHold) then report their active input's metadata at each frame. Native nodes derive it at a time; OFX nodes keep one time-invariant answer per hash, since getClipPreferences has no time. Consumers outside the render (Viewer, Write, node graph, Python, project format) ask at an explicit time, and Write fixes one header per file.

  Blocked on: M144's design choice; this milestone should absorb or decide it, since hash-keyed, on-request metadata removes the main-thread-only refresh. Also a decision on what a Write does when its input's metadata varies across the range.

  Acceptance sketch:
  - A Switch keyed rgb+spec at f1 and rgba at f2 reports RGB metadata and layers at f1 and RGBA at f2, and so does every node downstream, including AddLayers' getPreferredMetadata.
  - The Viewer's format, PAR and channel menu follow the current frame; a Write over a varying range applies the agreed policy.
  - An OFX node's clip preferences are computed once per hash, and OFX tests are unchanged.
  - Metadata is queried off the main thread with no deadlock or stale value (M144's acceptance).
  - Scheduler-equivalence, cache-equivalence and TimeVaryingLayers suites are green.

### 2026-10-24T14:56:00Z — info
- refs: none
- The user, during M110 - Remaining Native GPU Ports (2026-10-24 13:15Z): "nodes are not supposed to be holding premult information either, that was supposed to be ripped out". The board has M43 - Drop Premult Concept as `cancelled`, and premult state is still in main (e.g. `ImagePremultiplicationEnum`, `getOutputPremultiplication`, OFX `kOfxImageEffectPropPreMultiplication` in Engine). Planning needs to reconcile the cancellation with the user's statement: revive M43 or file a new milestone to remove premult from node metadata. The per-frame metadata stub above already leaves premult out.

### 2026-10-24T16:00:00Z — change-request
- refs: none
- From M91 - NVIDIA GPU Validation (pm `bosley-faux`): please file two follow-up milestones (E5), both plan-scoped so left for /cat-plan.
  1. **NVIDIA Viewer Hand-off Validation**, Deps M107 - GPU Viewer Hand-off and M91. Carries M91's struck Phase 91.3 verbatim — P3.T0 (surface `GpuTransfer::stagedReasons()` in `RegionReport`/profile JSON via `Engine/GpuRegionTask.cpp`), P3.T1 (`GUI_TEST_HOST_DISPLAY=:0` mode in `Tests/gui/run-gui-test.sh`), P3.T2 (M107 suites + `m107_viewer_handoff.py` UAT on every rung on the RTX 3070 Ti host bosley-faux). Full briefs are in `PLAN/MILESTONES/M91-nvidia-gpu-validation.md` Phase 91.3. Needs the NVIDIA host.
  2. **Pipelined Strip Member Timing**: in pipelined strips, per-member kernel times are inflated (Grade 0.21 ms pipelined vs 0.045 ms serial on the RTX 3070 Ti), which skews `GpuCostModel` calibration samples. Found in M91.P1.T6 triage; M91.P2.T5 only relaxed the test to assert nesting. Deps blank (touches `Engine/GpuRegionTask.cpp`, an M107 file).

### 2026-10-24T19:05 — change-request
- refs: none
- New E2 - Build, CI And Release milestone, user-requested from the gpu PM's build/test efficiency review: (b) give `.github/workflows/remote-build.yml` and `tools/ci/remote/build.sh` a single-scheduler `--tests` option for intermediate fix builds, keeping both scheduler legs (legacy and taskgraph, `remote-build.yml:102-122`) for final milestone gates. Today every `--tests` run does the whole ctest suite and the smoke test twice. (a) Have `tools/ci/local/build.sh` derive `JOBS` from the host (`nproc`, and MemAvailable per compile job) instead of the hardcoded 3 (`build.sh:275-282`, sized for a 4-core 15 GB host), and **always leave at least one core free** (user). Keep the `NATRON_BUILD_JOBS` override. Deps: `-`.
- Addendum (2026-10-24, gpu PM): **`tools/ci/local/build.sh` drops its arguments on main** since M154 - Fresh Buffer Page Zeroing (#79). The parse loop (~:99) shifts `$@` empty before the host-side re-exec (~:263, `... build.sh "$@"`), so every host invocation runs as `debug` without `--tests`. The fix is to save `ORIG_ARGS=("$@")` before the loop and re-exec with `"${ORIG_ARGS[@]}"` (the diff is in the gpu PM's scratchpad; it is applied uncommitted in `.worktrees/m110` only for P4.T3's RADV build). It belongs in this new milestone, or in an earlier hotfix.

### 2026-10-24T19:30 — info
- refs: none
- `Tests/gui/m55_node_graph.py` section `splice-vertical` fails 6 checks on main (seen on `M80-final.AppImage`, main plus M80, and on M134's branch): the hover highlight isn't drawn, the splice connections aren't made, and redo doesn't re-apply them. Found while gating M134 - Output Picker Pixel Aspect, which doesn't touch the node graph. Needs an owner: likely a regression from a node-graph change, or a script assumption that no longer holds.

### 2026-10-24T19:40 — info
- refs: none
- `NativeRawColorCalibrationGpuTest.EveryAdaptationVersionClipAndGamutMatchesTheCpu` (M80 - Native RAW Support, on main) fails on real RADV hardware (RX 7900 XTX, `NATRON_TEST_REQUIRE_GPU=1`): in several adaptation/clip/gamut cases one float of 256 (float 26) differs from the CPU by ~1.3–1.6e-6, against an expected 0 mismatches (`Tests/Native/RawGpuTestSupport.h:202`). It passes on lavapipe. It needs a ulp- or relative-tolerance bound like the other GPU parity suites. It could go in M171 - GPU Correctness Fixes, or to whoever owns RAW follow-ups. Found during M110 - Remaining Native GPU Ports' RADV run; log at the gpu PM's scratchpad `m110-radv-ctest.log`.
