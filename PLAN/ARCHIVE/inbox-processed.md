

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
