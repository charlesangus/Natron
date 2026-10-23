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

### 2026-10-23T22:18:43Z — change-request
- refs: M16
- **User decision (2026-10-23): drop `gui.yaml` entirely.** The bundle becomes `mimetype`, `manifest.yaml`, `project.yaml` and `blobs/**`. This replaces M16 - Project Format Redesign's 2026-10-10 "Member split" decision and P3.T2's headless `gui.yaml` pass-through. Record it in M16's `## Decisions` and add revision tasks (Phases 16.1–16.5 are checked off, so these are new tasks, not edits to done ones).

  Rationale (user): splitting the files gives no clean-diff benefit, because the graph is unreadable without the positions, so any useful diff spans both files and carries the position churn anyway. Later work such as the E12 graph diff has to handle positions and topology together; the two can't be separated. The rest of `gui.yaml` is either app state or transient.

  Where each `ProjectGuiSerialization` field goes (`Serialization/ProjectGuiSerialization.h`):
  - **Node position, size, tile colour, user colour, overlay colour, preview flag** → per-node data in `project.yaml`, owned by the Engine `Node`, with `NodeGui` mirroring it. Today `Node::setPosition` drops the value and `getPosition` returns (0, 0) with no GUI (`Engine/Node.cpp:8261-8283`), so headless scripts can't place nodes. Engine ownership fixes that, and position now survives renames, because `gui.yaml` was keyed by fully-qualified name. Group children carry theirs through the `children:` nesting.
  - **Backdrops** → they are nodes, so they follow the row above.
  - **Not saved (app-level layout):** pane layout, splitters, floating windows, toolbar visibility, Python panels. These belong to the app's layout manager (the `.nl` save/load and the default-layout preference). Opening a project never changes the layout, and a fresh boot gets the default layout. Keep P2.T4's layout encoder for `.nl` (P4.T3); only the project-GUI encoder goes.
  - **Not saved (transient):** viewer zoom/pan and `zoomOrPanSinceLastFit`, node selection, open histograms, open settings panels and their minimized/hide-unmodified flags, script editor input.
  - **Viewer state** (gain, gamma, display/view/look, channels, layers, inputs A/B, wipe operator, fps, frame bounds, user RoI, and so on) → **not saved by M16 - Project Format Redesign.** It will become knobs on the Viewer node in separate work, filed separately and outside this milestone's scope. Until then it is unsaved, which is accepted.
  - **Loading several viewers with no saved layout:** each Viewer node opens as a tab in the default viewer pane.

  Expected revision tasks:
  - node schema and encoder: add the layout fields (P2.T3's `ProjectYaml`)
  - remove `ProjectGuiYaml`'s project entry point and its test cases
  - bundle member list without `gui.yaml` (P3.T1)
  - save/load paths without `ProjectGuiSerialization`, and no headless pass-through (P3.T2)
  - Engine-side position storage on `Node`
  - clipboard positions taken from node data (P4.T2)
  - viewer placement on load
  - remove the code that restores opened panels, histograms and selection on load
  - update or delete tests that assert on `gui.yaml`
  - regenerate the fixtures and bench comps
  - P7.T2 docs without the `gui.yaml` schema
  - P7.T3 UAT step 3 becomes "Move a node: only that node's position lines in `project.yaml` change. Change a Blur size: one line changes." Step 2's `unzip -l` member list has no `gui.yaml`.
