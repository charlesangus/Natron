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

- [ ] M154.P3.T2 — ⚑ Allocate native nodes' scratch buffers uninitialised from the pooled huge-page allocator (after M137 - Native Base Class Flexibility merges: it rewrites `ErodeDilate.cpp` and `EdgeDetect.cpp`)
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

- [ ] M154.P5.T1 — Thorough bench against main on the target host
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

**Verification gate:**
- Fresh-process single frame (a-warm), Erode 40 16k, CPU-only (NATRON_GPU=off): the in-process render time drops by ≥ 5% against main, as the median of same-boot pairs with every round below 1.0. 24k and EdgeDetect are reported.
- GPU Auto shows no regression: within ±2% of main on every bench case and kind.
- No warm regression: bc (same-frame re-render, frame jump) and playback frames 2+ are within ±2% of main in both modes, and Off bc re-renders are reported (the pooled scratch should speed them too).
- On every session, peak RSS (`VmHWM`) is ≤ main's + 2%. The pool never exceeds `maxPoolBytes()`.
- Idle trim still works: after 2× the idle period, the idle process's pooled bytes and RssAnon are back at the floor.
- Full ctest passes on lavapipe, RADV and no ICD, in both scheduler modes, and `-DNATRON_GPU=OFF` builds.

## Decisions`, data under `.local/m154/p1/`)
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

- [ ] M154.P3.T2 — ⚑ Allocate native nodes' scratch buffers uninitialised from the pooled huge-page allocator (after M137 - Native Base Class Flexibility merges: it rewrites `ErodeDilate.cpp` and `EdgeDetect.cpp`)
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

- [ ] M154.P5.T1 — Thorough bench against main on the target host
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

**Verification gate:**
- Fresh-process single frame (a-warm), Erode 40 16k: the in-process render time drops by ≥ 5% against main, as the median of same-boot pairs with every round below 1.0, on both GPU Auto and CPU-only (NATRON_GPU=off). 24k is reported.
- No warm regression: bc (same-frame re-render, frame jump) and playback frames 2+ are within ±2% of main in both modes on every bench case.
- On every session, peak RSS (`VmHWM`) is ≤ main's + the reserve budget P3.T1 states, or ≤ main's + 2% when P3.T1 dropped. The pool never exceeds `maxPoolBytes()`, and the reserve never leaves `MemAvailable` below its stated margin (unit test).
- Idle trim still works: after 2× the idle period, the idle process's pooled bytes and RssAnon are back at the floor.
- `prepareWaitMs` on first GPU renders stays ≤ 1.2× its page-zeroing floor at the same-boot THP rate (M109's gate line).
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
