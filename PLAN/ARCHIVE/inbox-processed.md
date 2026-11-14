

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

### 2026-10-24T20:30:00-04:00 — info
- refs: none

Found in M167 - Per-Frame Node Metadata: `Node::refreshDirtyHashes` can hand back a hash computed over a still-dirty input. After 886c0baf1 such a node stays dirty, but only the metadata cache checks `getHashValueIfUpToDate` before storing. The identity cache, the actions cache (RoD etc.) and the image cache still store under such a hash during concurrent edits. `NativeEffectBase::getOutputMetadata` (key/value metadata) also has no mark-count or write-gate check, so a hash that goes A→B→A during a derive can leave a stale entry. Not owned by any milestone yet.

### 2026-10-24T19:40:00-04:00 — info
- refs: none

Python bindings that return `StatusEnum` and can set a Python error from C++ (e.g. via `checkIsEditable`/`checkHolderIsEditable` on a placeholder node for an uninstalled plug-in), such as `ParametricParam.addControlPoint`/`setNthControlPoint` and other param setters, surface the error as `SystemError: ... returned a result with an exception set` instead of the intended exception, because the generated wrapper keeps converting the return value. M96 fixed this for its two new weighted calls with typesystem inject-code (`if (PyErr_Occurred()) return {};`). A sweep over the other `StatusEnum` methods that can raise is a follow-up, not owned by any milestone yet.

### 2026-10-24T23:30:00Z — info
- refs: none
User request: a Python API for undo steps, as context managers — e.g. `with app.newUndoStep("label"):` groups everything inside into one undo step (nesting folds into the outer step), and `with app.noUndoStep():` runs edits without recording them. Builds on M114 Phase 114.5 (Python edits recorded through the Engine-side UndoRecorderI with UndoStepGuard/UndoSuppressGuard), which exposes no Python API. It also covers edits made from PySide widget handlers in Python panels, which 114.5 leaves unrecorded because they have no C++ entry point. Decide names, where they live (app vs module-level), behaviour with no recorder (background/command line: no-ops), and exception handling (the step still closes; whether a failed block is rolled back).

### 2026-10-25T00:40 — change-request
- refs: none
- New E5 - GPU Compute milestone, user-requested as M156 - GPU Download Throughput In Comps' follow-up, name suggestion "Copy-Compute Contention": in fused comps, in-app device copies run at 7.5–11.7 GB/s down and 5.6–7.9 up, against GpuBench's 16.1 / 10.4 for an isolated host-import copy. GpuBench's overlap-on rows show ~10 / 9.9 whenever a copy runs beside compute. RADV here (RX 7900 XTX, `RADV_PERFTEST=transfer_queue`) gives copies no transfer-only queue in practice (`GpuDevice.cpp:308-334`), so they share the compute queues. Scope: measure whether a dedicated transfer/SDMA queue, or scheduling region copies apart from kernels (no overlap at region edges), recovers the isolated rate; then fix if it does. Data: `.local/m156/p2t2-ka1/`, `.local/m156/p2t1/`, M156's Decisions. Deps: M156.

### 2026-10-25T06:05:19+00:00 — change-request
- refs: M116
- Remove the dependency on M86, M87, and M162

### 2026-10-25T16:00:00Z — change-request
- refs: M70.P5.T2, M70.P5.T3
- From the user, via /cat-discuss, for M70 - RFV Declaration And Storage (see `DECISIONS/2026-10-25-rfv-promote-undo-and-active-copies.md`). (1) **Promote must be undoable.** "Add to project variables" becomes one undo step through M114's undo history: undo removes the project's copy, and redo restores it with the same value and animation. Amend this milestone's 2026-10-25 "Project variables and M114 undo" decision to make promote an exception to it; `app.createRFV*Param` is still not recorded. Add to T2's verify: a gtest runs promote, undo and redo. Add to T3's verify: an Xvfb check that Ctrl+Z after the right-click removes the variable from the project Variables section. (2) **Both copies stay active.** After a promote, the Set node's variable must not be disabled, read-only or greyed out, and both copies stay editable. The node's copy keeps overriding the project's upstream of that Set node. Only the menu item disables, with its tooltip. Add to T3's Xvfb verify a check that the node's knob is still enabled and editable after the promote, and include it in the screenshot. Code to change is already on milestone/m70 (03e610296 and the commits after it).

