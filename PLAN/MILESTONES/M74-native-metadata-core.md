# M74 - Native Metadata Core

Gives native nodes a host-side per-frame metadata map, so metadata no longer stops at the first native node (`Engine/OfxClipInstance.cpp` `fetchMetadata` falls back to project values there). Every later milestone in this group builds on it. See `DECISIONS/2026-10-08-native-io-and-metadata-design.md`.

## Phase 74.1: Data model and OFX bridge

- [x] M74.P1.T1 — Define the `ImageMetadata` type
  - files: new `Engine/Nodes/Metadata/ImageMetadata.h/.cpp`, `Tests/ImageMetadata_Test.cpp`
  - approach: ordered string-key to typed-value map (int, double, string, int/double vectors, matrix-free). Keys use the `ofx/`, `exr/`, `exif/`, `dpx/` prefixes from `ofxMetadata.h` (openfx-metadata checkout under `build/assets/plugin-src/openfx-metadata/include/`). Cheap copy (shared, copy-on-write), equality, stable hash for cache keys, get/set/remove/merge. No Qt types in the header beyond what the Engine already uses.
  - verify: gtest covers round-trip of every value type, copy-on-write isolation, hash stability, and merge precedence.
  - size: M

- [x] M74.P1.T2 — Bridge `ImageMetadata` to and from OFX clip metadata
  - files: `Engine/OfxClipInstance.cpp`, `Engine/OfxClipInstance.h`, new `Engine/Nodes/Metadata/OfxMetadataBridge.h/.cpp`
  - approach: functions converting between `ImageMetadata` and the OFX clip metadata property set used by `fetchMetadata` / `addReaderFileMetadata`. An OFX node downstream of a native node receives the native node's metadata instead of the project-value fallback; a native node downstream of an OFX node receives the converted set.
  - verify: gtest with a native passthrough feeding an OFX node (and the reverse) shows the key set arrives unchanged, including `ofx/frame` and `ofx/filepath`.
  - size: M

## Phase 74.2: Flow through native nodes

- [x] M74.P2.T1 — Add the metadata accessor to the native node API
  - files: `Engine/Nodes/NativeEffectBase.h/.cpp`, `Engine/Nodes/README.md`
  - approach: virtual `getOutputMetadata(time, view, ctx)` on `NativeEffectBase`. The default merges the first connected input's metadata unchanged, so existing native nodes become transparent with no edit. Results are cached per node, time and view, and invalidated when a knob or input changes, following the locking rules in `docs/decisions/2026-09-10-metadata-cache-locking.md`. Document the contract in the README.
  - verify: gtest on `Tests/TypedPassthrough_Test.cpp`'s pattern: a Shuffle and a Merge chain keep the A-input metadata; a knob change invalidates the cache.
  - size: M

- [x] M74.P2.T2 — Ensure the framework-level nodes declare their merge policy
  - files: `Engine/Nodes/Merge/Merge.cpp`, `Engine/Nodes/Generator/*.cpp` (the generator base only), `Engine/Nodes/Transform/*.cpp` only where a decision is needed
  - approach: Merge takes metadata from its A input (documented, matches Nuke's behaviour); generators emit only `ofx/frame`, `ofx/framerate`, `ofx/pixelaspect`; transforms and filters keep the default. Override `getOutputMetadata` only where the default is wrong.
  - verify: gtest per policy: Merge with differing A and B metadata yields A's keys; a generator has the minimal set.
  - size: M

- [x] M74.P2.T3 — End-to-end gate test: OFX Read → native Grade → native Merge → OFX Write
  - files: new test in `Tests/` (or an existing metadata test file), `Tests/CMakeLists.txt`
  - approach: build the graph in a `BaseTest`, read a small EXR/PNG fixture through the Read container, and assert the Write's embedded OFX writer input clip sees the Read's metadata (`ofx/filepath`, `ofx/frame` for a sequence, any format-specific keys the reader sets).
  - verify: the test passes and fails if `getInputEffectMetadata`'s OFX branch is stubbed out.
  - size: M

**Verification gate:** `ImageMetadata` and bridge gtests pass; a graph of OFX Read → native Grade → native Merge → OFX Write preserves the Read's metadata end to end; full ctest green; `format`, `lint-ci`, `build-and-test` green on the PR.

## Decisions
- 2026-10-08 — **Freshness check passed:** every referenced file exists; `ofxMetadata.h` is in-repo at `libs/OpenFX/include/ofxMetadata.h`, so briefs point there rather than at `build/assets`.
- 2026-10-08 — **Generators omit `ofx/frame` (deviation from P2.T2's brief):** `ofxMetadata.h` defines `ofx/frame` as int, the frame number within the source, omitted for a single image and never renumbered to the timeline. A generator has no source, so it emits only `ofx/framerate` and `ofx/pixelaspect`. Test sources inject `ofx/frame` as int.
- 2026-10-09 — **All four tasks landed as one commit** (`7b0df1fac`): they were implemented in parallel without a build between them and share `NativeEffectBase.*` and `Tests/CMakeLists.txt`, so splitting them would mean hand-splitting hunks. Debug build green; full ctest 1254/1254. The gate's end-to-end OFX Read → Grade → Merge → OFX Write test was not covered by any task; added as P2.T3.
- 2026-10-09 — **P2.T3 landed** (`e5d8fb6c9`): real Read and Write containers, self-generated EXR sequence; fails with the OFX bridge branch stubbed. PR #44 opened against `main`.
- 2026-10-09 — **Review:** Codex round 1 raised 6 findings, all fixed in `38d0fcaa8`. The two majors: native-fed OFX clips no longer get host-fabricated source keys (every fallback key is source-describing per `ofxMetadata.h`), and `getOutputMetadata` is now a non-virtual cache over a virtual `deriveOutputMetadata`, so overriding nodes are cached. Round 2 on the fix commit found nothing. Full ctest 1257/1257.
- 2026-10-09 — **Follow-up (not in scope):** a built-in node that is neither native nor OFX (e.g. Dot, a `NoOpBase`) still breaks the chain. Downstream OFX clips get the host fallback with a timeline-derived `ofx/frame`, and native nodes get an empty map. The unconnected-input and output-clip fallbacks also fabricate source keys, and existing tests pin that behaviour. Both predate M74; M75 - Native Read or M77 - Native Metadata Nodes should close them.
- 2026-10-09 — **Shipped:** PR #44 squash `2cf878e7f` on `main`; CI green.
