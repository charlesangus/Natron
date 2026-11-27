# M636 - For-Each Loop Thread Scaling

The geometry For-Each loop barely scales with threads. M402 - Geometry Loops' bench cooks 10,000 connectivity pieces in 1684 ms on 1 thread and 1404 ms on 4 (1.20x). The serial sections the scout found are the first cook, `GeoMerge::merge` (`Engine/Geo/GeoForEachEnd.cpp` ~:1091, `GeoMerge.cpp:237`, :244) and `GeoForEachPieces::partition` (`GeoForEachPieces.cpp:197`). The parallel loop is `parallelForCancellable` at `GeoForEachEnd.cpp` ~:1062. The bench is `tools/bench/geo/foreach_pieces.py`, with its emitter at `Tests/Native/GeoForEach_Test.cpp:1038`. Profile where the per-piece work serialises, fix the largest serial sections, and re-bench.

## Scope

**In:**
- Piece extraction, per-iteration setup, the final merge and any lock in the For-Each cook path.

**Out:**
- Feedback (iterative) loops, which are serial by definition.
- Image loops.

**Edge cases:**
- **Order of merged pieces:** stays piece order, so results are bit-identical to the serial cook.
- **Cancellation:** stays fast; parallel merge must check abort.

**Assumptions:**
- The dev host has 4 cores; many-core numbers come from the GPU host (benches together, before and after in one sitting).

## Phase 636.1: Profile

- [ ] M636.P1.T1 — Profile the 10,000-piece bench at 1, 2 and 4 threads
  - files: read-only: `Engine/Geo/GeoForEachEnd.cpp`, `GeoForEachPieces.cpp`, `GeoMerge.cpp`, `tools/bench/geo/foreach_pieces.py`; report to `build/m636/profile.md`
  - approach: time each stage (partition, per-piece setup, body cook, merge) per thread count, and take eu-stack samples (needs `-u root --privileged`) to find locks and serial sections. Check `/proc` thread states for waiting.
  - verify: profile.md gives the time per stage per thread count and names the serialising sites with file:line
  - size: M

## Phase 636.2: Parallelise

- [ ] M636.P2.T1 — Parallelise the dominant serial stages from the profile
  - files: per T1, likely `Engine/Geo/GeoMerge.cpp` (`merge`), `GeoForEachPieces.cpp` (`partition`), `GeoForEachEnd.cpp`; `Tests/Native/GeoForEach_Test.cpp`
  - approach: for merge, size the output from piece counts first, then copy pieces into their offsets in parallel. For partition, a parallel histogram then scatter. Remove any lock T1 found on the per-piece path. Keep piece order. Add a test that the parallel result equals the single-threaded result exactly.
  - verify: the equality test passes; `ctest -R "GeoForEach|GeoMerge"` green; the bench at 4 threads is at least 2x the 1-thread speed on the dev host
  - size: L

**Verification gate:** full build and ctest green; before/after bench on the GPU host in one sitting, numbers in the PR.
