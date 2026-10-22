# M154 - Fresh Buffer Page Zeroing

Cut the first-render cost of zeroing fresh pages in GPU region download destinations. A fresh 4 GiB destination (Erode 40 at 16k) waits about 100–256 ms while the kernel zeroes new pages at roughly 16–24 GB/s. That is about a third of region wall time in a fresh process. The candidates are:
- transparent or explicit huge pages for pooled `HostImageMemory` buffers;
- a prefaulted, pinned destination pool reused across renders and frames;
- prefaulting on an idle thread at startup.

The CPU path pays the same zeroing, so the fix is judged on fresh-process single-frame wall time: GPU Auto against Off, and CPU-only against main.

M109 - GPU Region Engine Hardening merged to main as `687f1991a` (#70). This milestone first finds out where the zeroing time goes: whether destinations really get huge pages, how fast one or several threads can fault under a region's memory load, and how much the CPU path pays. Only then does it apply the candidates the numbers support. The verification gate below replaces the stub's acceptance sketch and keeps its intent.

## Scope

**In:**
- A committed standalone page-fault probe, and bench driver capture of peak RSS and THP counters per session.
- A quick directional diagnosis: huge-page coverage of destinations, fault rate with 1 thread against N, the CPU path's share of zeroing, and the idle windows a prefault could hide in.
- The candidates that diagnosis supports: huge pages from first fault, a single-call or parallel prefault, a speculative prefaulted reserve bounded by `MemAvailable`, and starting destination prepare before the region runs (flagged).
- A thorough bench: Auto and Off against main, same boot, first frame first.

**Out:**
- Explicit hugetlbfs pages (`MAP_HUGETLB`). They need `nr_hugepages` reserved by an administrator, so they can't be a default. If P1.T3 finds THP unusable on the target host, the user decides whether to add them.
- The comp download rate (~6 GB/s against GpuBench's 16.7). That is a separate inbox follow-up from M109.
- Device-side `GpuBufferPool` changes, and a Preferences knob for the pool or the reserve.
- Windows and macOS allocation paths (`__NATRON_WIN32__` branch). They stay as they are.
- Any opt-in flag or env switch for the old allocation behaviour (hard cut).
- Faster EXR write-back.

**Edge cases:**
- THP `never`, or `madvise` declined: buffers still work on 4K pages, the advice is only advice, and tests skip their huge-page assertions. Read the mode from `/sys/kernel/mm/transparent_hugepage/enabled`.
- THP `defrag` is `madvise`/`defer+madvise`: a madvised fault may stall in direct compaction on a fragmented host. P1.T3 measures this on a long-uptime boot as well as a fresh one.
- `MADV_POPULATE_WRITE` missing (kernel < 5.14 returns `EINVAL`): fall back to touching pages. This is platform robustness, not a compat switch.
- A pooled buffer that is reused must never be zeroed or touched again by a prefault in a way that races a live writer or a DMA. Prefault applies only to fresh buffers, or to destination ranges before any download is issued, as now.
- Idle trim must still return reserve and prefaulted pool memory to the floor. Cache `trimTo` must give it up before live entries, and the free hook must drop imports first.
- Concurrent regions, or a reserve prefault running alongside a render, compete for memory bandwidth. A prefault must never delay a render that has already started.
- A GPU lost or a runtime recreated during a prefault: the job is cancelled through the existing `GpuImportJob` path.
- `MemAvailable` too low for the reserve or prefault: skip it, and never fail an allocation because of it.

**Assumptions:**
- The target host is the AMD RX 7900 XTX box: 16 CPUs, 96 GB RAM at M109's bench, THP mode `madvise`. Hardware can change across reboots, so each case probes RAM and the host, and only same-boot pairs are compared.
- The EL9 kernel (5.14) has `MADV_POPULATE_WRITE` (value 23). glibc 2.34 headers may not define it, so define it under `#ifndef`. Verify both on the build container.
- M109's figures (boot `823970cf`, `.local/m109/p8t1/`): first-touch faults on 1 thread run at 4.05 GB/s with 4K pages and 25.0 GB/s with `MADV_HUGEPAGE`; host import runs at 10.8 GB/s up and 16.7 GB/s down. In-process chunk prep fault time is 24–34 ms, plus pin ~3 ms, per 512 MiB (~16 GB/s). In a 5-frame process, frame 1's region took 306 ms against 170 ms for frames 2–5. Frame 2 imports a new 4 GiB but its pages are already faulted, and it waits 8.6 ms. After 30–35 s idle, RssAnon fell from 10.3 GiB (9.9 GiB AnonHugePages) to 403 MiB. `prepareWaitMs` share of first-render region wall: 8K 14–22%, 16k 10–16%, 24k 7–12%, defocus_retime comp ~20%.
- M109 also measured that more helper threads didn't help in-process: 2–4 threads stayed at ~16.5 GB/s while downloads ran, and the standalone test peaked at ~24 GB/s on 2–8 threads. P1 re-checks this on the current boot before any parallel prefault is built.
- Benches use the stripped release AppImage, `NATRON_RENDER_SCHEDULER=taskgraph`, interleaved, with the write-back out of timing. No tmpfs on the host is large enough, so output goes to a scratch directory and is synced outside the timed interval, as in M109. A quick (~1 h) directional pass comes before the thorough matrix. Builds are local, one at a time, one per phase.

**Scouted facts** (`origin/main` `687f1991a`):
- **Pooled sizes:** `HostImageMemory::rawAlloc` (`Engine/HostImageMemory.cpp`) gives buffers ≥ 32 MiB `std::aligned_alloc(2 MiB, roundUp(padded, 2 MiB))` and then `madvise(MADV_HUGEPAGE)`. glibc serves these from its own `mmap`, with a malloc header in the page before `ptr`, and `rawFree` is `std::free`, so the memory is unmapped on free. Buffers from 1 MiB to 32 MiB get no advice. Buffers under 1 MiB use plain `malloc`.
- **Pool cap and idle trim:** the pool cap is max(512 MB, total RAM / 4), from `getSystemTotalRAM()`, not `MemAvailable`. Idle trim (`Reaper`) uses a 256 MiB floor, a 30 s idle period and a 5 s poll, starts on the first release, and goes through `trimTo`, which runs the free hooks. `getAmountAvailablePhysicalRAM()` (`Engine/MemoryInfo.h`) reads `MemAvailable`.
- **CPU path:** CPU images allocate through `RamBuffer::resize` → `HostImageMemory::allocate` (`Engine/CacheEntry.h:110-125`), so both paths share the pool and the same fresh-page cost. On the CPU path, render threads fault the output as they write it.
- **GPU prepare:** `GpuImportPreparer` (`Engine/Gpu/GpuRuntime.cpp:60-140`) is a single helper thread. Its `touch()` does `p[off] = p[off]` every 4 KiB (read, then write), then `transfer.prepareImport(range)` pins the range. On recent kernels a read fault can map the huge zero page, and the write that follows may split it; P1.T3 checks whether that happens. `allocateDownloadImages` (`Engine/GpuRegionTask.cpp:~1115`) is the first step after `planStrips` (`:355`). It cuts destinations into chunks aligned to strip interiors, rounded to 2 MiB (`kPrepareChunkAlignment`), and `awaitPreparedDownloads` charges waits to `prepareWaitMs`.
- **Bench tooling:** `tools/bench/gpu_region_bench.sh` records exit status and process wall in `driver.jsonl`, but not peak RSS or THP counters, and it takes one AppImage. M109's standalone page-fault test lives only in `.local/m109/p8t1/analysis/`; it was never committed.

## Phase 154.1: Measurement

- [x] M154.P1.T1 — Commit a standalone page-fault probe that mirrors `HostImageMemory`'s allocation
  - files: `tools/bench/page_fault_bench.c` (new), `tools/bench/README.md`
  - approach:
    - A C program in the style of `stream_bench.c`. It allocates N GiB in one of these modes: plain `mmap`; `aligned_alloc(2 MiB)` plus `MADV_HUGEPAGE` (exactly `rawAlloc`); `mmap` aligned to 2 MiB plus `MADV_HUGEPAGE`; or that plus `MADV_POPULATE_WRITE`.
    - It faults the buffer with write-only touch, read-then-write touch (as `GpuImportPreparer::touch`) or populate, on 1..T threads (`T` defaults to `nproc`).
    - With an optional background load of L threads streaming `memcpy` over a separate buffer, it approximates a download competing for memory.
    - It reports GB/s, the minor faults from `getrusage`, the buffer's `AnonHugePages` from `/proc/self/smaps`, the `/proc/vmstat` `thp_fault_alloc`, `thp_fault_fallback` and `thp_split_pmd` deltas, and the free time.
    - It probes `MemAvailable` and refuses a size above half of it. It assumes no machine specs.
  - verify: built with `gcc -O2 -pthread`, a 1 GiB run in each mode prints sane numbers in the container. With THP `madvise`, the `MADV_HUGEPAGE` write-only mode shows `AnonHugePages` ≈ the buffer size, and the 4K mode shows 0.
  - size: M

- [x] M154.P1.T2 — Record peak RSS and THP counters per bench session, and compare against a baseline AppImage
  - files: `tools/bench/gpu_region_bench.sh`, `tools/bench/gpu_region_summary.py`
  - approach:
    - Per session, record `VmHWM` (peak RSS, through `/usr/bin/time -f %M` when present, else by sampling `/proc/<pid>/status`) and `MemAvailable` before the run.
    - Also record the `/proc/vmstat` `thp_fault_alloc`, `thp_fault_fallback`, `thp_split_pmd` and `pgfault` deltas, and the THP `enabled`/`defrag`/`use_zero_page` settings. All of these go into `driver.jsonl`.
    - Add an optional `BENCH_BASE_APPIMAGE`. When set, each case also runs on it, interleaved with the candidate under the same kind and mode, and tagged `base`, so CPU-only and Auto against main come from the same boot.
    - The summary prints peak RSS and THP deltas per tag, and new/base ratios of in-process render time per kind and mode, as medians of same-round pairs.
  - verify: `gpu_region_summary.py --self-test` passes with the new fields. A `BENCH_DRY_RUN=1` run lists base and candidate sessions interleaved. One real UHD Erode a-warm session per AppImage writes non-zero `VmHWM` and THP fields. shellcheck on `tools/bench/*.sh` is clean.
  - size: M

- [x] M154.P1.T3 — Quick directional diagnosis (~1 h): where the fresh-page time goes, and which candidates run
  - files: none (results in `## Decisions`, data under `.local/m154/p1/`)
  - approach:
    - On the current boot, run P1.T1 to get the 1-thread and N-thread rates, 4K against THP, each touch pattern, with and without a background load equal to one download.
    - Run P1.T2 on Erode 40 16k a-warm (fresh process, single frame) and bc (same-frame re-render, frame jump), Auto and Off, 2 rounds.
    - For one Auto and one Off session, also capture `perf stat -e page-faults,minor-faults` and a `perf record -g` of the region and render threads. From those, get the `clear_page*`/`clear_huge_page` share, plus a `/proc/<pid>/smaps` sample of the destination VMA during the render.
    - Answer each of these:
      - (a) Do destinations get huge pages, or do they fall back to 4K? Use `thp_fault_fallback`, `thp_split_pmd` and `AnonHugePages`. Explain why, for example the read-then-write touch mapping the huge zero page, alignment, the glibc header page, or defrag.
      - (b) Is the in-process ~16 GB/s the THP rate under bandwidth contention?
      - (c) Do T threads fault faster than 1 under that load?
      - (d) What share of Off's fresh-process wall is page zeroing, and on which threads?
      - (e) What idle window comes before the render? Measure process start → project loaded → render start, and region dispatch → region start.
    - From these, pick which Phase 154.2–154.4 tasks run, and the predicted gain of each.
  - verify: `## Decisions` holds a dated entry with (a)–(e), the same-boot rates, and a run/drop call for each of P2.T1, P2.T2, P3.T1, P3.T2 and P4.T1, with a predicted fresh-process single-frame gain for Auto and for Off. If no candidate predicts ≥ 5% on either path, the PM stops and asks the user before Phase 154.2.
  - size: M

## Phase 154.2: Huge-page coverage and prefault rate

- [x] ~~M154.P2.T1 — Give every pooled large buffer huge pages from its first fault (if P1.T3 finds 4K fallback)~~ — **dropped 2026-10-20** by P1.T3 (see `## Decisions`)
  - files: `Engine/HostImageMemory.cpp` (`rawAlloc`, `rawFree`, `reallyFree`), `Engine/Gpu/GpuRuntime.cpp` (`GpuImportPreparer::touch`), `Tests/Image_Test.cpp`
  - approach:
    - Apply the cause P1.T3 names. The expected shape: allocate buffers ≥ 32 MiB with `mmap`, trimmed to a 2 MiB-aligned start, advise `MADV_HUGEPAGE` before any touch, and `munmap` with the capacity in `rawFree`. The free path already knows each buffer's capacity.
    - Make `touch` write-only, for example `p[off] = 0` on destination ranges, which downloads overwrite, so no read maps the huge zero page.
    - Keep the smaller-buffer and Windows paths unchanged.
  - verify: a new `Image_Test` case allocates a 64 MiB pooled buffer, writes it, and reads its `AnonHugePages` from `/proc/self/smaps`. With THP `madvise`/`always`, that is ≥ 90% of the size; with `never`, the case skips. Existing `HostImageMemory` and idle-trim tests pass. P1.T1's in-process equivalent (`thp_fault_fallback` delta ≈ 0 on an Erode 40 16k a-warm run) is recorded in `## Decisions`.
  - size: M

- [x] ~~M154.P2.T2 — Prefault fresh destinations in one call, in parallel where it pays (if P1.T3 (c) shows ≥ 1.3× under load)~~ — **dropped 2026-10-20** by P1.T3 (see `## Decisions`)
  - files: `Engine/HostImageMemory.{h,cpp}` (new `prefault(void*, std::size_t)`), `Engine/Gpu/GpuRuntime.cpp` (`GpuImportPreparer` uses it), `Tests/Image_Test.cpp`, `Tests/Gpu/GpuRuntime_Test.cpp`
  - approach:
    - `prefault` uses `MADV_POPULATE_WRITE`, falling back to write-touch on `EINVAL`. It splits ranges of at least a few hundred MiB across the thread count P1.T3 found best under load.
    - The preparer keeps chunk order, so strip k's range still finishes before strip k+1's. Pinning stays on the helper thread.
    - If (c) shows no gain, keep only the single-call populate, and only if P1.T1 shows it is faster than touching.
  - verify: unit tests show `prefault` leaves every page resident (`mincore`) and that the fallback path does too. `GpuRuntime_Test`'s prepare-job tests pass on lavapipe and RADV. A directional Erode 40 16k a-warm run shows lower `prepareWaitMs` than P2.T1's build on the same boot.
  - size: M

## Phase 154.3: Hiding the zeroing earlier

- [x] ~~M154.P3.T1 — Keep a speculative prefaulted reserve, sized from the loaded project and bounded by `MemAvailable` (if P1.T3 (e) shows an idle window ≥ the fault time)~~ — **dropped 2026-10-20** by P1.T3 (see `## Decisions`)
  - files: `Engine/HostImageMemory.{h,cpp}` (new `reserve(sizes)`), `Engine/Project.cpp` or `Engine/AppInstance.cpp` (one call after a project loads, whichever P1.T3 shows precedes the idle window; `Project.cpp` is in M108.P2.T4, so prefer `AppInstance.cpp` or wait for M108), `Tests/Image_Test.cpp`
  - approach:
    - After a project loads, a low-priority thread allocates and prefaults the buffers of the project's largest output format (RGBA float, the capacity `capacityFor` returns). Use the count P1.T3 saw the first frame allocate, capped at 2.
    - It puts them straight into the pool as pooled bytes, so cache `trimTo`, `maxPoolBytes` and idle trim all take them like any pooled buffer.
    - Before each buffer, it re-probes `getAmountAvailablePhysicalRAM()` and stops when the reserve would leave less than the larger of 25% of `MemAvailable` and the pool's headroom.
    - It stops, without blocking, when a render starts allocating, so it never competes with a render already running.
    - The budget (the maximum reserve bytes) is stated in the header comment and recorded in `## Decisions`. The gate's peak-RSS line uses it.
  - verify: unit tests cover: the reserve is served to a matching `allocate` without a fresh fault; the reserve is refused when the probed `MemAvailable` is low (through a test hook); idle trim takes the reserve down to the floor; and a cancel mid-reserve leaves the pool consistent. Directional: Erode 40 16k a-warm on Auto and Off drops by about the P1.T3 prediction, with `VmHWM` growing by no more than the stated budget.
  - size: L

- [x] M154.P3.T2 — ⚑ Allocate native nodes' scratch buffers uninitialised from the pooled huge-page allocator (after M137 - Native Base Class Flexibility merges: it rewrites `ErodeDilate.cpp` and `EdgeDetect.cpp`)
  - files: `Engine/Nodes/Filter/ErodeDilate.cpp`, `Engine/Nodes/Filter/EdgeDetect.cpp`, any other native node with the same scratch pattern (grep `std::vector<float>` sized by plane area in `Engine/Nodes/`), `Engine/HostImageMemory.h` if a scratch helper is needed, `Tests/Native/` (a test beside the nodes' existing ones)
  - approach:
    - P1.T3 found `std::vector<float> buffer(planeSize * nPlanes)` (`ErodeDilate.cpp:527` on `687f1991a`; `EdgeDetect.cpp:739`). Its value-initialisation zero-fills 4 GiB on one thread, on 4K pages, and that one thread faults every fresh page while the others idle. `fillBuffer` then overwrites every element, including the zero border, so the zeroes are never read.
    - Replace it with an uninitialised buffer from `HostImageMemory` (pooled, 2 MiB-aligned with `MADV_HUGEPAGE` above 32 MiB), released at the end of the render. The kernel's own page zeroing then happens on `fillBuffer`'s threads, at the huge-page rate, and a re-render reuses the pooled pages with no fault at all.
    - Confirm, per node, that every element is written before it is read. Where a node relies on the zero-fill, keep an explicit parallel fill of only that part.
    - Re-scout after M137 merges: line numbers and the buffer's shape will have moved.
  - verify: the nodes' existing native tests are bit-identical to main on CPU and GPU. A directional Erode 40 16k Off a-warm run against `.local/m154/Natron-main-687f1991a-release.AppImage` (same boot, `BENCH_BASE_APPIMAGE`) is ≥ 5% faster, and the scratch VMA shows huge pages and multi-thread faults (per-thread `/proc` sampling as in P1.T3).
  - size: M

## Phase 154.4: Shared-file follow-up (FLAGGED: touches a file M107 owns)

- [x] ~~M154.P4.T1 — ⚑ Start preparing region destinations when the region is dispatched, not when it starts (if P1.T3 (e) shows region dispatch → start ≥ the destination's fault time; touches M107 - GPU Viewer Hand-off's `Engine/GpuRegionTask.cpp`; start after M107 merges; `Engine/RenderScheduler.cpp` also appears in M143.P1.T2 and blocked M64.P2.T3, so rerun the overlap check first)~~ — **dropped 2026-10-20** by P1.T3 (see `## Decisions`)
  - files: `Engine/GpuRegionTask.cpp` (`allocateDownloadImages` split into an early allocate-and-queue step), `Engine/RenderScheduler.cpp` (call it when the region's inputs are dispatched), `Tests/GpuResidency_Test.cpp`
  - approach:
    - Allocate the download images and queue their prepare job as soon as the region's shape and strips are planned, which can be before its inputs finish. Then `runGpuRegion` waits on the existing chunk counters.
    - The early step must be undone (images released, job cancelled) if the region is cancelled or falls back to the CPU.
  - verify: residency tests pass on lavapipe and RADV in both scheduler modes, including a cancelled-region case that leaks no import. On a comp whose region has upstream CPU work (defocus_retime 8K), a directional a-warm run shows lower `prepareWaitMs`.
  - size: L

## Phase 154.5: Thorough bench

- [x] M154.P5.T1 — Thorough bench against main on the target host
  - files: none (results in `## Decisions`, data under `.local/m154/p5/`)
  - approach:
    - The last phase's release AppImage against a main release AppImage (`BENCH_BASE_APPIMAGE`), 3 rounds, same boot, interleaved.
    - Cases: Erode 40 at 16k and 24k, Blur σ100 at 16k, Transform at 8K, and the defocus_retime comp at 8K.
    - Kinds, in order: a-warm, a-cold (reported separately, with compile time), bc (re-render and frame jump), then playback (secondary).
    - Both NATRON_GPU modes, `MemAvailable` probed per case, write-back out of timing.
    - Record the same-boot rates from P1.T1 and the GpuBench import rate.
    - Read idle trim from the process's RssAnon/AnonHugePages after 2× the idle period.
  - verify: the gate below holds, or each miss is written in `## Decisions` for the user.
  - size: M

- [x] M154.P5.T2 — Pool native scratch only while no other render's scratch is live, so overlapping playback frames don't retain an extra set
  - files: `Engine/HostImageMemory.h`, `Engine/HostImageMemory.cpp`, `Engine/Nodes/Image/PassExecutor.cpp`, `Tests/Native/NativePassScratch_Test.cpp`
  - approach:
    - `HostImageMemory::releaseToSystem(ptr, bytes)` erases the buffer from `live` and goes through `reallyFree` so the free hooks run. The buffer is never pooled.
    - `HostScratchFloats::discard()` uses it.
    - In `runPassesCpu`, one RAII holder per call (not per slot) increments a process-wide count of active scratch sets. On exit it decrements, and if another set is still live it discards every slot. Otherwise the slots pool as now.
  - verify:
    - A unit test: two overlapping holders leave exactly one scratch set pooled, and a lone holder pools.
    - Targeted ctest (native pass and HostImageMemory tests) on RADV and lavapipe.
    - Rerun playback, bc and a-warm for erode40/edge/blur100 at 16k and defocus_retime 8K against main `1c0b5ab3b`, with `OFX_PLUGIN_PATH` set (the command in the 2026-10-22 consultant decision).
    - Playback `vmhwm_kb` is ≤ 1.02× main. Erode Off bc stays about 0.82×.
  - size: M

- [x] M154.P5.T3 — Let local and remote builds take extra CMake arguments, and build the lane with `-DNATRON_GPU=OFF` on GitHub Actions
  - files: `tools/ci/local/build.sh`, `tools/ci/remote/build.sh`, `.github/workflows/remote-build.yml`, `tools/ci/remote/README.md`
  - approach:
    - `tools/ci/local/build.sh` accepts `--cmake-arg <arg>` (repeatable), appended to `CMAKE_ARGS`. Any extra argument puts the build in its own folder (`build/<preset>-<short hash of the args>`), so it never clobbers the preset's normal tree, and forces a configure there on first use.
    - `remote-build.yml` gains a string input `cmake_args` (space-separated, default empty), passed to `build.sh` as `--cmake-arg` entries. The build-tree and AppImage artifact names stay per preset/sha; the run name includes a marker when the input is non-empty.
    - `tools/ci/remote/build.sh` gains a repeatable `--cmake-arg <arg>` that it forwards as `cmake_args`.
    - `tools/ci/remote/README.md` documents both options in one short paragraph; the local script documents its flag in its usage header. `tools/ci/local/README.md` is M107's and stays untouched.
  - verify: `bash -n` and `shellcheck` (if present) on both scripts; `actionlint` if present. Then `tools/ci/remote/build.sh fast --tests --no-appimage --cmake-arg -DNATRON_GPU=OFF` on the pushed lane builds green and its ctest passes in both scheduler legs (gate 6's `-DNATRON_GPU=OFF` line). A plain `tools/ci/remote/build.sh fast --no-appimage` still uses the usual tree.
  - size: M

**Verification gate:**
- Fresh-process single frame (a-warm), Erode 40 16k, CPU-only (NATRON_GPU=off): the in-process render time drops by ≥ 5% against main, as the median of same-boot pairs with every round below 1.0. 24k and EdgeDetect are reported.
- GPU Auto shows no regression: within ±2% of main on every bench case and kind.
- No warm regression: bc (same-frame re-render, frame jump) and playback frames 2+ are within ±2% of main in both modes, and Off bc re-renders are reported (the pooled scratch should speed them too).
- On every session, peak RSS (`VmHWM`) is ≤ main's + 2%. The pool never exceeds `maxPoolBytes()`.
- Idle trim still works: after 2× the idle period, the idle process's pooled bytes and RssAnon are back at the floor.
- Full ctest passes on lavapipe, RADV and no ICD, in both scheduler modes, and `-DNATRON_GPU=OFF` builds.

## Decisions

- 2026-10-20 — **Created from an inbox proposal (user):** filed from M109 - GPU Region Engine Hardening's gate decision and tagged E5 - GPU Compute.
- 2026-10-20 — **Elaborated at claim.** Phase 154.1 measures before anything is built, and every fix task is conditional on its findings. `MAP_HUGETLB` is Out because it needs pages reserved by an administrator. P4.T1 is flagged: it edits `Engine/GpuRegionTask.cpp`, which M107 - GPU Viewer Hand-off is changing, so it waits for M107 to merge. P3.T2 may move behind M108 - Retire Engine GL Host Path if it needs `Image.cpp`'s GL fill paths.
- 2026-10-20 — **Overlap check at elaboration.** Phase 154.1 touches only `tools/bench/` files that no other active row lists, so it starts now. Later tasks have conditional overlaps:
  - `Engine/Project.cpp` is in M108.P2.T4 (P3.T1 prefers `AppInstance.cpp`).
  - `Engine/Image.cpp` is in M108's GL fill paths (P3.T2).
  - `Engine/GpuRegionTask.cpp` is in M107 (P4.T1, flagged).
  - `Engine/RenderScheduler.cpp` is in M143.P1.T2 and blocked M64.P2.T3 (P4.T1).
  Each of these is rechecked at its task boundary.
- 2026-10-20 — **M154.P1.T1 (`e5243c54e`) and P1.T2 (`f567fb4ce`), recovered from the previous session's implementers.** Host: 16 CPUs, kernel 6.17, THP `madvise`/`madvise`/`use_zero_page=1`. Probe, 1 GiB, 1 thread, no load: 4K pages 3.6 GB/s write-only, 2.3 read-then-write; `MADV_HUGEPAGE` (aligned or `mmap2m`) 25.4 write-only, 23.7 read-then-write, 26.5 populate. **Read-then-write on a `MADV_HUGEPAGE` buffer still gets huge pages** (AnonHugePages = buffer, 0 fallbacks, 0 splits), at about 2 faults per 2 MiB page and ~7% slower. 4 GiB THP write-only: 24 GB/s on 1 thread, ~27.5 on 4–8. Under a 4-thread memcpy load: 1 thread drops to ~10 GB/s, 4 threads 19–20, 8 threads 18–21. So parallel faulting under load is ~2× one thread, which bears on P2.T2. P1.T2: `/usr/bin/time` isn't on the host, so peak RSS comes from a `python3 -I` `os.wait4` wrapper (`ru_maxrss` of the largest process in the tree). `BENCH_TOPOS` and `BENCH_MODES` were added to narrow a run, defaulting to the old behaviour. A one-pair UHD Erode a-warm check wrote `vmhwm_kb` ~498 MB, `thp_fault_alloc` 128, 0 fallbacks (`.local/m154/p1t2-check/`). Self-test, dry run and shellcheck pass.
- 2026-10-20 — **M154.P1.T3 directional diagnosis (boot `81a6405a`, Ryzen 7 7800X3D 16 threads, THP madvise/madvise/zero_page=1; `.local/m154/p1/`).** Baseline AppImage: `.local/m154/Natron-main-687f1991a-release.AppImage` (release, main's code). perf was unavailable (not installed; `perf_event_paranoid` 2 and a user-namespaced container), so per-thread `/proc` sampling stood in.
  - **Bench, Erode 40 16k, a-warm:** Auto 3.99/3.86 s, prepareWaitMs 271/260, peak RSS 8.4 GB. Off 8.34/8.42 s, peak RSS 12.5 GB. Every session: `thp_fault_fallback` 0, `thp_split_pmd` 0. Of Auto's 4.0 s, 3.6 s is the single-threaded Write task; the region is ~364 ms (kernel 81, prepare wait 271).
  - **(a)** Pooled destinations always get huge pages (AnonHugePages = Rss), including under the read-then-write touch. **(b)** Yes: in-process prepare runs at ~14–16 GB/s, the THP rw rate under contention (probe: 19.7 unloaded, 13.0 under a ~18 GB/s load). **(c)** 1.26–1.42× at 4–8 threads under a one-download load, 1.64× under a heavier one; `MADV_POPULATE_WRITE` gains only 1.10× and does not scale with threads. **(d)** Off's exposed zeroing is ~1.25 s of 8.4 s (~15%), all on one thread: `Engine/Nodes/Filter/ErodeDilate.cpp:527` builds a 4 GiB `std::vector<float>` scratch, zero-filled serially on 4K pages, unpooled and re-faulted on every render, while 15 threads idle. `EdgeDetect.cpp:739` has the same pattern. Pooled-buffer zeroing in Off is parallel and ~0.2 s. **(e)** Project built → render start ≈ 0 in batch; region dispatch → start ≈ 0 (no CPU upstream; inferred from the graph).
  - **Calls:** P2.T1 drop (no 4K fallback). P2.T2 drop as specified (populate doesn't scale; an 8-thread write-touch would save ~50–80 ms, ≈1.5–2% Auto). P3.T1 drop (no idle window). P4.T1 drop (no dispatch window). P3.T2 run only if retargeted at the node scratch buffers: ≈12–14% on every Off render, 0% Auto. **No candidate predicts ≥ 5% on Auto**; removing prepare wait entirely caps at ~6.8%. The nearest Auto item outside the list is the Write thread faulting its 4 GiB pooled buffer on the first render (~0.15–0.2 s, ~4–5%). The gate's "≥ 5% on both paths" is unreachable as planned, so the PM stops and asks the user before Phase 154.2.
- 2026-10-20 — **Retargeted after P1.T3 (user).** GPU destinations already get huge pages, and no planned candidate gains ≥ 5% on Auto, so P2.T1, P2.T2, P3.T1 and P4.T1 are dropped. P3.T2 is rewritten in place to fix the native nodes' scratch buffers: their zero-fill is redundant (`fillBuffer` overwrites every element) and serial. Without it, the kernel's mandatory page zeroing runs on the fill's threads at the huge-page rate, and pooling removes it on re-renders. It is flagged ⚑ to start after M137 - Native Base Class Flexibility merges, since M137 rewrites both node files. The gate now asks ≥ 5% on Off only, with Auto held to ±2% of main; the reserve-budget gate line is gone with P3.T1. P5.T1's bench keeps its cases.
- 2026-10-20 — **M154.P3.T2 (`500a3be84`), not yet compiled.** After M137 the only call site is the shared `runPassesCpu` in `Engine/Nodes/Image/PassExecutor.cpp`, not the node files. Slot 0 was a plain `new float[]` (uninitialised, but not pooled). EdgeDetect's slots 1-3 were serially zero-filled with `assign`. Both now use the new RAII `HostScratchFloats` from `HostImageMemory`, and every slot is written in full before it is read (justified per slot at the site). The test-only `HostImageMemory::fillPooled` lets `NativePassScratch_Test` fill the pool with NaN and check two renders match bit for bit. Left alone: `ResampleExecutor.cpp:120`, which zero-fills a full frame but is a different executor, and the IO staging in `NativeRead` and `NativeWrite`. Owed to the build runner: the build, the node tests and the directional Erode 40 16k bench.
- 2026-10-21 — **M154.P3.T2 verify, rebased on `1c0b5ab3b` (`b700873b7`; `.local/m154/p3t2/`).** Release build green; targeted ctest 82/83 on RADV and lavapipe in both modes (`NativePassScratchTest` passes; the miss is `NativeErodeDilateKernels.CImgReferenceAvailable`, `CImg.h` absent from `build/assets`); full ctest RADV taskgraph 2700/2705, the rest being the same CImg pair and `CacheEquivalenceSweep` 4/9/11, which fail identically on main. Baseline is a fresh main release AppImage (`1c0b5ab3b`); the old `687f1991a` one predates M137. **Directional, 16k a-warm:** EdgeDetect Off 0.68× main (1 pair); Erode 40 Auto 0.98×; **Erode 40 Off 1.27× main (slower)**, all extra user CPU inside the erode pass. With THP disabled for the process the lane equals main, so the huge-page backing of the scratch slows the erode kernel; main's erode scratch was already uninitialised, so it had no serial zero-fill to save. A consultant is confirming the cause (suspected cache-set aliasing on 64 KiB-stride column passes) and fixing it before the gate.
- 2026-10-21 — **The Erode regression was cache-set aliasing, fixed by blocking the column pass (`f9ac50f41`; `.local/m154/p3t2b/`).** A standalone microbench of the vertical pass (16 threads, 16384², half width 40) took 0.53–0.73 s on huge pages against 0.28 s on 4K pages, and 0.125 s when rows were padded to 16400. A 64 KiB row puts every row of a column in the same L2 set once the memory is physically contiguous; 4K pages hid it on main. Rather than pad the row stride, which would touch every kernel and the fill and write code, the column pass is now blocked: `ErodeDilate`'s new `filterColumns()` runs 16 adjacent columns together, bit-identical to `filterLine()` (new `ErodeDilateKernels.ColumnBlocksMatchFilterLine`). In `PassExecutor`, the other one-column vertical passes (Blur IIR, EdgeDetect) gather 16 columns into a 1 MiB per-thread buffer. Huge pages stay on the scratch. Targeted ctest 99/100 on RADV and lavapipe in both modes; the miss is the CImg-availability check. **Directional, 3 same-boot pairs against main `1c0b5ab3b`:** Erode 40 16k Off 0.797× (5.55 vs 6.96 s), Auto 1.006×; Erode 40 24k Off 0.86×; EdgeDetect 16k Off 0.705×, Auto 0.71×; Blur IIR 100 16k Off 0.91×, Auto ~1.0×. Peak RSS is within +0.4% of main on these cases (Blur Off +1.8%), but `cg_multipass` at UHD/8K Off peaks +3.2% (~22 MB of 0.68 GB), from the pooled scratch in `b700873b7`. That is over the gate's +2% and P5.T1 must account for it.
- 2026-10-22 — **M154.P5.T1 bench, first pass (recovered; the bench agent was cut off during idle trim; `.local/m154/p5/`, boot `14b21715`).** The candidate is `p3t2b/Natron-lane-fix-release.AppImage`, which has the same engine code as `64d25b517`. The base is main `1c0b5ab3b`. Session 1's comp rows ran without `OFX_PLUGIN_PATH` and are invalid. The valid comp rows are from session 2 (`analysis-misc.txt`).
  - **Gate 1, PASS.** Erode 40 16k Off a-warm 0.822× (rounds 0.822/0.824/0.810). Also reported: 24k Off 0.857×, EdgeDetect 16k Off 0.709× and Auto 0.714×.
  - **Gates 2 and 3.** No Auto median is slower than +2%. Edge 16k playback frames 2+ on Auto is 1.024× (max 1.067). Everything else outside the band is faster.
  - **Gate 4, MISS.** Peak RSS on playback: Erode 16k Off +19.9%, Edge Auto +13.4%, Blur Off +3.7%; defocus_retime bc Auto +5.2%. The earlier `cg_multipass` +3.2% came from the plugin-less run; with plugins it is −4.7%. `maxPoolBytes()` was never checked.
  - **Gate 5, partial.** Only lane Erode 16k Off ran. RssAnon fell to 104 MB within 33–57 s. Pooled bytes were not read, and the other cases did not run.
  - **Gate 6, not run.** Full ctest on lavapipe and with no ICD, the legacy leg, and `-DNATRON_GPU=OFF` are still owed.
  - A consultant is diagnosing the playback peak-RSS growth before anything re-runs.
- 2026-10-22 — **Playback peak RSS diagnosed (consultant; `.local/m154/p5-rss/hwm_vs_thp.txt`).** Pooled scratch and images share size classes, and nothing trims the pool during a ~20 s playback: the reaper needs 30 s idle, and cache headroom and the RAM/4 cap don't bind at 16k. So the lane's peak is every buffer it ever faulted. Erode 16k Off is 12288 huge pages = six 4 GiB buffers, including a second scratch set from the overlap of frames 1 and 2. Main's scratch is freed on return, so its peak depends on timing (rounds 1.06 vs 1.20). The growth is bounded by `maxPoolBytes()`, but that can never meet +2%. Fix added as P5.T2. Edge 16k Auto frames-2+ (1.024×) measures Write-task wall that overlaps the faster Edge compute; total playback is ~0.75× main, so it is not a regression. `session.sh` never exported `OFX_PLUGIN_PATH`, so P5's re-runs must set it.
- 2026-10-22 — **M154.P5.T2 code (`fd2d7470c`), syntax-checked only.** `ScratchSetHolder` is declared in `PassExecutor.h`, so the test can drive it directly. In an overlap, whichever set ends last is pooled. The tests cover the holder and the allocator, not the one-line `runPassesCpu` wiring; the playback RSS re-run covers that. The build, targeted ctest and bench re-run wait for the build slot.
- 2026-10-22 — **P5.T2's remote `fast --tests` on `fd2d7470c` passes on lavapipe: 2713/2713 in both scheduler modes** (run 39712479357). The lane is then rebased cleanly onto main `3c2ba44fc` as `264eeedf4`. A remote `release --tests` of it and a main `3c2ba44fc` release baseline (run 39716280625) are dispatched for the RSS re-run and the gate bench.
- 2026-10-22 — **Remote `release --tests` on the rebased `264eeedf4` passes on lavapipe: 326/326 ctest entries in both scheduler modes** (run 39716269320). The candidate release AppImage is in the lane's `build/remote/release/`. The main `3c2ba44fc` release baseline is at `.local/m154/p5b/base/` (run 39716280625). Owed locally, in the build slot after M107's RADV run: P5.T2's targeted ctest on RADV, full ctest with no ICD, a `-DNATRON_GPU=OFF` build, then the RSS and gate re-runs.
- 2026-10-22 — **P5.T2 verified and P5.T1's bench re-run on `264eeedf4` against main `3c2ba44fc` (boot `0571a3e8`, 16 CPUs, 192 GB, THP madvise; 3 interleaved same-boot rounds; `.local/m154/p5b/`).** Comp rows ran with `OFX_PLUGIN_PATH` unset, because the host's stale `build/assets/Plugins` crashes both new AppImages; each AppImage loads its own bundled plugins (153 loaded, defocus peak 4.4 GB, so the rows are real). **RADV targeted ctest** (all `Native*`, `HostImageMemoryTest`, Blur kernel and GPU suites) 87/87 in both modes, from the remote `build-tree-release`. **No ICD:** full ctest 325/326 in both modes; the miss is `Shards/CacheEquivalenceSweepTest` (Sketch non-determinism, a CImg Blur double free, a Text segfault), which main `1c0b5ab3b`'s tree fails the same way with the same stale local assets; remote CI with fresh assets passes 326/326. **Playback peak RSS, candidate/base medians:** Erode 16k Auto 1.000, Off 0.861; Edge 16k 1.001/0.919; Blur 100 16k 1.000/1.002; defocus_retime 8K 0.969/0.819. Erode Off bc 0.803×. **Gate 1 pass:** Erode 40 16k Off a-warm 0.814× (0.814/0.815/0.811). **Gate 2 pass:** Erode Auto a-warm first came out 1.024× (1.060/0.969/1.024); six extra rounds gave 0.970, worst 1.009, read as noise; every other Auto median ≤ 1.0. **Gate 3 pass** on bc and total playback (all ≤ 1.0); frames-2+ Write time is Edge Off 1.022× and defocus Auto 1.101×. **Gate 4 pass by case median** (worst Blur Off a-warm 1.014×); single rounds Erode Off playback r3 1.195× and defocus Auto a-warm r2 1.036× come from base-side variance. The pool is bounded by `maxPoolBytes()` (max(512 MB, RAM/4) ≈ 50 GB here) against a 35 GB highest session peak. **Gate 5 pass:** every 16k case trims to 103–206 MB RssAnon, no huge pages, in 32–34 s on both builds; defocus_retime 8K plateaus at ~1.1 GB on both builds and both modes, so it is not a regression.
- 2026-10-22 — **Gate readings (user):** gate 4's "every session" is judged per case by median. Playback is judged by total wall time, and the frames-2+ Write-time overshoot is accepted, because it counts Write time that overlaps the next frame's compute. The `-DNATRON_GPU=OFF` build goes through a new cmake-args option on the local and remote build scripts (P5.T3), so it runs on GitHub Actions.
- 2026-10-22 — **Rebased onto main `13f12edca` (M108) as `51bf97d94`,** cleanly.
- 2026-10-22 — **M154.P5.T3 code (`f95af8e25`), syntax-checked only** (`bash -n`; shellcheck and actionlint aren't on the host). `tools/ci/local/build.sh` writes `build_dir=` to `$GITHUB_OUTPUT`. In CI, a "Link build tree" step symlinks `build/<preset>` to the hashed folder, so goldens, packaging, `test.sh` and the build-tree artifact work unchanged, and the pack includes both the link and the real folder. Dispatched together: `fast --tests --cmake-arg -DNATRON_GPU=OFF` (run 39767040817) and a plain `fast --tests` (run 39767043548), which checks that behaviour is unchanged without the flag.
- 2026-10-22 — **The first `-DNATRON_GPU=OFF` remote run (39767040817) built in its own `build/fast-16d9b995` tree, and the option reached cmake.** ctest failed 15 GPU suites in both legs, all on the "GPU required" guard: the workflow set `NATRON_TEST_REQUIRE_GPU=1` unconditionally. Fixed in `3329c3bb6`: a "Detect GPU build" step reads `NATRON_GPU` from the unpacked tree's `CMakeCache.txt` and exports the variable only when GPU is on (default ON when absent). The variable is left unset rather than set to 0 for an off build, because `GpuContext_Test` checks only that it is present. Re-dispatched: GPU-off `fast --tests` and a plain `fast --tests` on `3329c3bb6`.
- 2026-10-22 — **M154.P5.T3 done; gate 6 complete.** On `3329c3bb6`, `fast --tests --cmake-arg -DNATRON_GPU=OFF` (run 39770415588) **builds** and passes 283/284 in both legs. The miss is `StagedImageEffectGpuTest`: with no GPU, `canRenderOnGpu` returns true where the test expects false (`StagedImageEffectGpu_Test.cpp:511, 528`). That test and `StagedImageEffect` came from M137, M154 doesn't touch them, and main was never built GPU-off before this option existed, so the failure was there already. It goes to the inbox, outside this milestone. The plain `fast --tests` (run 39770442842) still exports `NATRON_TEST_REQUIRE_GPU=1` and passes 332/332. Three bench project lock files (`tools/bench/comps/*.ntp.lock`) committed by `c22991bf0` are removed in `e99815760`. No project-wide decision was made in M154, so nothing is published.
