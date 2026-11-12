# M403 - Geometry File Cache

Write native geometry to disk and read it back, losslessly and fast, so heavy chains and simulations can be cached and rendered headless. A native paged format mirrors `GeoDetail` (`Engine/GeoDetail.h`, `Engine/Geo/GeoPage.h`). `GeoFileCache` (the File Cache SOP) saves a frame range as a render target like Write, and `GeoFile` (the File SOP) reads any cached sequence. M404 - Geometry Solver spills its sim cache in this format.

Member of F18 - Geometry Loops And Solver: branches off and merges into `feature/f18-geometry-loops-and-solver` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- A native binary format (working extension `.ngeo`): header and version, per-attribute paged storage with constant pages stored once, per-page compression, groups, both packed kinds, `pointPrecision64`, and a content hash. Lossless for every attribute type and class.
- `GeoFileCache`: Save to Disk over a frame range, Load From Disk, or pass through; versioned paths with `$F`/frame padding and project variables; a render target in the render queue and `NatronRenderer -w`, with progress and cancel.
- `GeoFile`: reads a single file or sequence, with missing-frame handling (error, hold, or empty).
- Frames save in parallel unless something upstream (a solver) needs them in order.

**Out:**
- Reading Houdini `.bgeo`/`.geo` (none; user chose the native format only).
- USD export (stays with `GeoToUSD`).
- Alembic (none).

**Edge cases:**
- A file written by a newer format version: `GeoFile` errors naming the versions, never guesses.
- A truncated or corrupt file: error naming the file and the failed section; no partial detail.
- Missing frames in a sequence: per the node's missing-frame knob.
- Saving onto a file being read by another `GeoFile` in the same project: write to a temp file and rename atomically.
- Disk full mid-save: the frame fails with the OS error, earlier frames stay, the render reports the failure.
- Empty detail: writes and reads back as empty.

**Assumptions:**
- Unconfirmed: per-page compression uses zstd (already in the image through OpenEXR/OIIO dependencies), with a fast level by default.
- Unconfirmed: `GeoFile` loads lazily and memory-maps uncompressed pages where possible.

## Phase 403.1: Format

- [ ] M403.P1.T1 — Native geometry file writer and reader
  - files: `Engine/Geo/GeoFileFormat.{h,cpp}`, `Engine/CMakeLists.txt`, `Tests/GeoFileFormat_Test.cpp`
  - approach: Serialise a `GeoDetail` as a versioned chunked binary: header, index maps (with tombstones defragmented on write), topology, then one chunk per attribute with each page compressed independently and constant pages stored as one value; groups as paged bitsets and edge pairs; packed primitives with embedded details deduplicated by content hash. Reader rebuilds through `GeoDetailBuilder`. Pure engine code, no node.
  - verify: `GeoFileFormat_Test` round-trips details covering every attribute type, tuple size, array attributes, all four groups, both packed kinds, 64-bit `P`, and an empty detail, comparing `contentHash()`; a truncated file and a future version both error cleanly.
  - size: L

## Phase 403.2: Nodes

- [ ] M403.P2.T1 — `GeoFile` reader node
  - files: `Engine/Nodes/Geometry/GeoFile.{h,cpp,reg.inc}`, `Engine/CMakeLists.txt`, `Tests/GeoFile_Test.cpp`
  - approach: Follow `GeoImportOBJ` for structure and file-knob handling, resolving `$F` and project variables the way Read does. Missing-frame knob: Error, Hold Nearest, Empty. Time-varying only when the path has a frame token.
  - verify: `GeoFile_Test` reads a three-frame sequence written by `GeoFileFormat`, checks hold and error behaviour on a gap, and that a frameless path caches once across frames.
  - size: M
- [ ] M403.P2.T2 — `GeoFileCache` node as a render target
  - files: `Engine/Nodes/Geometry/GeoFileCache.{h,cpp,reg.inc}`, the Write render-target plumbing it reuses (`Engine/OutputSchedulerThread.cpp`, `Engine/RenderQueue.cpp` or their current equivalents), `Tests/GeoFileCache_Test.cpp`
  - approach: A geometry-input node with Save/Load/Pass-through, frame range, version knob and path. Register it as a render target so the render queue and `NatronRenderer -w` drive it; each frame pulls `evaluateGeometry()` and writes via temp-and-rename. Frames run in parallel unless an upstream node declares ordered frames (a flag M404 - Geometry Solver sets). Load mode reads through the same code as `GeoFile`.
  - verify: `GeoFileCache_Test` saves frames 1–5 of an animated `GeoTransform` chain, reloads them in Load mode with identical hashes, and a headless `NatronRenderer -w GeoFileCache1 1-5 <project>` run writes the same files; cancelling mid-range leaves earlier frames intact.
  - size: L

**Verification gate:** full ctest green including `GeoFileFormat_Test`, `GeoFile_Test` and `GeoFileCache_Test`; a headless `NatronRenderer` save of a 100-frame chain succeeds; run M360 - Geometry Kernel Language's deferred 10M-point `@P.y += 1` timing through `GeoFileCache` and record it in `## Decisions` (report, no threshold).

## Decisions

- 2026-11-12 — **Native paged format (user, /cat-plan grill):** lossless and fast, mirrors `GeoDetail`; no `.bgeo` reader and no USD cache format.
- 2026-11-12 — **`GeoFileCache` plus `GeoFile` (user):** following the File Cache and File SOPs.
- 2026-11-12 — **Saves are render targets like Write (user):** render queue and `NatronRenderer -w`; also geometry's first headless consumer, which M360 - Geometry Kernel Language's deferred bench needs.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
