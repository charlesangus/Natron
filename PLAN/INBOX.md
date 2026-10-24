## Pending

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
