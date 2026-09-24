# Milestone 28: Stop treating page cache as memory pressure

`AppManager::checkCacheFreeMemoryIsGoodEnough()` drains the node cache and the
deep image cache whenever `getAmountFreePhysicalRAM()` falls below
`getSystemTotalRAM() * getUnreachableRamPercent()` (default 20%).
`getAmountFreePhysicalRAM()` (`Engine/MemoryInfo.cpp:305`) reads
`sysinfo.freeram` — Linux `MemFree`, which excludes page cache. On any
long-running Linux desktop the kernel deliberately keeps `MemFree` low and
`MemAvailable` high, so Natron reads normal, healthy caching as an emergency
and evicts the caches the user's interactivity depends on.

The loop also cannot reach its own exit condition this way: releasing cache
entries returns memory to the allocator, not to the OS, so `MemFree` barely
moves and it keeps evicting until a cache is empty. Measured 2026-09-11 on a
15735 MB host: `MemFree` 2879-2990 MB against a 3147 MB bar, with 7800 MB in
`buff/cache` and `MemAvailable` around 10300 MB — so every cache allocation
drained both caches, which is what made two of M18's tests fail (fixed
hermetically in M18.P1.T5, which does not touch this).

## Phase 28.1: Measure the right thing

- [x] M28.P1.T1 — Read reclaimable memory, not free memory
  - files: `Engine/MemoryInfo.cpp`, `Engine/MemoryInfo.h`, `Tests/` (new case)
  - approach: on Linux, prefer `MemAvailable` from `/proc/meminfo` — the kernel's own estimate of what is allocatable without swapping, which is exactly the question being asked — and fall back to `sysinfo.freeram` only if the field is absent (it has been present since Linux 3.14, so the fallback is belt-and-braces, not a supported path). Keep the function's contract and name honest: if it now returns available rather than free memory, rename it and update both call sites (`AppManager.cpp:2806`, `:2817`) rather than leaving a name that says `Free`. The `_WIN32` and BSD/Apple branches are dead in this Linux-only fork — check whether they are reachable at all before spending effort on them, and delete rather than maintain them if they are not.
  - verify: unit test parsing a fixture `/proc/meminfo`-shaped buffer (factor the parse out so it is testable without the host's real numbers), covering the field present, absent, and malformed; plus a measured before/after on this host showing the reading move from ~2900 MB to ~10300 MB. Whole ctest suite green.
  - size: M

- [x] M28.P1.T2 — Make the eviction loop terminate on its own terms
  - files: `Engine/AppManager.cpp`, `Tests/` (extend the `evictLRUFromMemoryCaches()` coverage from M18.P1.T4)
  - approach: even with the right reading, the `while` loop in `checkCacheFreeMemoryIsGoodEnough()` re-reads a host-global number that its own evictions barely move, so under genuine pressure it degenerates into "empty both caches". Bound it by what it is actually trying to achieve — evict until the caches' own accounted memory has dropped by the shortfall, with the host reading as the trigger rather than as the loop condition. Do not silently change the trigger threshold; if 20% is the wrong default, say so and raise it separately.
  - verify: a test driving the loop with a stubbed/injected free-memory reading, asserting it stops after freeing the shortfall rather than draining the caches; driven red-then-green. Whole ctest suite green.
  - size: M

## Decisions

- 2026-09-23 — **P1.T1 landed.** `getAmountFreePhysicalRAM` was renamed to `getAmountAvailablePhysicalRAM` and now reads `MemAvailable` through the testable `parseMemAvailableKB`. The unreachable Windows/BSD/Apple branches in `MemoryInfo.cpp` were deleted. On this host MemFree read 447 MB while MemAvailable read 11.3 GB. The full ctest suite passed, 400/400.
- 2026-09-23 — **P1.T2 landed** (d66bde20f). The seam is a pure `evictMemoryCachesUntilShortfallCovered(available, keepFree, getAccounted, evictOnce)`: the host reading is only the trigger, and the loop stops once accounted cache memory has dropped by the shortfall. Full ctest passed, 403/403. Gate green; opened PR #32 against `main`.
- 2026-09-24 — **PR #32 Codex review (one round): 6 findings, all fixed in bd4ea7b18.** The MemAvailable parser is now strict, the disabled RSS helpers are deleted, and 4 comments were trimmed. Full ctest passed, 408/408. The PR stays open for the user to merge.

**Verification gate:** the free-memory reading reflects reclaimable memory on this host; the eviction loop provably stops short of draining the caches under a simulated shortfall; whole ctest suite green.
