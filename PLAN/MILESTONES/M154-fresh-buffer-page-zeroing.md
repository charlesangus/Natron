# M154 - Fresh Buffer Page Zeroing

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Cut the first-render cost of zeroing fresh pages in GPU region download destinations. A fresh 4 GiB destination (Erode 40 at 16k) waits about 100–256 ms while the kernel zeroes new pages at roughly 16–24 GB/s. That is about a third of region wall time in a fresh process. The candidates are:
- transparent or explicit huge pages for pooled `HostImageMemory` buffers;
- a prefaulted, pinned destination pool reused across renders and frames;
- prefaulting on an idle thread at startup.

The CPU path pays the same zeroing, so the fix is judged on fresh-process single-frame wall time: GPU Auto against Off, and CPU-only against main.

Blocked on: M109 - GPU Region Engine Hardening landing. The proposal comes from its gate decision, and its region engine is the code the candidates change.

Acceptance sketch:
- Fresh-process single-frame wall time for the Erode 40 at 16k case drops measurably on both the GPU Auto and CPU-only paths, measured on the target host.
- No steady-state (warm) regression, and no growth in peak RSS beyond the pool's stated budget.

## Decisions

- 2026-10-20 — **Created from an inbox proposal (user):** filed from M109 - GPU Region Engine Hardening's gate decision and tagged E5 - GPU Compute.