### 2026-10-25T07:40 — info
- refs: none
- **Nightly on main has failed since `9ab236449`** (runs 40163566294, 40167954015 and 40170317744). The `release-build-and-test` job fails `GuiThreadGLBinding` (from M161 - Startup Compositor Crash) and `TextRenderer` (from M172 - GL Widget Context Loss) in the release build; PR CI's debug build passes them. Found while the gpu PM dispatched Nightly on M171's branch (run 40170808335, which shows the same two failures). Needs an owner, likely bosley-native-nodes, which shipped both. M171 doesn't touch these tests.
- Resolved 2026-10-25 by bosley-native-nodes: Nightly ran on the bare ASWF image, whose Mesa lacks the system-LLVM relink, so GLX had no FBConfigs; both jobs moved to the dev image (#103, 823f55d2c), Nightly green on the branch.

### 2026-10-25T15:30:00Z — change-request
- refs: M129, M129.P1.T1, M129.P1.T2, M129.P1.T3, M129.P2.T1
- **M129 - Clean-Sheet Noise Node: FastNoise2 replaces the spike's candidates (user, /cat-discuss 2026-10-25).** FastNoise2 (github.com/Auburn/FastNoise2, MIT, same author as FastNoiseLite) has full 4D on every generator: Perlin, Value, Simplex, SuperSimplex, cellular value/distance (F1–F4 combinations, Euclidean/Manhattan/hybrid metrics), white, FBm/Ridged/PingPong fractals, and gradient/simplex domain warps, plus progressive/independent fractal warp. The milestone's note "FastNoiseLite is ruled out: 2D/3D only" is true of Lite only. MIT licensing removes the licence question (no GPLv3 consequence). Cycles and OpenSimplex2 are dropped as candidates. The spike now chooses between two ways of integrating FastNoise2, not between libraries. Apply to the milestone file:
  1. **Candidate sources:** add FastNoise2 as the chosen source with the facts above. Its kernels (`include/FastNoise/Generators/*.inl`) are scalar-looking code over FastSIMD vector types (`float32v`/`int32v`/masks), and gradients come from hash bit tricks with no lookup tables. FastSIMD is fetched at configure time through CPM, so linking it means vendoring FastSIMD. Relaxed mode uses `rsqrt`/`rcp` approximations. Mark Cycles and OpenSimplex2 as considered and dropped.
  2. **Replace P1.T1 (keep the ID):** "Port FastNoise2's 4D kernels to plain C++ and Slang (option A)". files: `tools/spikes/noise/fn2port/` (new: `fn2port4d.h`, `fn2port4d.slang`, `LICENSE`), `tools/spikes/noise/bench.cpp` (new). approach:
     - Port 4D Perlin, SuperSimplex and cellular distance F1 (Euclidean) plus FBm, line for line, into one C-like header and its Slang mirror. Map `float32v`→`float`, `int32v`→`int`, masks→`bool`, `FS::Select`→`select`, `Masked*`→ternaries, and `FMulAdd`→explicit `fma` on both sides.
     - Use exact `InvSqrt`/`Reciprocal`. Check `FS::Convert`'s float→int rounding and match it.
     - Write the CPU row loop so it autovectorises (SoA, no branches in the inner loop, `-O2 -march` baseline matching the build, no fast-math, `-ffp-contract=off` or explicit `fma`), and check the vectoriser report.
     - Harness: 2K tiles on CPU and GPU through the existing Vulkan test fixture.
     verify: the harness reports CPU ms/Mpx (single-thread and all cores), GPU ms/Mpx, max abs CPU/GPU difference, and the vectoriser report, and writes EXR samples to `build/spikes/noise/`. size: L.
  3. **Replace P1.T2 (keep the ID):** "Link FastNoise2 for the CPU, and benchmark A against B and FastNoiseLite (option B)". files: `tools/spikes/noise/fn2lib/` (new: build glue for FastNoise2 plus vendored FastSIMD at pinned revisions), `tools/spikes/noise/bench.cpp`. approach:
     - Build FastNoise2 with FastSIMD vendored at pinned revisions (no CPM network fetch), both in strict and in relaxed mode.
     - Render the same types and parameters through `GenPositionArray4D`, one call per row.
     - Benchmark three CPU paths on identical inputs: the P1.T1 port (A), FastNoise2's tuned SIMD (B), and FastNoiseLite's scalar 3D equivalents as the floor.
     - Measure A's and B's difference from each other and B's difference from P1.T1's GPU output.
     verify: the harness reports ms/Mpx (single-thread and all cores) for A, B (strict and relaxed) and FastNoiseLite, plus max abs differences A↔B and B↔GPU. size: L.
  4. **P1.T3 approach:** compare A against B, not libraries. Criteria:
     - **CPU speed (user):** A doesn't need every ounce of B's performance, but it must be faster than FastNoiseLite. The user expects a carefully written loop to come close to B.
     - **CPU/GPU parity:** A has parity by construction. For B, measure the gap.
     - **Maintenance:** one implementation (A) or two (B).
     - **Build cost:** vendoring FastSIMD and its runtime SIMD dispatch.
     Recommend A or B and checkpoint with the user. Licence is settled (MIT). Record the decision and fill in P2.T1's file list.
  5. **P2.T1 approach:** the core is FastNoise2-derived (option A: our ported header; option B: vendored FastNoise2+FastSIMD on the CPU with the Slang port for the GPU), per P1.T3. Add the types FastNoise2 lacks: hybrid multifractal, true cellular distance to edge (FastNoise2's F2−F1 only approximates it) and smooth F1. Write them ourselves; copy no Apache-2.0 code. Billow/turbulence is FBm over `Abs`.
  6. **Scope, In:** add a loop-period knob for seamless time loops. A 2D pattern loops through 4D (x, y, r·cos t, r·sin t), with the note that a looping 3D pattern would need 5D and is out. **Scope, Out:** a NoiseTool-encoded FastNoise2 node-tree knob, since the GPU would need a tree interpreter (no milestone; compose with several Noise nodes and a Merge instead). **Edge cases:** 4D cellular checks 81 cells per sample per octave, so the spike times it on the GPU.
  7. **Decisions:** append "2026-10-25 — FastNoise2 is the source (user): MIT and full 4D. The spike decides between porting its kernels to C++/Slang (A) and linking its SIMD library on the CPU with a Slang port (B). A must beat FastNoiseLite on the CPU but need not match FastNoise2's tuned SIMD." Amend the 2026-10-13 FastNoiseLite bullet to note that FastNoise2 is 4D. The 2026-10-13 spike decision is superseded: the Cycles and OpenSimplex2 candidates and their licence choice are dropped.

### 2026-10-25T18:00:00Z — change-request
- refs: M92
- From the user's M92 - Linked Nodes UAT. Three changes, each reversing or refining shipped behaviour (the owning PM should add tasks and record decisions):
  1. **Copy, paste and duplicate make an identical copy, links included.** Reverses M92.P2.T1 and the gate item "Right-click Duplicate and Paste produce independent copies". Keyboard and right-click Duplicate, Copy/Paste and clipboard paste all keep the copy's knob links and Linked Node membership; the copy joins the original's link groups (it is not independent, and the copy is drawn as linked). Supersedes the 2026-10-25 decision that pasted copies link only to each other. Alias (permanent) groups still follow whatever the Group copy already does. Update the gate wording and undo/redo coverage.
  2. **Each link line points to only the nearest other node.** Today every member draws to every other member, making a web. Per link group (and per expression dependency set), draw one line to the single nearest other member node, by node-centre distance. A fast approximation is fine; exactness is not required. Applies to both link and expression lines, and to the dashed Linked Node lines.
  3. **Swap the dash styles.** Whole-node links (Linked Nodes) now draw **solid**; single-knob links (Paste Link, drag-to-link, alias) draw **dashed**. Today it is the reverse. Update the Xvfb screenshot expectations in M92.P2.T4 and the gate.

### 2026-10-25T12:00:00Z — info
- refs: Engine/Knob.cpp setExpressionInternal; found during M92
`setExpressionInternal` rethrows a caught `const std::exception&` with `throw e;`, slicing a `std::runtime_error` to a plain `std::exception` and dropping its type. Use `throw;`.

### 2026-10-25T06:30:00Z — info
- refs: M167
A Switch's per-file key/value metadata (custom header attributes) always comes from its input 0, not the input it routes to at that frame, so a Write over a keyed Switch writes the first source's custom attributes into every file. Found while writing M167 P5.T4 (NativeWriteMetadata EachFileOfASequenceTakesItsOwnFramesFormat checks only size and PAR). Fix belongs in Switch / NativeEffectBase::getOutputMetadata routing; outside M167's files.

### 2026-10-26T19:30:00Z — info
- refs: M19, M20
- From the user (2026-10-26): merge M19 - USD/Hydra Foundation and M20 - 3D Node Vocabulary as they are, plus the two UAT fixes in the 2026-10-26T19:00:00Z change-request for M20 - 3D Node Vocabulary. None of the follow-up MVP work goes in: that is M226–M235, now on the board. Remaining gate items: tick M19.P3.T6 (its numbers are already recorded in M19's file); rebuild the AppImage with the two fixes for the user's re-check; and run the AMD/NVIDIA hardware checks with screenshots, unless the user confirms their UAT covered them. Then merge M19 first, without `--delete-branch` (that would close M20's stacked PR). Retarget M20's PR to main via REST and merge it.
- processed 2026-10-26 by bosley-native-nodes: M20.P4.T7–T8 added; M19.P3.T6 ticked.

### 2026-10-26T19:00:00Z — change-request
- refs: M20
- From the M19/M20 UAT grill (user chose to fix these before M20 - 3D Node Vocabulary's gate). Add two fix tasks to M20 - 3D Node Vocabulary. (1) **File pickers show no files with the `*` filter.** This is an M16 regression that hits every file knob not marked as an input image: USDRead's File, USDMaterial's texture knobs, USDWrite's save dialog. `Gui/KnobGuiFile.cpp:157-161` passes `{"*"}`. `SequenceFileDialog::setDefaultFilter` (`Gui/SequenceFileDialog.cpp:1942-1949`) turns that into a bare `*`. `FileSystemModel::setRegexpFilters` (`Engine/FileSystemModel.cpp:1043`) skips the `*` token, so `isAcceptedByRegexps` (`:1079-1092`) rejects every file. Fix: an empty regexp list with `*` means accept all. Also give USDRead's knob the USD/Alembic extensions and the texture knobs image extensions. Verify with a gtest on FileSystemModel and an Xvfb check that the USDRead dialog lists `.usda` files. (2) **Right-click leaves a stuck dolly.** `SceneViewport::mousePressEvent` (`Gui/SceneViewport.cpp:1517-1533`) stores `_imp->button` for every press. The context menu then grabs the release, so `mouseReleaseEvent` never resets it, and later hover moves dolly (`:1564-1565`). Fix: set the button only on accepted presses, clear it in `contextMenuEvent`, and check `e->buttons()` in move. Verify with an Xvfb script: right-click, Esc, then mouse moves leave the camera unchanged. (Paths from the m20 worktree.)
- processed 2026-10-26 by bosley-native-nodes: M20.P4.T7–T8 added; M19.P3.T6 ticked.

### 2026-10-26T21:00:00Z — change-request
- refs: none
- From M71 - RFV Render Context (bosley-native-nodes). Pre-existing: a knob's cached expression result (`_exprRes`) for an expression that reads no render context is keyed by time only, not view, so an expression whose value depends on the view (e.g. reads `view` or a per-view knob) returns the first view's cached value for every view in a multiview render. Key the cache by (time, view), or by view only when the expression reads the view; add a two-view test.
- processed 2026-10-26 by bosley-native-nodes: fixed in M71 - RFV Render Context (expressions evaluate at the requested view; caches keyed by time and view).

### 2026-10-26T17:00:00Z — change-request
- refs: none
- From bosley-native-nodes (PR CI). `Comps/CacheEquivalenceBenchCompTest` runs 372–434 s inside the debug ctest legs against a 450 s timeout (M177's PR CI: 386 s and 434 s; M92's legacy leg timed out at 450 s, then passed alone in 190 s). It flakes under load on every branch. Shrink the comp or its frame count so it runs well under the timeout, keeping what it proves.
- processed 2026-10-26 by bosley-native-nodes: became M237 - CI Flake Fixes.


### 2026-10-27T05:00:00Z — change-request
- refs: M122, M123, M129, M185
- From gpu (user decision, 2026-10-26): cost seeds are abolished (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). M239 - Weight-Based GPU Placement deletes `*.costseed.inc`, `Engine/GpuCostSeeds/`, `render-profile-report.py --emit-cost-table` and the measured cost model. Nodes declare `gpuWeight()` instead (Engine/GpuWeight.h). Override it only when cost grows with a knob (`gpuSizeScaledWeight`) or the work always wants the GPU (`GpuWeight::heavy()`). In M122 - Native Colour Utilities, M123 - Native Sharpen And Glow, M129 - Clean-Sheet Noise Node and M185 - Variable Warps, strike any unfinished task or clause that measures or writes cost seeds, and don't add new `*.costseed.inc` files. Seed files already on a branch are deleted when that branch rebases after M239 merges, or by M239's rebase if your milestone merges first.

### 2026-10-25T12:00:00Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation (bosley-faux). New milestone suggested ("Startup Pane Paint On NVIDIA"): on NVIDIA GL under KWin (RTX 3070 Ti, driver 580.178.04, :0), a newly started Natron window shows its panes unpainted (#202020) on screen for ~16–18 s after map; only the menu bar and left toolbar draw. Happens with NATRON_GPU=off too, never under Xvfb. Screenshot `.local/m168/diag/run1-live.png`. User-visible startup paint issue, not the GPU hand-off.

### 2026-10-25T12:00:01Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation. Small test fixes for M107 - GPU Viewer Hand-off's UAT: (1) "the viewer shows the comp" in `Tests/gui/m107_viewer_handoff.py` checks only non-black, so an unpainted #202020 viewer passes; use `looks_blank`. (2) The UAT docstring and `Tests/gui/run-gui-test.sh`'s header should say host-display runs (GUI_TEST_HOST_DISPLAY) need a CPU reference taken on the same display. (3) Cosmetic: `tools/gpu-spike/desktop-handoff-check.sh` warns "GL renderer is not radeonsi" on NVIDIA.

### 2026-10-25T15:00:00Z — change-request
- refs: none
- From M169 - Pipelined Strip Member Timing (bosley-faux). Two follow-ups: (1) Flake: `GpuResidencyTest.RegionMembersAreTimedOnTheDevice` failed `EXPECT_GT(blurNs, gradeNs)` in whole mode once in 10 NVIDIA runs (Grade 0.260 ms vs Blur 0.209 ms, first region of the process, no transfer overlap). (2) Spike-robust GPU calibration: 2–16× slow member intervals with no transfer overlap appear in every mode on a shared desktop GPU (22–50% utilisation from other programs) and swing `GpuCostModel`'s EMA; consider outlier rejection or a robust estimator. Data: `.local/m169/p1/` on bosley-faux.

### 2026-10-25T17:00:00Z — change-request
- refs: none
- From M169 - Pipelined Strip Member Timing (bosley-faux), NVIDIA RTX 3070 Ti gate: (1) `CurveSnapshotPrecisionTest.CurveMulKeepsTheProductError` (M171 - GPU Correctness Fixes' test) fails on NVIDIA in both schedulers and both push-descriptor variants: 25 BelowTiny pairs (`8e-31 × [0.5,1]`), e.g. `|hi+lo-exact| 5.06e-39 > 3.07e-44`. Residuals sit just above FLT_MIN but the Dekker partial products are subnormal and appear flushed to zero; the test's `representable` guard checks only the final residual. Passes on lavapipe. (2) `RenderSchedulerTest.CachedOutputsOfTwoFramesAtOnce` fails consistently on bosley-faux (leaf nodes render twice, with or without GPU) but passes on CI; undiagnosed. (3) The bosley-faux host env lacks git-lfs (raw fixtures are pointers) and has stale `build/assets/Plugins` (fetch-assets --check-pins fails); a host-setup note, not code. Logs: `.local/m169/p3/out/`.

### 2026-10-27T06:30:00Z — change-request
- refs: none
- From gpu, M239 - Weight-Based GPU Placement bench. New milestone suggested ("Write Path Variables"): on main (52d58e736 and later), the native Write no longer expands `[Project]` in its output path. `NatronRenderer -w Write 1-1 tools/bench/comps/keying_grade.ntpz` fails with `Could not open "[Project]/../../../build/bench/m82-profile/out/.keying_grade.0001.tmp-….exr" (No such file or directory)`, from any working directory, with GPU on or off. All three bench comps (keying_grade, cg_multipass, defocus_retime) are affected, so `tools/bench/comps/run_comps.sh` is broken. The literal `[Project]` reaches the file open. Workaround used for the bench: copies of the comps with absolute Write paths.

### 2026-10-26T16:00:00Z — change-request
- refs: none
- From M185 - Variable Warps UAT (bosley-native-nodes, user). New milestone suggested ("Cache Invalidation Audit"): the user saw STMap keep rendering as if its old Map were still connected after the Map input was disconnected; not reproducible later. Thoroughly audit cache invalidation for holes: input connect/disconnect (including optional inputs such as Map/Mask), node hash and render-hash composition, the ActionsCache, the image and viewer texture caches, GPU residency and region caches, the timeline cached-frames line, expression and link invalidation, and per-frame metadata retention. Related: M178 - Render Context Core's UAT found the timeline line ignoring invalidation and OFX binary stamps changing per AppImage mount. Deliver a written map of every cache and what invalidates it, stress tests (random edit/connect/disconnect sequences compared against a cache-off render), and fixes.

### 2026-10-27T05:18:45+00:00 — change-request
- refs: M207
- Use OCIO to do the compression; can use an srgb display-referred space that compresses to 0-1 to go from most colourspaces to srgb, and it's invertible.

### 2026-10-27T05:22:05+00:00 — change-request
- refs: M187
- Hold on this til my say-so.

### 2026-10-27T05:22:20+00:00 — change-request
- refs: M188
- Hold on this until my say-so.

### 2026-10-27T05:19:47+00:00 — change-request
- refs: M203
- Spike/investigate if we _need_ to end GPU regions for this to be practical.
- Withdrawn unapplied (user, 2026-10-27): superseded by the M246 - Vulkan Inference Provider stub and the 2026-10-27T18:00:00Z M203 change-request.

### 2026-10-26T23:00:00Z — change-request
- refs: M101
- From gpu (user decision, 2026-10-26): cost seeds are void (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). Strike any unfinished M101 - Clean-Sheet Merge Node task or clause that measures or writes a `*.costseed.inc`, and don't add the README cost-seed checklist step. M239 - Weight-Based GPU Placement deletes the seed machinery; if M101 lands first, its already-committed seed file is removed in M239's rebase.
- Archived unapplied (user, 2026-10-28): M101, M123 and M185 are all done, so no unfinished task carries a cost seed.

### 2026-10-27T12:00:00Z — change-request
- refs: M123, M185
- From cat-plan, re-filed from the 2026-10-27T05:00:00Z entry (M122 - Native Colour Utilities and M129 - Clean-Sheet Noise Node are done, so only these two remain). Cost seeds are abolished (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). M239 - Weight-Based GPU Placement deletes `*.costseed.inc`, `Engine/GpuCostSeeds/`, `render-profile-report.py --emit-cost-table` and the measured cost model, and nodes declare `gpuWeight()` instead (Engine/GpuWeight.h). Override it only when cost grows with a knob (`gpuSizeScaledWeight`) or the work always wants the GPU (`GpuWeight::heavy()`). In M123 - Native Sharpen And Glow and M185 - Variable Warps, strike any unfinished task or clause that measures or writes cost seeds, and don't add new `*.costseed.inc` files. Seed files already on a branch are deleted when that branch rebases after M239 merges, or by M239's rebase if your milestone merges first.
- Archived unapplied (user, 2026-10-28): M123 and M185 are done, so no unfinished task carries a cost seed.

### 2026-10-27T18:00:00Z — change-request
- refs: M203.P1.T2, M203.P1.T3, M203.P2.T1
- From cat-plan (user, 2026-10-27). M246 - Vulkan Inference Provider will later register onnx-vulkan-rs, an ONNX Runtime plugin execution provider, through `RegisterExecutionProviderLibrary`. Keep M203 - ONNX Inference Backend open to that without building it: (1) M203.P1.T2: the loader keeps a data-driven provider table (name, how it is detected, how it is appended to session options) rather than hard-coding the Auto order and probe list, so a plugin EP library registered after `OrtEnv` creation can add an entry. (2) M203.P1.T3: `aiDevice`'s choices come from that table, not a fixed enum. Persist the provider name, so an entry added later doesn't shift saved values. (3) M203.P2.T1: the `SessionCache` key has room for an EP-specific part; M246 needs the input shape there, because the Vulkan plugin needs concrete input dimensions. No plugin loading, no Vulkan entry and no new UI in M203.
- Archived unapplied (user, 2026-10-28): M203 is done, so its tasks are no longer editable; M246 - Vulkan Inference Provider owns the work.

### 2026-10-27T07:30:00Z — info
- refs: none
- From gpu: main at 622c03219, after M178 - Render Context Core merged, fails two tests 5 times out of 5 in a local debug build with ctest on RADV (`NATRON_DEV_GPU=1`): `PersistentMessageTest.RenderFailingAfterItsChannelCheckPassedKeepsTheError` (Tests/PersistentMessage_Test.cpp:437) and `ShuffleRenderTest.ExplicitRowVariesPerFrameOnShuffleDisableChain` (Tests/ShuffleRender_Test.cpp:361). Both fail on `hasPersistentMessage()` being unexpectedly true. They passed on a branch based just before M178. Unconfirmed suspect: M178's new `contextCountWarningMessage` source in Node.cpp. Neither test uses a node the gpu PM's branches touch. It hasn't been checked whether PR CI on lavapipe also fails them. Needs an owner: a fix milestone or the M178 author.
- Applied (user, 2026-10-28): filed as M271 - Persistent Message Test Failures.

### 2026-11-02T13:25:54+00:00 — change-request
- refs: M53
- I don't think the plan is correct for this. The idea is painting in the 3d viewport, mari-style, not some kind of 2d to 3d bridge where paint in 2d goes back to 3d.
- applied: 2026-11-03 by bosley-native-nodes, together with the redesign entry below

### 2026-11-03T01:33:08+00:00 — change-request
- refs: M53
- /cat-grill (hazel-raven) redesign of M53 - Projected Texture Painting with the user, answering the 13:25 change-request for M53 (archive that entry with this one). Replace `PLAN/MILESTONES/M53-projected-texture-painting.md` with the content below: new intro and `## Scope`, M53.P3.T1–T2 cancelled, new Phases 53.4–53.6 (13 tasks), new verification gate, ten 2026-11-02 Decisions. Phases 53.1–53.2 stay as built. The board's Deps change and the new stubs are a separate plan-scoped entry. Until the UDIM Live Textures stub exists and is done, M53.P4.T1–T3 can proceed; M53.P5.T3 ships the one-tile Output Tile fallback.

````markdown
# M53 - Projected Texture Painting

Full title: Mari-style texture painting in the Viewer's 3D view

Redesigned in the 2026-11-02 grill (user: "painting in the 3d viewport, mari-style, not some kind of 2d to 3d bridge"). M53 delivers a `TexturePaint` image node: you paint directly on meshes in the Viewer's 3D view with RotoPaint's brushes. Strokes land in a screen-space paint buffer, and navigating (or a Bake key) projects the buffer into the node's UDIM textures with occlusion. The UV bake (`SceneRaster`) and the projection logic (`ProjectTexture`, now internal) from Phases 53.1–53.2 are the bake engine. The common workflow: project plates onto geo with M234 - Camera Projection Nodes, then clean up in 3D. Live textures are M233 - Live Node Textures; the multi-tile URI is the new UDIM Live Textures stub, which M53 depends on. Read `StageCache` below as M19's `SceneQueryPool`.

## Scope

**In:**
- `TexturePaint` image node: Scene input (meshes via Prim Pattern; all matches share one UDIM texture set), output a multi-layer UDIM texture image that feeds M233's `USDMaterial` image inputs.
- Mari paint buffer in the Viewer's 3D view: strokes are stored in normalized view coordinates with the view camera they were painted through; the buffer has its own Buffer Resolution knob, independent of window size.
- Bake on orbit/pan/zoom/tool switch, plus a Bake key; the bake is async and never blocks navigation (unbaked strokes stay drawn until the textures update, progress shown, new strokes queue behind).
- All RotoPaint brush types (Solid, Eraser, Clone, Reveal, Blur, Sharpen, Smear, Dodge, Burn) via the Roto stroke engine.
- Natron's layer/channel system, as in RotoPaint: each stroke targets a layer and channels. The layer→material-purpose mapping travels as stream metadata (M74 - Native Metadata Core); TexturePaint writes it and downstream nodes may read it.
- Strokes are the truth, with a pixel cache. Internal bake engine: **Bake Inputs** freezes the upstream look (bound material channels, including USDProject3D projections) into one set of UDIM files as the paint base; **Bake Output** flattens strokes so far into a second set, a cache that freezes those strokes and a hand-off.
- Tile Resolution knob (1K/2K/4K/8K, default 4K).
- Static/Animated texture mode: in Animated mode strokes have RotoPaint lifetimes, caches are per frame and the output is a texture sequence. Animated cameras are supported.
- Undo per stroke and per bake.
- `ProjectTexture` folds into internals and is no longer a user node.

**Out:**
- UDIM token on live-texture URIs and `USDMaterial`: new stub "UDIM Live Textures" (M53 depends on it).
- Paint layered over an existing material (mix per channel; UsdPreviewSurface/glslfx now, MaterialX later): new stub "USDLayerMaterial".
- Standalone bake node that writes UDIM files, sharing M53's bake engine, plus M234 - Camera Projection Nodes' USDWrite bake for meshes that aren't card-like: new stub "USDBakeTextures".
- Paint-through 2D inputs, Screen or Camera mode per input, including plates through animated cameras: new stub "Paint-Through Inputs".
- Direct texel (raycast splat) painting: new stub "Direct Texel Painting".
- Baking render-delegate renders (e.g. Cycles) onto UVs: new stub "Delegate UV Baking"; the bake engine keeps a pluggable backend for it.
- Deforming geometry: none (geometry is static).
- The 2D-paint `PaintThroughCamera` preset: none (cancelled, wrong shape).

**Edge cases:**
- Matched meshes whose UVs overlap: a node warning naming the clashing prims; use one TexturePaint per object.
- A base texture of a different size than Tile Resolution: resampled, with a warning.
- A stroke started while a bake runs: queued behind it.
- Undoing a Bake Output: those strokes become live again, and the cache is discarded and rebuilt from strokes.
- Animated camera or plate in Animated mode: baked per frame.

**Assumptions:**
- Unconfirmed: bake file paths are knobs with a `<UDIM>` token (and a frame token in Animated mode), defaulting to project-relative paths.
- Unconfirmed: a matched mesh without the UV primvar gives a node error naming the prim (as ProjectTexture does).
- Unconfirmed: the paint tool is active in the 3D view while a TexturePaint node's panel is open, with RotoPaint's toolbars.
- Unconfirmed: pen pressure behaves as in RotoPaint.
- Unconfirmed: Clone/Reveal sample a CPU unlit textured view of the base plus baked textures through the session camera (deterministic, no GL in ctest), not a readback of Storm's lit frame.
- Unconfirmed: until UDIM Live Textures lands, the output shows one tile chosen by an Output Tile knob (default 1001).
- A native node can host a RotoContext and get Roto serialization and toolbars; M53.P5.T1 and M53.P6.T2 check this.

## Phase 53.1: CPU raster substrate

- [x] M53.P1.T1 — `SceneRaster`: camera-space z-buffer and UV-space bake over a composed stage
  - files: `Engine/Nodes/Scene/SceneRaster.h`/`.cpp`, `Tests/SceneRaster_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: pure-CPU triangle rasterizer, no GL. Input: composed `UsdStage` (via `StageCache`), `UsdTimeCode`, prim set. Triangulate `UsdGeomMesh` faces with Hydra's `HdMeshUtil` (ships in the USD lib; handles n-gons and primvar index remapping). Mode A, camera: given a `UsdGeomCamera` path and a resolution, write depth, prim id and world position per pixel with a z-test. Mode B, UV bake: given one prim, its `st` primvar name and a resolution, rasterize triangles in UV space and write world position and world normal per texel, plus a coverage mask. Parallel over scanline chunks (reuse `NativeEffectBase::makeDeepScanlineChunks`' partition shape). Subdivision surfaces rasterize their control cage (documented limitation).
  - verify: unit tests on hand-authored `.usda` — a unit quad facing the camera fills the expected pixel rect with monotonic depth; a UV-mapped quad bakes a full 0–1 coverage mask whose corner texels carry the quad's corner world positions.
  - size: L

- [x] M53.P1.T2 — Primvar interpolation and UDIM tile selection in the UV bake
  - files: `Engine/Nodes/Scene/SceneRaster.cpp`, `Tests/SceneRaster_Test.cpp`
  - approach: handle `faceVarying`, `vertex` and `varying` `st` (indexed or not) through `HdMeshUtil`'s primvar remap; add a UDIM tile parameter so the bake covers tile `1001 + u + 10*v` — UV values are offset by the tile origin before rasterization. Missing `st` sets a node error naming the prim.
  - verify: unit test — the same quad with `faceVarying` indexed `st` bakes identically to the `vertex` case; a quad whose UVs lie in tile 1002 bakes empty at 1001 and full at 1002.
  - size: M

## Phase 53.2: Nodes

- [x] M53.P2.T1 — `ProjectTexture` node: scene + image → UV-space image
  - files: `Engine/Nodes/Scene/ProjectTexture.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/ProjectTexture_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene, Image (the thing to project; typically a `USDRender` + paint, or any image). Knobs: camera path, `PrimSelector` (first match is the bake target), `st` primvar name, UDIM tile, output resolution, occlusion on/off with a depth bias, edge dilation in texels. Render (output kind **image**, RoD = resolution): UV-bake the prim with `SceneRaster`; for each covered texel project its world position through the camera into the input image's pixel space and sample bilinearly; texels occluded per the camera z-buffer, off-screen, or back-facing get alpha 0; dilate the result by N texels so bilinear texture filtering doesn't bleed black. The input image is fetched for the camera's full frame at the project format. Result is a normal `Image` and caches normally.
  - verify: unit test — a constant red image projected onto a camera-facing UV quad yields an all-red, alpha-1 texture inside the coverage mask; a second quad in front of half of it leaves that half at alpha 0 with occlusion on and red with it off.
  - size: L

- [ ] ~~M53.P2.T2 — Live-texture registry and `Ar` resolver plugin~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Engine/Nodes/Scene/LiveTextureRegistry.h`/`.cpp`, `Engine/Nodes/Scene/LiveTextureResolver.h`/`.cpp` (`ArResolver` + `ArAsset`), `Engine/Nodes/Scene/plugInfo.json` + CMake to install it where `PlugRegistry` finds it, `Tests/LiveTexture_Test.cpp`
  - approach: URI scheme `natron://<nodeScriptName>/<plane>?v=<imageHash>`. The registry maps node script name → weak `NodePtr` plus the time the image was registered for (set by `USDMaterial` at layer-authoring time). The resolver answers `Resolve()` with the URI itself when the node exists and `OpenAsset()` with an in-memory `ArAsset` wrapping the node's rendered RGBA float image for that time. A URI whose node is gone resolves to nothing, so Hydra shows the missing-texture fallback rather than crashing. Registered via `plugInfo.json`; USD's plugin path must include the Natron binary's plugin dir in the installed and AppImage layouts.
  - verify: unit test — register a `Constant` node, `ArGetResolver().OpenAsset()` on its URI returns pixel bytes of the expected size; unregistering makes the resolve fail cleanly. Packaging (M15) still builds with the plugin dir.
  - size: L

- [ ] ~~M53.P2.T3 — `Hio` image plugin for live-texture assets~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Engine/Nodes/Scene/LiveTextureImage.h`/`.cpp` (`HioImage` subclass), `Engine/Nodes/Scene/plugInfo.json`, `Tests/LiveTexture_Test.cpp`
  - approach: an `HioImage` implementation for the `natron` scheme that reads the `ArAsset` from T2 as RGBA float with the correct size and orientation (Natron images are bottom-up; Hio expects top-down — flip once). Registered in the same `plugInfo.json`. This is what lets Storm's texture loader consume a node output without touching disk.
  - verify: unit test — `HioImage::OpenForReading()` on a registered `Constant` URI reads back matching dimensions and the constant's colour at the corners.
  - size: M

- [ ] ~~M53.P2.T4 — `USDMaterial` texture inputs bound as live textures~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Engine/Nodes/Scene/USDMaterial.cpp`, `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/` (USDMaterial and USDWrite tests)
  - approach: add optional **Image** inputs to `USDMaterial` for diffuseColor, roughness, metallic, normal and emissive alongside the existing file-path knobs (a connected input wins). For a connected input, author a `UsdUVTexture` shader whose `file` is the `natron://` URI with `?v=<input image hash>`, register the node with `LiveTextureRegistry`, and fold the input image hash into the node's layer hash so a changed texture yields a new layer, a new stage and a new texture path Storm reloads. `USDWrite` gains "export live textures": on export, each `natron://` reference is rendered to `<usd basename>_textures/<node>_<plane>.exr` next to the output and the exported layer's path is rewritten to it.
  - verify: unit test — a `Constant` → `USDMaterial` diffuse input composes a `UsdUVTexture` with a `natron://` asset path; `USDWrite` produces the `.exr` and a layer referencing the relative file path that `USDRead` composes cleanly.
  - size: M

- [ ] ~~M53.P2.T5 — The Viewer's 3D view refreshes when a live texture's source changes~~ — cancelled 2026-10-26: moved to M233 - Live Node Textures.
  - files: `Gui/SceneViewport.cpp`, `Engine/Nodes/Scene/USDMaterial.cpp`
  - approach: the viewport already re-composes when the viewed node's stack hash changes (M19.P1.T3); confirm the `?v=<hash>` path change propagates through Storm's texture registry as a reload, and add an explicit `HdChangeTracker` material-dirty mark if a stale texture persists. Keep it simple: whole-texture reload per change is acceptable for v1 (design doc, Risks).
  - verify: manual — paint a stroke upstream of a live-textured material and see it appear on the model in the Viewer's 3D view without touching the viewport.
  - size: S

## Phase 53.3: Paint workflow

- [ ] ~~M53.P3.T1 — `PaintThroughCamera` preset group~~ — cancelled 2026-11-02: 2D-to-3D bridge, replaced by painting in the 3D view (Phases 53.4–53.6).
  - files: a shipped PyPlug/preset under the existing built-in presets location, `Gui/` only if the preset loader needs a menu entry, brief usage notes in `Engine/Nodes/Scene/README.md` (create if absent)
  - approach: a group node wiring `USDRender`(camera) → `RotoPaint` → `ProjectTexture`(same camera, same prim) → 2D `Merge` over an optional existing-texture input → output image, with the camera path and prim selector promoted to the group. Its output feeds `USDMaterial`'s diffuse input. Multi-angle painting is several of these merged in 2D; say so in the notes.
  - verify: manual on real hardware — paint a stroke in the 2D viewer on the rendered view; the stroke shows on the model in the Viewer's 3D view from a different angle, correctly occluded where another prim was in front.
  - size: M

- [ ] ~~M53.P3.T2 — Paint pipeline integration test in CI~~ — cancelled 2026-11-02: superseded by the TexturePaint gtests and M53.P6.T3.
  - files: `Tests/ProjectTexture_Test.cpp` (extend), reference `.usda` with a UV-mapped quad and an occluder
  - approach: headless graph — `USDRead` + `USDCamera` + `Constant` → `ProjectTexture` → assert coverage and occlusion; `ProjectTexture` → `USDMaterial`(live) → `USDWrite`(export textures) → assert the exported `.exr` matches the `ProjectTexture` output. No GL involved (`SceneRaster` is CPU).
  - verify: test green in `build-and-test`.
  - size: M

## Phase 53.4: Paint substrate

Before M53.P5.T4, merge the current tips of M233 - Live Node Textures, M52 - USD Layer-Stack Completion and M234 - Camera Projection Nodes into this branch: `PrimSelector`, `LiveTextureStore` and `USDProject3D` are not on main.

- [ ] M53.P4.T1 — `SceneRaster`: render through an arbitrary view camera and output UV per pixel
  - files: `Engine/Nodes/Scene/SceneRaster.h`/`.cpp`, `Tests/Scene/SceneRaster_Test.cpp`
  - approach: `CameraArgs` only accepts `cameraPath` (SceneRaster.h:53). Add an optional explicit camera (camera-to-world as a Gf row-major matrix, view height, aspect, orthographic flag, clip range). It mirrors main's `SceneViewportRenderer::CameraPose` but lives in Engine, since Engine must not include Gui. Add an optional `st` + UDIM-tile buffer to `CameraBuffers` for the CPU unlit textured view.
  - verify: gtest — a camera given as a matrix gives the same depth/primIndex as the same `UsdGeomCamera` given by path; the UV buffer at a quad's corners matches its authored `st`.
  - size: M

- [ ] M53.P4.T2 — Fold ProjectTexture into an internal `TextureProjection` helper and unregister the node
  - files: `Engine/Nodes/Scene/TextureProjection.h`/`.cpp` (new), delete `Engine/Nodes/Scene/ProjectTexture.{h,cpp,reg.inc}`, `Tests/Scene/ProjectTexture_Test.cpp` → `Tests/Scene/TextureProjection_Test.cpp`
  - approach: move the private `ProjectTexture::renderTexture` logic into a free function. It takes a float buffer + the M53.P4.T1 camera + one mesh + `st` name + UDIM tile + resolution, and returns RGBA + coverage, keeping occlusion, depth bias, back faces and edge dilation. Deleting the `.reg.inc` unregisters it (Engine/CMakeLists.txt globs `Nodes/*.reg.inc`).
  - verify: the existing red-quad and half-occluder cases, ported to the helper, give identical results; `ProjectTexture` is missing from the plug-in list.
  - size: L

- [ ] M53.P4.T3 — Let the Roto stroke tree run on a node other than RotoPaint, with a host-supplied source
  - files: `Engine/RotoDrawableItem.cpp`, `Engine/RotoContext.h`/`.cpp`, `Engine/EffectInstance.h`/`.cpp`, `Tests/RotoLayer_Test.cpp`
  - approach: `refreshNodesConnections` hard-codes the host's input 0 as the bottom of the tree (RotoDrawableItem.cpp, `getContext()->getNode()->getInput(0)`). Add `virtual NodePtr EffectInstance::getPaintTreeSource()`, defaulting to input 0. Host nodes opt in through `getCurveContextKind()` returning `eCurveContextKindPaint` (EffectInstance.cpp checks the plug-in ID only), and `RotoContext`'s `isDefaultPaintNode` must accept them too.
  - verify: the Roto gtests pass unchanged; a new gtest has a test effect whose source is a `Constant`, and its Solid stroke merges over that constant, not over input 0.
  - size: M

## Phase 53.5: TexturePaint node

- [ ] M53.P5.T1 — `TexturePaint` node: knobs, a hosted RotoContext, paint sessions, save/load
  - files: `Engine/Nodes/Scene/TexturePaint.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Scene/docs/TexturePaint.md`, `Tests/Scene/TexturePaint_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Scene input; `PrimSelector` Prim Pattern (all matched meshes share one UDIM set); UV Primvar; Tile Resolution 1K/2K/4K/8K (default 4K); Buffer Resolution; Static/Animated mode; Bake Inputs / Bake Output paths with a `<UDIM>` token. A *session* is the set of strokes painted through one view camera: a `RotoLayer` carrying the M53.P4.T1 camera plus a frozen flag, all serialized. Knob labels match script names. Check that hosting a RotoContext gets Roto serialization.
  - verify: gtest — create the node, add a session with two strokes, save the project and load it; strokes, camera matrices, frozen flags and knob values all round-trip.
  - size: L

- [ ] M53.P5.T2 — Render a session's strokes into the paint buffer, all nine stroke types
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintView.h`/`.cpp` (new), `Tests/Scene/TexturePaint_Test.cpp`
  - approach: `TexturePaintView` is an internal image source: the CPU unlit textured view through the session camera at Buffer Resolution, M53.P4.T1's UV buffer sampled from the current base plus the baked textures. It is returned from `getPaintTreeSource()` (M53.P4.T3), so Clone/Reveal/Blur/Sharpen/Smear/Dodge/Burn sample the 3D view. Strokes are stored in normalized view coordinates and rasterized at Buffer Resolution; each targets a layer and channels as RotoPaint's do.
  - verify: gtest — a Solid stroke fills the expected buffer pixels; Eraser removes them; Clone copies view pixels by the offset; a stroke on `rgba` leaves another layer untouched; the buffer size does not change with the viewer size.
  - size: L

- [ ] M53.P5.T3 — Bake sessions into UDIM tiles; multi-layer output with layer→material-purpose metadata; overlapping-UV warning
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintCache.h`/`.cpp` (new), `Tests/Scene/TexturePaint_Test.cpp`
  - approach: for each matched mesh and covered tile, `TextureProjection` (M53.P4.T2) projects each session buffer into a per-tile pixel cache, base under strokes. Lazy, per tile, half-float on disk (an 8K float RGBA tile is about 1 GB per layer). Until UDIM Live Textures lands, the output image is one tile chosen by an Output Tile knob (default 1001), as multiple planes via `getComponentsNeededAndProduced`. Override `deriveOutputMetadata` to write keys such as `natron/texturePaint/purpose/rgba = diffuseColor`. Meshes whose UVs overlap get a node warning naming both prims.
  - verify: gtest — stroke → buffer → tile 1001 shows paint only on the visible half (occluder case) and none on back faces; the metadata keys are present downstream of a `ViewMetadata`; two overlapping quads produce the warning text.
  - size: L

- [ ] M53.P5.T4 — Bake Inputs: freeze the bound material's look into UDIM base files
  - files: `Engine/Nodes/Scene/MaterialBake.h`/`.cpp` (new), `Engine/Nodes/Scene/TexturePaint.cpp`, `Tests/Scene/TexturePaint_Test.cpp`
  - approach: evaluate on the CPU at each texel from `SceneRaster::bakeUV`: UsdPreviewSurface constants, `UsdUVTexture` files (OiioReadSupport), `natron://` (M233 `LiveTextureStore`), and USDProject3D's projection, which reimplements M234's `natronProject3D.glslfx` with `TextureProjection`. Keep the evaluator behind an interface so USDBakeTextures, MaterialX and delegate backends can plug in later. Write one half-float EXR per tile per purpose (OiioWriteSupport); a base of a different size is resampled to Tile Resolution with a warning.
  - verify: gtest — a material with constant red diffuse bakes red into `<UDIM>`=1001; a 1K file texture at 2K resamples and warns; a USDProject3D card bakes its plate; a deleted base file reloads as missing with an error.
  - size: L

- [ ] M53.P5.T5 — Bake Output, freezing, and undo per stroke and per bake
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintUndo.h`/`.cpp` (new), `Engine/RotoUndoCommand.h` (reuse), `Tests/Scene/TexturePaint_Test.cpp`
  - approach: Bake Output writes base plus all sessions so far to the output files and marks those sessions frozen; frozen strokes are not re-rendered and come from the files. A stroke is undone through RotoPaint's own undo commands; a bake through a new command that unfreezes the sessions, discards the cache and rebuilds it from strokes. Strokes stay the truth.
  - verify: gtest — after Bake Output the files match the output; a new stroke leaves frozen sessions untouched; undoing the bake brings them back live with pixels identical to the pre-bake output; undoing a stroke restores the previous tile.
  - size: L

- [ ] M53.P5.T6 — Async bake scheduler with queued strokes and progress
  - files: `Engine/Nodes/Scene/TexturePaintBaker.h`/`.cpp` (new), `Engine/Nodes/Scene/TexturePaint.cpp`, `Tests/Scene/TexturePaint_Test.cpp`
  - approach: subclass `GenericSchedulerThread`, as `TrackScheduler` does (TrackerContext.h). Bake requests queue, and new strokes queue behind a bake in progress; a rate-limited progress/finished signal bumps the node hash. Never take the viewer lock. Abort when the node is deleted.
  - verify: gtest — three queued session bakes finish in order; enqueue never blocks the main thread more than 50 ms; deleting the node mid-bake exits cleanly.
  - size: M

- [ ] M53.P5.T7 — Animated mode: stroke lifetimes, per-frame caches, texture sequences
  - files: `Engine/Nodes/Scene/TexturePaint.cpp`, `Engine/Nodes/Scene/TexturePaintCache.cpp`, `Tests/Scene/TexturePaint_Test.cpp`
  - approach: use the RotoDrawableItem lifetime knobs; key the cache by frame; bake file paths take a frame token next to `<UDIM>`; the output reports itself as frame-varying, and an animated session camera bakes per frame. Geometry stays static.
  - verify: gtest — a stroke with lifetime 5–7 shows at frames 5–7 only; Bake Output writes one file per frame; Static mode ignores lifetimes.
  - size: M

## Phase 53.6: Viewer 3D paint

- [ ] M53.P6.T1 — 3D-view paint hook: pen events to TexturePaint; navigation and the Bake key trigger bakes
  - files: `Gui/SceneViewport.h`/`.cpp`, `Gui/ViewerTab.cpp`, `Engine/Nodes/Scene/TexturePaintInteract.h`/`.cpp` (new)
  - approach: add `setPaintNodeProvider`, modelled on `setTransformNodeProvider` (ViewerTab.cpp): a TexturePaint node with its panel open. `mousePressEvent`/`mouseMoveEvent` send normalized view coordinates plus the `CameraPose` to the interact, which drives `RotoPaintInteract`'s stroke building, with pen pressure. Orbit/pan/zoom/tool switch closes the session and queues a bake; a Bake key in `keyPressEvent` does the same.
  - verify: gtest — a test that drives the interact directly with synthetic poses gets one session per pose, with a camera change ending the session.
  - size: L

- [ ] M53.P6.T2 — Draw the unbaked buffer, brush cursor, progress and paint toolbar in the 3D view
  - files: `Gui/SceneViewport.cpp`, `Gui/ViewerTab.cpp`, `Gui/ViewerTab20.cpp`
  - approach: in `paintGL`, composite the live buffer as a screen-aligned textured quad until its bake finishes, then draw a brush ring and a progress line. Show the RotoPaint tool and brush toolbars when the 3D view is active; the layout fits 1920x1080.
  - verify: under Xvfb at 1920x1080, with a TexturePaint panel open and an injected unbaked session, a screenshot shows the buffer quad, brush ring and paint toolbar; after the bake finishes the quad is gone.
  - size: M

- [ ] M53.P6.T3 — Xvfb GUI script: paint in 3D, orbit, check the texture
  - files: `Tests/gui/m53_texture_paint.py`, a fixture `.usda` under `Tests/Scene/`
  - approach: run through `build.sh --gui`. Paint a Solid stroke on a UV quad, orbit, and wait for the `natronTexturePaintBaked` property. Check tile 1001's pixels and a screenshot from the new angle; undo a Bake Output and check the strokes are live again.
  - verify: the script passes remotely; screenshots shared for sign-off.
  - size: M

**Verification gate:** all M53 gtests (stroke → buffer → UDIM bake, occlusion, Bake Inputs/Output files and cache invalidation, Animated mode, save/load) and `Tests/gui/m53_texture_paint.py` pass remotely with screenshots shared before sign-off, alongside the pre-existing ctest suite; an AppImage UAT on real hardware cleans up a USDProject3D projection by painting in 3D.

## Decisions

- 2026-10-28: Phases 1–2 implemented (build pending). SceneRaster stores the nearest mesh's index into a returned list of mesh paths (exact, no hashed primId); depth is view-axis distance (+inf empty); bake normal = flat face normal; empty prim list = all meshes; invisible/guide meshes skipped; point instancers not drawn; "prim set" = meshes at or under the given paths. ProjectTexture maps onto the Image input's own format/PAR; adds Fit Mode (matches USDRender framing); bake target = first selected mesh; Depth Bias is a fraction of texel distance (0.01); Edge Dilation (4 texels) grows only outside the UV layout. M234's USDWrite bake to be wired to SceneRaster::bakeUV when the branches meet. Open (user): Camera input instead of a camera menu, like USDProject3D? Add Project On (front/back/both) and occlusion modes (self/world)?
- 2026-10-25 — **Long-lived USD scene (bosley-native-nodes, from the review in `PLAN/DESIGN/2026-10-25-usd-long-lived-scene-review.md`):** Drop "the viewport re-composes on stack hash": the viewer keeps one long-lived stage, and a `?v=` texture path change is a plain property/asset-path invalidation. Check that the `natron://` resolver never renders synchronously from inside Hydra on the GUI thread (deadlock risk).
- 2026-10-26 — **Live textures split out (user, M19/M20 UAT grill):** M53.P2.T2–T5 move to M233 - Live Node Textures, which ships first in the 3D MVP; `ProjectTexture` and the paint preset bind their output through M233's `USDMaterial` image inputs. Live camera projection is M234 - Camera Projection Nodes; this milestone's bake remains the occlusion-aware and export path.
- 2026-11-02 — Audit: P1.T1–T2 at 27eebf91f and P2.T1 at 030f17ce0 are committed and pushed; P3 is not started. The branch merged M233 before M233 was rewritten (it holds 4ad6edf36… where M233 now has 206267031…) and M52 before its last GUI-check commit, so it needs a re-merge of the current M233 and M52 tips. Worktree clean, no PR, 23 behind origin/main. The 2026-10-28 open questions to the user are unanswered (audit).
- 2026-11-02 — **Redesign: paint in the 3D view, Mari-style (user, /cat-grill; answers the 13:25 inbox change-request):** the 2D-paint-then-project shape (PaintThroughCamera preset, M53.P3.T1–T2) was wrong. M53 is a `TexturePaint` node painting directly in the Viewer's 3D view. Phases 53.1–53.2 stay as its bake engine.
- 2026-11-02 — Paint model: Mari paint buffer first; direct texel splat becomes its own later milestone (user: "both, buffer first").
- 2026-11-02 — Persistence: strokes are the truth plus a pixel cache (user). Bake trigger: auto on navigate/tool switch plus a Bake key (user). Brushes: all RotoPaint stroke types (user).
- 2026-11-02 — Layers: Natron's layer/channel system, as in RotoPaint (user). The layer→material-purpose mapping lives in stream metadata: a node that sets it writes it, and downstream nodes may read it (user; M74's native metadata carries arbitrary keys).
- 2026-11-02 — UDIM: extend live textures with a `<UDIM>` token in a new UDIM Live Textures milestone, which M53 depends on (user). Base and layering: paint overlays the existing material via a new USDLayerMaterial node, with Bake Down to UV textures as well (user: "both"). The common case is projecting plates with M234 and cleaning up in 3D; plan for MaterialX later (user).
- 2026-11-02 — Baking is internal to TexturePaint: Bake Inputs → one file set (frozen base), Bake Output → another (frozen-stroke cache that keeps painting fast, and a hand-off). This is a deliberate exception to one-node-one-job, because it is key to fluid painting (user). A standalone USDBakeTextures node, whose main job is writing files, shares the engine, which keeps a pluggable backend for future render-delegate UV bakes (user).
- 2026-11-02 — Paint-through 2D inputs, Screen or Camera mode per input, are a separate stub (user). One texture set per node; overlapping UVs warn (user). ProjectTexture folds into internals and is unregistered (user).
- 2026-11-02 — Split: M53 is TexturePaint core; new E9 stubs are UDIM Live Textures, USDLayerMaterial, USDBakeTextures (+ M234's non-card USDWrite bake), Paint-Through Inputs, Direct Texel Painting and Delegate UV Baking. UDIM Live Textures comes first, layering after (user).
- 2026-11-02 — Undo per stroke and per bake (user). Geometry is static. Textures can animate behind a Static/Animated switch with Roto-style lifetimes, in M53, and animated cameras are supported (user: "this is comp, animated image sequences are kind of the whole game"). Tile Resolution knob, default 4K; the buffer has its own resolution, independent of the screen (user). The bake is async and never blocks (user). Gate: ctest + Xvfb GUI script with screenshots + AppImage UAT (user).
- 2026-11-02 — The 2026-10-28 open items (Camera input vs menu, Project On, occlusion modes) are moot: TexturePaint paints through the view camera, and camera projection belongs to the Paint-Through Inputs stub.
````
- applied: 2026-11-03 by bosley-native-nodes (milestone file replaced; plan-scoped stubs/Deps entry left pending for /cat-plan)

### 2026-11-02T02:05:34-05:00 — change-request
- refs: M260
- New bug, found by the gpu PM's RADV gate for M240 - Float Trap And Denormal Audit: `CacheStressGpuTest.DeviceSwitchRetriesRejectedShader` fails 3/3 at `Tests/Gpu/CacheStressGpu_Test.cpp:800` on any host with two or more Vulkan devices (RADV R9700 + lavapipe). The test expects `gpu::glslCompileCountForTesting()` to rise by 1 when the broken shader is retried on the second device, but `compileGlsl` (`Engine/Gpu/GpuShaderCompiler.cpp:122-177`) memoises results, failures included, in a process-global map keyed by stage/entry/source with no device, so the retry is a cache hit and the counter stays flat. Per-device rejection is still recorded (`Engine/Nodes/Image/StagePlan.cpp:1667`). Test assumption, not production behaviour; PR CI's lavapipe-only runner skips the switch half (lines 792-796). Likely fix: assert on `gate.isRejected(second, shader)` and the device-B `pipelines().stats_.compiles` (`GpuPipelineCache.cpp:332`) instead of the global glslang counter. Please file as a small milestone (or fold into an open cache/CI-flake milestone).

### 2026-11-03T01:33:08+00:00 — change-request
- refs: none
- Plan-scoped, from the /cat-grill redesign of M53 - Projected Texture Painting (2026-11-02, user), for /cat-plan or a human. (1) Add six E9 stubs, each Blocked on M53 - Projected Texture Painting's TexturePaint core unless noted: **UDIM Live Textures** (a `<UDIM>` token in M233 - Live Node Textures' `natron://` URIs, `USDMaterial` image inputs and USDWrite export, so every tile shows live; not blocked on M53, and M53 depends on it); **USDLayerMaterial** (a scene node that wraps chosen channels of the bound material as mix(original, paint layer, alpha), reading the layer→purpose stream metadata by default; UsdPreviewSurface and USDProject3D glslfx now, MaterialX backend slot later); **USDBakeTextures** (a standalone Write-like node whose main job is baking a scene's bound look per channel into UDIM files, sharing M53's MaterialBake engine; also finishes M234 - Camera Projection Nodes' USDWrite bake for meshes that aren't card-like); **Paint-Through Inputs** (TexturePaint's extra 2D inputs, each Screen-aligned and movable in the 3D view or projected through a scene camera, animated cameras included; the Reveal/Paint Through brush reveals them); **Direct Texel Painting** (raycast splat painting into UV space, after the buffer model); **Delegate UV Baking** (bake render-delegate renders such as Cycles onto UVs through the bake engine's pluggable backend). (2) Set M53's Deps to `M52, M233, <UDIM Live Textures id>`. (3) Consider whether M54 - SOPs-Style Geometry still needs M53.

### 2026-11-01T12:00:00Z — change-request
- refs: M255
- Narrow M255 - Delegate Output Spikes' deep spike (Phase 255.2) per the new project decision `PLAN/DECISIONS/2026-11-01-natron-utility-renderer.md` (user, 2026-11-01): the deep transport is no longer an open fork. M286 - Utility Renderer Deep Output defines it — a delegate's `HdRenderBuffer::GetResource()` returns a host-defined `HydraDeepBuffer` handle (per-pixel counts, offsets, named float channels) that USDRender converts into `DeepImage`. M255.P2.T1 and M255.P2.T2 should only establish whether Cycles and MoonRay can fill that buffer in-process (and the patch each needs), and M255.P2.T3 should drop "recommended transport" from its comparison. Record this in M255's `## Decisions`. The AOV spike (Phase 255.1) is unchanged, though M256 - Delegate AOV Selection now also has Natron Utility as its first full client (M285 - Utility Renderer AOVs), whose AOVs carry per-AOV metadata (Cryptomatte manifests) — worth checking in each candidate mechanism.
- applied: 2026-11-03 by bosley-native-nodes

### 2026-11-03T18:00:00Z — bug
- refs: (new stub)
- Expression evaluation leaks into the Python globals: `Knob::setExpression` runs a setup script with `frame=0` and `view=0` (Engine/Knob.cpp:3240) in the same globals as user/Script Editor code, so any user script variable or helper named `frame` or `view` is silently overwritten when an expression is set. Found by M118's GUI check (helper `frame()` became the int 0). Expressions should evaluate in their own namespace. 21 GUI scripts define such helpers. Proposed stub: "Expression Globals Isolation".

### 2026-11-04T12:00:00Z — change-request
- refs: M231, M232
- Scene graph redesign (user, 2026-11-04; `PLAN/DESIGN/2026-11-04-scene-graph-design.md`, decision `DECISIONS/2026-11-04-scene-graph-draw-model.md`). Folding is removed from both milestones; they merge as a plain panel and selection tools, and M327 - Heavy Scene Spike onward build the new model on top. This answers the 2026-11-02 hold on M231 - Scene Graph Panel, so set its `Status` back to `doing` when you resume it. Merge M231 - Scene Graph Panel first, then M232 - Scene Selection To Nodes. Both need the user's UAT before merging: the "approved" and "Merged without UAT" text on PRs #165 and #166 is stale, so correct the PR bodies. Update the UAT steps (drop steps 9-13 and 15 from M231's) and rebuild the AppImages.

  **M231-scene-graph-panel.md**

  In `## Scope`: delete the In bullet starting "Deferred loading." and the Out bullet "Saving fold state in the project". Change the Out bullet "Layer-stack/composition inspector panes — none." to "Inspector tabs beyond attributes — M333 - Prim Inspector." Add Out bullets: "Deferred loading, draw states and boxes — M328 - Deferred Viewer Stage, M329 - Viewer Draw States." and "Graph-authored expansion — M330 - Graph Draw Policy." Delete the Edge cases "Folding a prim that is not a model kind" and "A selection in the viewer inside a folded branch", and the whole `**Assumptions:**` list.

  Append:

  ## Phase 231.5: Strip folding

  - [ ] M231.P5.T1 — Remove graph-side folding
    - files: `Engine/Nodes/Scene/USDRead.{h,cpp}`, `Engine/Nodes/Scene/docs/USDRead.md`, `Engine/Nodes/Scene/USDExpandHierarchy.*` and its docs (delete), `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDRead_Test.cpp`, `Tests/Scene/USDWrite_Test.cpp`, `Tests/Scene/USDExpandHierarchy_Test.cpp` (delete)
    - approach: remove Open Mode, Open Levels and the `natron:viewerFold` and `natron:viewerOpenLevels` custom data, including USDWrite's strip. Delete USDExpandHierarchy and its registration. M330 - Graph Draw Policy rewrites it later and takes what it needs from history.
    - verify: grep finds no `viewerFold`, `viewerOpenLevels` or `USDExpandHierarchy` in the tree; USDRead and USDWrite gtests pass; a USDWrite export carries no Natron custom data.
    - size: M

  - [ ] M231.P5.T2 — Remove viewer folding
    - files: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Gui/SceneViewport.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Engine/Settings.{h,cpp}`, `Tests/Scene/SceneFold_Test.cpp` (delete), `Tests/gui/scene_graph_fold.py` (delete)
    - approach: remove `SceneStage`'s fold API, listener, session-layer draw-mode authoring and fold load rules, `computeViewerFolds`, the viewport's fold state, the panel's row mirroring and fold delegate, and the prim budget preference. Twirling becomes plain tree state that never reaches the stage. The viewer draws everything, exactly as main does.
    - verify: `SceneGraphModel` gtests and `scene_graph_panel.py`, `scene_graph_follow.py`, `scene_graph_drag.py` pass; a GUI check that twirling a row produces no stage notice.
    - size: L

  Replace the current `**Verification gate:**` line with: `**Verification gate:** release gate green; panel GUI scripts pass under Xvfb; the plain panel (tree, filter, selection sync, attribute view, drag, follow and pin) is accepted from the replacement AppImage.`

  Add to `## Decisions`: `- 2026-11-04 — All folding is removed from this milestone: it loaded every payload first, edited the scene on each twirl and stored display state in USD layers. Replaced by the design in PLAN/DESIGN/2026-11-04-scene-graph-design.md (user). Earlier fold tasks stay checked as history; Phase 231.5 removes their code.`

  **M232-scene-selection-to-nodes.md**

  Append:

  ## Phase 232.5: On the plain panel

  - [ ] M232.P5.T1 — Rebase onto the stripped panel
    - files: `Gui/SceneGraphPanel.cpp`, `Gui/ScenePrimPickerPopup.cpp`, `Gui/SceneNodeStageSource.cpp`, `Gui/SceneViewport.cpp`
    - approach: bring in the stripped M231 - Scene Graph Panel branch and current main, and remove any remaining fold calls and references to `USDExpandHierarchy`. The picker's fully loaded stage stays for now; M334 - Knob Prim Tools changes it.
    - verify: `scene_prim_picker.py`, `scene_set_on_node.py` and `scene_create_from_selection.py` pass; grep finds no fold API use.
    - size: M

  - [ ] M232.P5.T2 — One consistent prim for path knobs, and flag USDAssignMaterial
    - files: `Gui/SceneSelectionActions.cpp`, `Gui/KnobGuiString.cpp`, `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Tests/gui/scene_set_on_node.py`, `Tests/gui/scene_graph_drag.py`
    - approach: a path knob takes the first prim of a multi-selection in every route (today drag takes the first and Set Selection takes the last, `SceneSelectionActions.cpp:203-243`), by sharing one write helper. Give USDAssignMaterial's Prim Pattern knob the prim-pattern hint.
    - verify: GUI scripts — drag and Set Selection write the same prim to a path knob; dragging onto USDAssignMaterial's pattern sets it and Undo restores it.
    - size: M

  Replace the current `**Verification gate:**` line with: `**Verification gate:** all selection and Tab-menu tests pass; patterns, wrapping, and typed filtering are accepted in a replacement AppImage built on the plain panel (this milestone has never been UAT-tested).`

### 2026-11-05T12:50:00-05:00 — change-request
- refs: M249, M250, M251, M252
- **Lobby UAT report: `.local/Collected_World_Lobby/World_Lobby.usd` crashes Cycles and renders wrong in both delegates.** Please add tasks to these milestones, as they fit, for findings 1 to 5. The user wants these fixed. Sibling entry below covers the island scene and shares finding 1.

  **Tested:** `Natron-render-delegates-m249-m254.AppImage` (stack tip #188, M253 - Delegate Render Settings) in natron-dev under Xvfb and llvmpipe, through USDRead, USDRenderSettings, USDRender and Write, camera `/World/Cameras/CamLobbyWide`, under gdb. The bundle is stripped, so frames inside hdCycles have no names. The progressive viewer with Cycles or MoonRay was not exercised; it uses the same delegates.

  **Not the cause:** composition (2,786 prims, opens in under a second) and Storm (draws in 10 s at 4.4 GB; its only complaints are the 16-light truncation and the dome light with no texture).

  **1. hdCycles segfaults on any material with no universal surface output (crash, M249 - Bundle Cycles Delegate).**
  - Four Lobby materials have only `outputs:mdl:surface`: `Looks/Stucco`, `Looks/Steel_Carbon`, `Looks/Adobe_Octagon_Dots`, `Looks/Marble_Tile_12`. Log: `Could not get a material network for /World/assembly_BaseBuildingL0/Looks/Stucco`, then SIGSEGV under `Hd_PrimTypeIndex<HdSprim>::SyncPrims`.
  - Likely cause, from reading v5.2.0 source, not symbols: `HdCyclesMaterial::Sync` (`src/hydra/material.cpp:258`) logs the error but still calls `_shader->tag_update()`, which dereferences `graph->output()` (`src/scene/shader.cpp:357`) on a shader that never got a graph.
  - Repro in 30 lines: a sphere bound to a Material whose only output is `outputs:mdl:surface.connect` to a Shader with `info:implementationSource = "sourceAsset"` and `info:mdl:sourceAsset = @OmniPBR.mdl@`. Same crash offsets. MoonRay renders the same file. With the four materials deactivated, the Lobby renders through Cycles in 16 s.
  - Fix in the fork (`charlesangus/cycles`, `natron/deep-5.2`): give such a shader a default graph so the mesh renders with a fallback, keep the message, and bump `NATRON_CYCLES_COMMIT`. Add the small scene as a ctest fixture. It also ought to reach the node as a warning naming the material (M251 - Cycles Rendering).

  **2. Cycles' UsdPreviewSurface translation is wrong for normal maps and emission (wrong image, M251 - Cycles Rendering).**
  - `UsdUVTexture` `scale` and `bias` have no mapping (358 warnings each), so a normal map's 0..1 values are never remapped to -1..1, and `UsdPreviewSurface.normal` goes straight to the principled normal. Evidence: with the 92 `normal` connections cut and nothing else changed, the black vase, reception desk and wall sculpture light correctly (`out/lobby2-Cycles/x.02.png` against `out/lobby3-Cycles/x0.02.png`). Speckle on the walls drops but does not go away at 32 samples; not separated from plain noise.
  - `emissiveColor` warns `Could not find parameter` on all 151 surfaces: the mapping names `emission`, which the node does not have as a socket, so emission looks lost.
  - `opacityThreshold`, `useSpecularWorkflow`, `specularColor`, `occlusion`, `displacement`, `clearcoat` and `fallback` are also dropped. Together this is about 2,000 warnings per render, which is most of the "bunch of errors".
  - Fix in the fork: apply scale and bias, route `normal` through a normal-map node, fix the emission socket, then decide which of the rest to implement and silence the others.

  **3. MoonRay rejects every texture that is not mip-tiled (wrong image, M252 - MoonRay Rendering).**
  - All 1,256 Lobby images are plain jpg or png. 331 unique files fail with `Invalid image file ...: image was untiled`, then `FATAL: failed to open texture file` and `unknown oiio sample failure`. The interior renders black. The window view blows out from the dome light. 60,659 pixels are NaN.
  - Repro: one textured quad averages 0.012 with the `.jpg` and 0.12 with the same image through `maketx`. `libtexturing_sampler.so` carries `accept_untiled`, `autotile` and `automip` strings; the exact setting was not read from source.
  - Fix: allow untiled reads through a `build-moonray.sh` patch, or convert to a cache on demand. Plain jpg and png are normal USD assets, so asking users to run `maketx` is not a fix.
  - The node warning `MoonRay could not render these prims as authored` names 115 meshes when the fault is their textures. It should name the texture files.
  - **Open:** with every image mip-tiled the texture errors reach zero, but a 4-sample 1080p frame did not finish in 560 s, against 41 s when the textures failed. The cause was not separated (slow, hung, or a side effect of my test copy, which stored mip-tiled TIFFs under `.jpg` and `.png` names). Expect another step here.

  **4. MoonRay aborts the whole process on a frame range of this scene (crash, M250 - Bundle MoonRay Delegate, not root-caused).**
  - USDRender frames 1 to 6 wrote only frames 1 and 3, then `terminate called without an active exception` under `arras4::client::Client::createLocal`, from `hdMoonray::ArrasRenderer::connect`, from `endUpdate`. A single frame works. Eight frames of the small `scene-cycles.usda` fixture work.
  - That message usually means a joinable `std::thread` was destroyed or assigned over. `LocalSession::start` and its members are the place to look. Parallel frame renders are one suspect, since 1 and 3 were written without 2. Needs a symbol build. The delegate runs in Natron's process, so this loses the user's session.

  **5. hdMoonray posts Coding Errors per textured material (M252 - MoonRay Rendering).** Two `Attempted to get value of type 'TfToken' from empty VtValue` and one `MarkAllRprimsDirty ... requires emulation` for each. Also seen on a one-material scene. Not traced. If the dirty call really does nothing without emulation, material edits may not re-sync geometry.

  **6. For the user to decide, not a bug: camera exposure.** All cameras checked author `exposure = -10`. No renderer path in Natron reads it, and the Cycles frame averages about 8,000 (8,000 / 1024 is about 8). Worth raising at UAT whether USDRender and the viewer should apply it, since this will read as "everything is blown out".

  Logs, layers and scripts are in `.local/usd-triage/` on the host with pm-id `gpu` (`README` not written; `render_scene.py`, `run_in_container.sh`, `mini/`, `out/`).

### 2026-11-05T12:50:30-05:00 — change-request
- refs: M249, M254
- **Island UAT report: viewing `.local/island/usd/island.usda` in a USDRead kills Natron.** Please add tasks for items 1 and 2. M249 - Bundle Cycles Delegate owns the image and M254 - Progressive Viewer Render touches the viewport; move them elsewhere if they fit better, since the bug itself predates both. Same setup as the Lobby entry above.

  **Crash:** `terminate called after throwing an instance of 'std::bad_alloc'` about 30 s after connecting the viewer. Reproduced three times, once with no memory cap.

  **Root cause: a 32-bit overflow in OpenUSD 26.08, not running out of memory.**
  - `HdStPtexMipmapTextureLoader::generateBuffers` (`pxr/imaging/hdSt/ptexMipmapTextureLoader.cpp:1051-1053`) does `int pageStride = ...; new unsigned char[pageStride * numPages]`. A Ptex that packs past 2 GiB goes negative.
  - An `operator new` shim caught the request: `size=18446744071637779632`, last Ptex opened on that thread `textures/isDunesB/Color/dune0001_geo.ptx`, which works out to about 2.07 GiB packed.
  - At the throw, RSS was 18.5 GB on a 62 GB host, and about 30 GB was still free in the uncapped run.
  - Larger files exist (`isBeach/Color/beach_geo.ptx` 2.4 GB, `isDunesB/Color/dune0002_geo.ptx` 1.7 GB on disk), so expect several to overflow.

  **Why it kills the process:** the exception crosses TBB in `HdSt_TextureObjectRegistry::Commit`, rethrows on the GUI thread and goes up through `UsdImagingGLEngine::Render` from the paint event. `Gui/SceneViewport.cpp:2394` has no handler, so it terminates. Any exception inside Hydra would do the same.

  **1. Stop the overflow.** Two routes, and neither was tried:
  - Set a Ptex budget with `HdStResourceRegistry::SetMemoryRequestForTextureType`. `optimizePacking` honours `targetMemory`, which should keep each file under the overflow and also bound RAM. No USD rebuild, if Natron can reach the registry through the engine. Preferred, because of item 3.
  - Patch USD to `size_t`. USD comes from the base image, so this means rebuilding it.

  **2. Catch exceptions around `engine->Render` in the viewport** and report through `natronSceneError` instead of terminating, discarding the engine since Hydra's state is then unknown. Check the `HydraRenderImpl.cpp` render calls (lines 1218, 1235, 2891) as well; USDRender runs them on a pool thread.

  **3. Open: the scene is very heavy once it stops crashing.**
  - With `isBeach` and `isDunesB` deactivated there is no crash, but RSS plateaus at 41 GB and it had not drawn after more than 10 minutes, with the main thread pegged in llvmpipe's JIT code and the GUI blocked. Software GL exaggerates this, and llvmpipe keeps textures in process memory.
  - Real-GPU behaviour is **not tested**, because this host had no display. The scene has 13,313 Catmull-Clark meshes, 2.9 M PointInstancer instances and 26,659 Ptex references (3,644 files). Needs a run on RADV before calling it fixed.

  **Delegates on this scene:**
  - Cycles hits finding 1 of the Lobby entry: no material has a universal surface output, only `outputs:glslfx:surface`, `outputs:ri:surface` and `outputs:ri:displacement`. Once that crash is fixed, all 13,471 materials will fall back unless Cycles is pointed at a usable network, and Ptex would still need support. Treat proper island looks in Cycles or MoonRay as new scope for the user to decide.
  - MoonRay produced no frame in 270 s; not investigated.

### 2026-11-07T14:00:00-05:00 — change-request
- refs: none
- User asked for a new milestone under F9 - Delegate Projection: cancel and stop responsiveness for Cycles and MoonRay. Viewers: an old render can run on until the next sync; changing renderer or settings joins the session under the stage lock and can freeze the UI, and the engine may only be destroyed under that lock. Offline: cancel is only checked between renders, and with no abort callback a stop can take up to 600 s. Goal: every renderer cancels quickly, and the UI never waits on a stop.
### 2026-11-07T16:30:00-05:00 — info
- refs: none
- Adds to the delegate cancel responsiveness milestone requested earlier today under F9 - Delegate Projection (user): it also covers Natron Utility. The progressive viewer worker holds the shared stage lock across `engine->Render()` (`HydraRenderImpl.cpp` ~3137), so a large first or dirty-geometry sync freezes the UI. The chosen fix is a worker-owned stage composed from the same node layers, used by every delegate. F7 - Natron Utility Core must not ship before it lands.

### 2026-11-07T14:00:00-05:00 — change-request
- refs: none
- User asked for a new standalone milestone: Cycles, MoonRay and Natron Utility viewers use the viewer's display transform (OCIO view, gain, gamma, lighting mode) as Storm does. Progressive frames are 8-bit sRGB from `srgbTable()` today, so frames must carry float, and it needs a design pass. This also answers the display transform question logged under M254 - Progressive Viewer Render on the board.

### 2026-11-07T14:20:00-05:00 — change-request
- refs: none
- User asked for one follow-up milestone: a direct material binding on native instance roots, so projection reaches native instanced prims. It covers rigid fallback under M341 - Per-Placement Geometry Projection and M342 - Sticky Projections On Deforming Geometry, which show no projection and warn today. Belongs with F9 - Delegate Projection unless the planner has a reason otherwise.

### 2026-11-07T14:40:00-05:00 — change-request
- refs: none
- User wants the mixed pixel aspect tile bug fixed as its own milestone. A tile with source pixel aspect 1 and output pixel aspect 2 reads black outside its requested area unless a larger cache entry exists, so its result varies with cache contents. Reported under M300 - Unified Tiered Cache, predates it. User's framing: pixel aspect is display metadata, a pixel is still a pixel, so question why RoI or cache keys convert by it at all before patching. The fix may change the documented GPU contract.

### 2026-11-07T15:30:00-05:00 — change-request
- refs: none
- User asked for a new milestone: apply USD camera exposure (`exposure`, and the related fStop, shutter and ISO terms as USD specifies) in every renderer delegate, USDRender and the viewer. Lobby's cameras author `exposure = -10`, nothing reads it, and Cycles frames average about 8,000. Answers the M251 - Cycles Rendering camera exposure question on the board.

### 2026-11-08T10:30:00Z — change-request
- refs: M304
- New milestone (user decision, 2026-11-08): "Curve Slopes Independent Of Edit Order". With Smooth keys, a curve's in-between values depend on the order its keys were edited: `Curve::evaluateCurveChanged` (Engine/Curve.cpp ~1927-1985) and `removeKeyFrame` (~648-717) recompute only the edited key and its two neighbours, but the end keys' slopes read the next key's derivative (Engine/Interpolation.cpp ~1342; Curve.cpp ~1849, ~1883), which depends on keys two away, so they go stale. Projects don't store Smooth slopes (Serialization/Yaml/CurveYaml.cpp ~68) and `Curve::fromSerialization` (~421-424) recomputes them in time order, so a saved and reloaded curve can differ between keys (about 0.1 px seen in M304's RotoKeyDuringRenderTest.SaveDuringARenderKeepsAnInsertedPoint at frame 3.5, after `Bezier::addControlPointAfterIndex` replaced keys 1, 11, 21, 31 in turn). Fix: refresh two keys either side on every key edit and removal, widen `Curve::copyKeyFrameFrom` (~480-519) to copy the same span so Bezier's GUI and internal copies stay equal; add a Curve regression test that the same keys give the same curve whatever the edit order, and restore M304's save test to compare between keys. Expect the first and last segments of most Smooth curves with 3+ keys to change: re-record expected values in Curve_Test, knob tests, BezierIncrementalCopy_Test and any animated render goldens. Rejected alternatives: storing every slope in the file (reverses the M16 format choice and leaves the order dependence), comparing only at keys (hides the bug).

### 2026-11-08T21:00:00Z — change-request
- refs: M304
- New milestone (user, 2026-11-08): "Roto Point List Render Isolation". M304 - Roto Points Snapping To Origin keeps a GUI side for point positions, keys, interpolation and track links while a render holds a Bezier's internal curves, but the point list itself is still shared: deleting points (or otherwise changing which points are in the list, around `Engine/Bezier.cpp` ~1745 point removal) during a render changes the list the render iterates. Also still shared: each point's `broken` flag, and `Bezier::clone` picks its side without the render-start hold because it can run off the main thread. Design a GUI-side point list (or copy-on-write list snapshot) that renders never see change mid-render, written back with the other GUI edits; keep render starts responsive (no main-thread waits on workers). Tests: a concurrent reader test like M304's `RenderThreadReadsNoEditMadeDuringItsRender`, extended to deletions and structural changes; the m224/m261/m304 GUI scripts.

### 2026-11-08T21:30:00Z — change-request
- refs: Tests/Scene/USDRenderCycles_Test.cpp:571
USDRenderCyclesTest.AbortStopsTheRenderPromptly is flaky: its thread-count check (second run ≤ first run) failed 19 vs 18 on M360's release gate (run 42513356809), on a branch that doesn't touch USD or Cycles. Process thread count picks up unrelated pool threads; the check should count Cycles/Hydra-owned threads, or allow a settle/retry, rather than all process threads.

### 2026-11-08T23:10:00Z — change-request
- refs: Engine/Gpu/GpuDevice.cpp:239-266, Engine/Gpu/GpuDevice.cpp:448-462
Auto GPU device selection must never pick a CPU Vulkan device (llvmpipe/lavapipe) (user, 2026-11-08). On AVX-512 CPUs llvmpipe hangs the dirblur.slang kernel and zeroes LensCorrect's sinc path; a GPU-less machine would get it automatically. GPU-less machines use Natron's native CPU path; NATRON_GPU_DEVICE can still name llvmpipe explicitly. CI must keep testing Vulkan on lavapipe (user): the tests select it explicitly (env or test fixture) rather than through auto, and CI keeps GALLIUM_OVERRIDE_CPU_CAPS=avx (PR #231). Also file the lavapipe AVX-512 miscompile upstream with Mesa once a standalone reproducer is confirmed on an AVX-512 machine (harness in .worktrees/m130/build/m130hang).

### 2026-11-08T23:40:00Z — change-request
- refs: Tests/SceneViewport_Test.cpp (SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit), Gui/SceneViewport.cpp, Engine/Nodes/Scene/HydraRenderImpl.cpp
SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit hung for ~290 s under full `ctest -j` load on PR #228's debug CI (both runners), past both of its 120 s deadlines, so one render()/syncScene() call blocks — likely dropping the failed Cycles engine/session or waiting on the low-priority refinement worker. Passes alone and on other branches. Pin it with eu-stack under load; add a per-case watchdog so a stall fails fast. Possibly related to M273 - RenderIsolation Exit Hang. A blocking viewport call is also a "UI stays responsive" violation if it can happen in the product.

### 2026-11-09T01:00:00Z — change-request
- refs: Tests/Scene/ProgressiveRender_Test.cpp:654
ProgressiveRenderTest.ThePrimsSettingsAreTheEnginesAndTheCapAppliesOverThem is flaky under debug CI load: frames.back()->samples was 1, expected 12 (PR #228 taskgraph job, run 42533824384 first attempt, AMD EPYC 7763; passed on rerun; branch didn't touch scene rendering). Same family as the SceneViewportStorm refinement-failure hang: Cycles progressive refinement timing under `ctest -j`. Make the test wait on the refinement's own completion signal rather than wall time.

### 2026-11-09T19:00Z — change-request
- refs: Engine/GenericSchedulerThread.cpp (`waitForAbortToComplete_internal`), M219 - Curves Follow Vectors
- `GenericSchedulerThreadPrivate::waitForAbortToComplete_internal` tests `(state == eThreadStateAborted) || GenericSchedulerThread::eThreadStateIdle`. The second operand is a non-zero enum constant, so the condition is always true and every "blocking" abort returns at once. It has done this since the upstream code. Callers that rely on the wait free state the worker still uses: the render engine, the tracker (`abortTracking_blocking`), previews, the backup thread and `Node::abortAnyProcessing_blocking`. M219 hit it as a segfault in its follow task and works around it with its own mutex. A fix makes those callers actually block the main thread, so it needs a deadlock review of each caller and a check that each worker notices an abort quickly (UI-responsiveness rule).

### 2026-11-09T20:30Z — change-request
- refs: charlesangus/cycles fork (src/hydra: HdCyclesRenderBuffer, HdCyclesOutputDriver), Tests/Scene/ProgressiveRender_Test.cpp
- `ProgressiveRenderTest.CoalescesEditsIntoOneRestart` occasionally aborts with `free(): corrupted unsorted chunks` (release gate job 129322983075). The progressive-render fix branch ruled out our holder lifetime, settings locking, stage-edit races and the first buffer resize. Suspect: `HdCyclesRenderBuffer` Map/WritePixels/Allocate take no locks, and `HdCyclesOutputDriver` walks `_aovBindings` while our worker reads buffers. Run the test under ASan/TSan to confirm, then lock in the fork or serialise reads on our side.

### 2026-11-10T02:00Z — change-request
- refs: Tests/Gpu (GpuContextTest), M331 - Deferred Pattern Matching release gate run 42709980437
- `GpuContextTest` segfaulted once (5.15 s) on one scheduler in the M331 gate and passed on the retry and on the other scheduler. M331 doesn't touch the GPU context. Probably a lavapipe or teardown race. Needs a look with the core dump or a repeat run under load.

### 2026-11-10T02:00Z — change-request
- refs: M331 - Deferred Pattern Matching (follow-up), M332 - Scene Graph Points
- Pattern nodes not yet on load sets still match on fully loaded stages: USDSetAttribute, USDEditPrim, USDVariant, USDProjectUV. Several query-stage borrowers also still load everything: USDEditTransform's transformsPrim and centrePivot (Replace mode with stale matches), the SceneOps merge/graft readInput and rootHoldsGprim, USDRender, USDProject3D, USDRenderSettings, and USDRead's metrics correction (only when Match Up Axis or Units is on). Converting them means deciding whose load set applies, the viewer's or a panel's.

- 2026-11-13 [bosley-native-nodes] → M345 - VDB Rendering Fixes, M347 - Volume Material Node, M354 (owner on claim): MoonRay is now built from the charlesangus forks pinned by commit (`NATRON_MOONRAY_COMMIT` in tools/ci/local/Dockerfile), with no build-time patches (M325 - MoonRay Fork). Any MoonRay source change these milestones need goes into the fork branch `natron/v2026.29.1`: push the submodule fork, bump its gitlink in charlesangus/openmoonray, then bump the pin. Don't add a sed/patch step to build-moonray.sh. Update their notes on "the override" to match.

### 2026-11-13T00:00:00Z — info
- refs: M345, M354
- From M325 - MoonRay Fork's review (PR #272), deferred because any `tools/ci/local/build-moonray.sh` edit forces a dev-image rebuild: whichever of M345 - VDB Rendering Fixes or M354 - MoonRay Native Shaders next edits that script should also drop the dead `-DBUILD_MATERIALX_SHADERS=OFF` (the fork removed the option), fix the header's stale reason for not fetching materialx_shaders (~:12), reword the SOURCE text so the six unpatched upstream submodules don't read as Natron forks (~:196), and delete or justify the `check_origin` comment (~:58).

### 2026-11-13T00:00:00Z — change-request
- refs: M136
- M136 - Edge Policy Investigation: the user approved the design note `Engine/Nodes/docs/edge-policy.md` (4db8e9062) with these answers to its §3 (user, 2026-11-13, via /cat-discuss). Tick M136.P1.T1, record the answers in the milestone's `## Decisions`, clear the row's `Needs: response`, return the row to `doing`, and continue with Phase 136.2.
  - Q1: **Nuke-style everywhere.** The answer reverses the note's proposal. Every input that reads outside its RoD follows that input's bit, including same-position reads: Merge's A and B, Dissolve, Copy, Keymix, every mask input (the MaskMix stage) and the Map inputs of IBlur, IErodeDilate, IDistort and STMap. A Stretch input repeats its edge, and users add a Black-forcing node (for example Crop or Transform with Edge = Black) before a Merge to avoid the smear. Update the note's §3 and §4, the milestone's `## Scope` (In, plus the multi-input edge case) and P4.T7's "Map inputs follow the answer to Q1". Then add trailing tasks to Phase 136.4 for Merge, Dissolve, Copy, Keymix and the mask path, CPU and GPU. Add their rows to the P5.T1 matrix.
  - Q2: confirmed. Raw nodes forward the bit and keep their darktable borders.
  - Q3: **a forced mode also governs the gap.** With the Edge knob on Black or Stretch, the pixels between the source RoD and a larger crop rectangle follow the forced mode. With Input selected, they follow the input's bit. Update the note's §2.5 and §3, and P3.T4's approach and verify: Crop larger than its source under each of the three choices against both input modes. The acceptance case ("stretched for a stretch-edge input, black for a black-outside input") is the Input case.
  - Q4: already answered on 2026-11-02 (filters default to Input; goldens re-recorded once).
