# M82 - Render Cost Profiling

Measure where render time actually goes on real comps, and what host↔GPU transfers cost on real hardware, before committing to any GPU architecture. The output is a go/no-go and a shape for M83 - GPU Compute Backend Spike and M84 - GPU Placement And Residency: do expensive nodes sit close enough to form resident regions, or are they isolated, in which case a few heavy nodes with async transfers is enough?

## Phase 82.1: Instrumentation

- [x] M82.P1.T1 — Record per-task render cost in the task-graph scheduler
  - files: `Engine/RenderScheduler.cpp`, `Engine/RenderScheduler.h`, a new `Tests/RenderProfile_Test.cpp`
  - approach: In `RenderScheduler::executeTask`, when a profiling switch is on (env var `NATRON_RENDER_PROFILE=<path>`; off by default, zero cost when off), record per task: plugin ID, node script name, time, view, mipmap level, RoI pixel count, components and bit depth, input-dependency task IDs, `estimatedBytes`, wall time of `renderRoI`, and whether the node is a pure point op (native `PixelKernel` effect) or reports GL support. Append records to a JSON-lines file per process, buffered and mutex-guarded so pool threads don't contend on I/O.
  - verify: a gtest renders a small Read → Grade → Blur → Merge graph with the variable set and checks one record per task with a non-zero time and correct dependency edges.
  - size: M
- [x] M82.P1.T2 — Region analysis report script
  - files: a new `tools/bench/render-profile-report.py`
  - approach: Read the JSON-lines output. Aggregate time per plugin. Mark nodes as heavy (above a configurable ms/Mpx threshold) or cheap. Find connected regions of "GPU-candidate" nodes (heavy nodes plus the cheap nodes linking them). For each region, compare its CPU time with its boundary bytes ÷ a bandwidth parameter (default 25 GB/s; unified memory as a second scenario). Print a per-comp summary: share of frame time in heavy nodes, number and size of regions, estimated net GPU gain.
  - verify: runs on M82.P1.T1's test output and on a synthetic file with a known answer.
  - size: M

## Phase 82.2: Measurement

- [x] M82.P2.T1 — Host↔GPU transfer microbenchmark
  - files: spike code under `build/` only; results into a `PLAN/DESIGN/` note
  - approach: Measure upload and download of a UHD RGBA float frame (and half, and 8-bit) through the existing GL PBO path, and through plain Vulkan with pinned staging if that's quick to stand up. Runs on the host GPU (the container has no usable GPU under Xvfb). Record PCIe generation and width.
  - verify: a design note with measured GB/s per direction and per path.
  - size: M
- [x] M82.P2.T2 — Profile representative comps
  - files: a `PLAN/DESIGN/` report
  - approach: Profile at least three comps with M82.P1.T1 and M82.P1.T2: a keying-and-grade comp, a CG multi-pass merge comp, and a defocus/retime-heavy comp. Use user-supplied comps if available, otherwise build plausible ones. The dev host is 4-core, so state the core count next to every number and prefer a faster host if one is available. Feed in M82.P2.T1's bandwidth.
  - verify: the user reviews the report and gives a go/no-go plus scope for M83 - GPU Compute Backend Spike.
  - size: M

- [ ] M82.P2.T3 — Fix the region report's gain, writer and time handling
  - files: `tools/bench/render-profile-report.py`, `tools/bench/comps/wall_gain.py` (fold in and delete), `tools/bench/comps/run_comps.sh` if it calls it
  - approach: Report net gain as a share of wall-clock frame time: divide thread-summed task time by the measured parallelism (task time ÷ frame wall time), as `wall_gain.py` does, and drop the old task-time "gain" so there's only one figure. Exclude writer/encoder nodes (`internalEncoderNode` and Write plugins) from GPU-candidate regions; they count as region boundaries. Key frames on the record's `frame` field, not `time`, which carries sentinels in retime comps. Add a `--serial-profile` option, or document that the heavy/cheap split should come from a 1-thread profile.
  - verify: the synthetic known-answer file still passes; rerunning on the three comps' raw output in `build/bench/m82-profile/` reproduces the report's wall-clock gains, with no writer node in any region.
  - size: M

**Verification gate:** `ctest` green including `RenderProfile_Test`; profiling off by default with no measurable overhead; the transfer note and comp report exist in `PLAN/DESIGN/`; the user has given a go/no-go on M83 - GPU Compute Backend Spike.

## Decisions

- 2026-10-10 — Runs on a second machine alongside M75 - Native Read: the user started the GPU milestones on the 7800X3D / RX 7900 XTX workstation while M75–M80 run on another machine. This PM leaves the board frontmatter (`current`, `pm_heartbeat`) to the M75 PM, edits only the M82–M87 rows and files, and pulls with rebase before every plan push. The PR targets `main`, not stacked on M75.
- 2026-10-10 — First full ctest on this machine: 1251/1253; the two failures (`BlurKernels.CImgReferenceAvailable`, `NativeErodeDilateKernels.CImgReferenceAvailable`) are a missing CImg.h test asset after the dev image was rebuilt from scratch, not code. `RenderProfileTest` passes.
- 2026-10-10 — M82.P2.T2 uses synthetic comps (user): no real comps were supplied, so a subagent builds plausible keying-and-grade, CG multi-pass merge and defocus/retime comps and profiles them on this 16-core host. It runs once M88 - FIR Gaussian Blur releases the Natron build slot.
- 2026-10-10 — M82.P2.T2 comps and report landed (code: `248ad8d0a`; report `PLAN/DESIGN/2026-10-10-render-cost-profile.md`). The task stays unchecked until the user reviews it. Profiler/report issues found: the report's "net gain" subtracts serial transfer time from thread-summed task time, so it isn't a frame share (`tools/bench/comps/wall_gain.py` rescales it by measured parallelism); `wallNs` under 16-thread contention makes the heavy/cheap split unreliable, so 1-thread profiles are used for it; the Write encoder lands inside GPU regions; the `time` field carries sentinels in retime comps. Transform with motion blur costs ~1014 ms/Mpx, worth a separate look.
- 2026-10-10 — User reviewed the comp report: **go**, and M84 - GPU Placement And Residency builds **resident regions from day one** rather than placing heavy nodes first. Added M82.P2.T3 (user) to fix the report script before the PR.
