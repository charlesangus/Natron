# M168 - NVIDIA Viewer Hand-off Validation

Validate M107 - GPU Viewer Hand-off on NVIDIA GL, on the RTX 3070 Ti host `bosley-faux`. This is M91 - NVIDIA GPU Validation's Phase 91.3, moved here because M107 had not merged when M91's Phase 91.2 finished. The tasks are M91.P3.T0–T2 verbatim (new IDs); M91's milestone file holds the baseline findings, the container recipe and P2.T1's sequencing rule they refer to. Filed 2026-10-24 at M91's request.

Deps: M107 - GPU Viewer Hand-off, M91 - NVIDIA GPU Validation. Needs the NVIDIA host.

## Phase 168.1: Viewer hand-off on NVIDIA GL

- [ ] M168.P1.T1 — Surface the staged-transfer reasons in the region profile (was M91.P3.T0)
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`, `Engine/RenderStats.h`, `Engine/RenderStats.cpp`
  - approach: M91.P2.T2 added `GpuTransfer::stagedReasons()`, but `RegionReport` is filled only in `GpuRegionTask.cpp`, which is an M107 file. After M107 merges: snapshot `stagedReasons()` beside `importStatsBefore` (~l.353) and write the deltas into new `RegionReport` staged* fields next to `importsAtUpload` (~l.394). Copy them into `GpuRegionTimes` in `RenderScheduler.cpp` (~l.1303). In `formatRegionRecord`, write `"stagedReasons":{"disabled":N,"unaligned":N,"importFailed":N,"forgotten":N}` only when a count is nonzero, so the exact-match string in `RenderProfile_Test.cpp` (~l.336) is unchanged.
  - verify: a `RenderProfile_Test` case with a nonzero staged count emits the object. The t5 render's profile shows reasons for every staged transfer.
  - size: M

- [ ] M168.P1.T2 — Let the GUI test harness run on an existing host display with that display's GL (was M91.P3.T1)
  - files: `Tests/gui/run-gui-test.sh`
  - approach: Add a mode, `GUI_TEST_HOST_DISPLAY=:0`, that skips Xvfb, the `/usr/lib64` GL symlink directory and the `mesa`/`LIBGL_ALWAYS_SOFTWARE` exports, running the extracted AppImage on the host against the system's glvnd with `env -u LD_LIBRARY_PATH`. Fail with a clear message when the display can't be opened, or when the GL renderer isn't hardware (that check takes an opt-out). The default Xvfb mode stays byte-identical.
  - verify: `GUI_TEST_HOST_DISPLAY=:0 XAUTHORITY=… Tests/gui/run-gui-test.sh Tests/gui/<existing smoke>.py <fast AppImage>` passes and logs an NVIDIA renderer. The same script still passes under Xvfb in the container. `bash -n` and `shellcheck` pass.
  - size: M

- [ ] M168.P1.T3 — Run M107's suites and UAT on NVIDIA, on every rung (was M91.P3.T2)
  - files: none in code. Output goes to `.local/m168/`, results in this milestone file.
  - approach: One remote `fast --tests` on this milestone's branch (main with M107 merged). In the NVIDIA container (M91.P1.T4's recipe), run `ViewerHandoff_Test`, `ViewerRootOnDevice_Test`, `ViewerGpuProcess_Test`, `OcioVulkanShader_Test` and `GlInterop_Test`, in both scheduler modes. Run `Tests/gui/m107_viewer_handoff.py`: cpu reference first; then `NATRON_VIEWER_HANDOFF=zerocopy` and `hostsync` on `:0` (P1.T2); `readback` under Xvfb; `NATRON_GPU_FAULT=lost@40` on the hardware device. Run `tools/gpu-spike/desktop-handoff-check.sh` on `:0`. Check via `NATRON_RENDER_PROFILE` that root regions report `downloadBytes == 0` on zerocopy and hostsync.
  - verify: all suites pass on the RTX 3070 Ti. Each UAT config shows `SUMMARY 0 failure(s)`. The handoff report says zero-copy was reached on NVIDIA GL, or names what is missing and the logged fallback rung. Viewer-path failures are fixed under M91.P2.T1's rules or filed as follow-ups.
  - size: M

**Verification gate:** P1.T1's `RenderProfile_Test` case passes in a remote `fast --tests` build, and P1.T3's results section in this file shows every listed suite passing on the RTX 3070 Ti in both scheduler modes, `SUMMARY 0 failure(s)` for each UAT config, and the zero-copy result (or the missing piece and its fallback rung).

## Decisions

- 2026-10-25 — Claimed despite file overlap: P1.T1's `Engine/GpuRegionTask.cpp` overlaps an open M101 - Clean-Sheet Merge Node task (P7.T2), and its `Engine/RenderScheduler.*`/`Engine/RenderStats.*` overlap parked M64 - Tiled Rendering tasks. The user accepted the overlap (§8a would otherwise make M168 wait on both); the edits are small additive fields, so whichever lands second rebases.
- 2026-10-25 — Freshness check passed against main 3c8553ec8 (M107 merged): references hold, line numbers drifted (`importStatsBefore` ~l.379, `importsAtUpload` ~l.421, `GpuRegionTimes` ~l.1286 in `RenderScheduler.cpp`, `formatRegionRecord` in `RenderStats.cpp` ~l.786). The suites live under `Tests/Native/` and `Tests/Gpu/`.
