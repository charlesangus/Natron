# Milestone 19: USD/Hydra foundation — ScenePayload, ReadScene, Viewport3D

"M-Scene-1" in the design doc: land the USD dependency, the `ScenePayload`/
`SceneOps` transport, one input node, and the Storm viewport. *Proves the
dependency and the viewport before any node vocabulary is built* — M20 waits on
this milestone's gate. Requires M17; parallelizable with M18.

Design authority: `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md`
("Part 2 — 3D system"). The model decision (USD as substrate, model A) is
recorded in `PLAN/DECISIONS/2026-09-05-adopt-typed-edges-and-usd-substrate.md`.

## Phase 19.1: Dependency and transport

- [ ] M19.P1.T1 — Pin and build USD in the toolchain and CI
  - files: CI workflow files (`.github/workflows/`), build container / dependency scripts, top-level CMake (find-package wiring)
  - approach: USD ≥ 24.11 (boost-free, oneTBB), **monolithic** build (`libusd_ms`), Python support OFF (avoids Shiboken6/PySide6-vs-pxr coexistence entirely), MaterialX off, Alembic file-format plugin ON. Pin the exact tag — same discipline as `PLAN/DECISIONS/2026-08-29-pin-exact-aswf-tag.md` — aligned with the CY2027 target. Check first whether the `aswf/ci-vfxall` image already carries a usable USD; prefer consuming it over building.
  - verify: CI builds link against USD; a trivial `UsdStage::Open` smoke test passes in the `build-and-test` job; local incremental build (M7 loop) still works.
  - size: L

- [ ] M19.P1.T2 — `ScenePayload` value type and `SceneOps` seam
  - files: `Engine/ScenePayload.h`, `Engine/Nodes/Scene/ScenePayloadImpl.cpp`, `Engine/Nodes/Scene/SceneOps.h`/`.cpp`
  - approach: immutable value — ordered `layerStack` (strongest last), combined `stackHash` (U64), metadata (up-axis, units, default prim, frame-rate mapping). Pimpl: USD types appear only in `Engine/Nodes/Scene/` internals, never in `Engine/` core headers. `SceneOps` is the thin author-layer/compose/query interface nodes use — deliberately *not* a full abstraction over USD semantics.
  - verify: unit tests — appending a layer changes stackHash; identical stacks hash identically; `Engine/ScenePayload.h` includes no pxr header (grep-enforced in the test).
  - size: M

- [ ] M19.P1.T3 — Per-node layer memoization and the consumer `StageCache`
  - files: `Engine/Nodes/Scene/SceneOps.cpp`, `Engine/Nodes/Scene/StageCache.h`/`.cpp`, `Engine/Nodes/NativeEffectBase.h` (scene-node helpers)
  - approach: each 3D node owns one anonymous `SdfLayer` holding exactly its edits, memoized keyed by (knob hash, upstream stack hash) — unchanged nodes re-emit the same layer handle, so a knob tweak at the end of a 30-node chain re-authors one layer, not thirty. Composition happens only at consumers through a bounded `StageCache` keyed by stackHash, yielding a composed read-only `UsdStage`. Time policy: animated knobs author USD time samples across the project range (re-authored on knob/range change); static knobs author defaults; vertex data is never baked. Layer authoring serialized per node; USD's TBB stays internal to USD.
  - verify: unit tests — layer handle identity across unchanged re-evaluations; StageCache hit on repeated composition; knob change invalidates exactly one node's layer.
  - size: L

## Phase 19.2: First node and the viewport

- [ ] M19.P2.T1 — `ReadScene` node
  - files: `Engine/Nodes/Scene/ReadScene.cpp`, CMake source list
  - approach: `NativeEffectBase` subclass, scene output kind; authors a layer that sublayers/references the chosen file. `.usd`/`.usda`/`.usdc`/`.usdz` natively; `.abc` via USD's Alembic file-format plugin (M19.P1.T1 builds it). Scene RoD action reports the stage's world bbox (informational).
  - verify: unit test — ReadScene on a reference USD file yields a payload whose composed stage contains the expected prims; `.abc` loads through the same path.
  - size: M

- [ ] M19.P2.T2 — `Viewport3D` panel: core-profile GL context + Storm
  - files: `Gui/Viewport3D.h`/`.cpp` (new `QOpenGLWidget`), `Gui/PanelWidget`/tab registration files, `Engine/Nodes/Scene/` (engine-side hookup)
  - approach: a **new** `QOpenGLWidget` with its own GL 4.5 core-profile context — never shares the GL-2-era `ViewerGL` context (`GPUContextPool` precedent). Renders the selected scene node's composed stage through `UsdImagingGLEngine` (Storm) via the Hydra 2.0 scene-index path. Modes: unshaded / shaded / wireframe. If a 4.5 core context can't be created, degrade to a clear error panel — never block 2D compositing.
  - verify: manual — load a USD file via ReadScene, view it in Viewport3D, scrub the timeline (time-sampled animation plays); forcing context failure shows the error panel with 2D viewer unaffected.
  - size: L

- [ ] M19.P2.T3 — Viewport picking and camera look-through
  - files: `Gui/Viewport3D.cpp`, `Gui/Viewport3D.h`
  - approach: `UsdImagingGLEngine` pick → prim selection (highlight in viewport); look-through any `UsdGeomCamera` on the stage plus a free tumble/pan/dolly camera. Kept minimal — gizmos arrive with `Transform3D` in M20.
  - verify: manual — clicking a prim highlights it; look-through a camera prim matches usdview's framing for the same file.
  - size: M

- [ ] M19.P2.T4 — Scene integration smoke test in CI
  - files: `tests/`, small reference `.usda` asset (hand-authored, tiny, committed)
  - approach: headless test — ReadScene → composed stage → prim/bbox assertions, plus the M19.P1.T2/T3 hashing and memoization tests wired into ctest. Viewport rendering is excluded (no GL 4.5 under Xvfb — see `PLAN/DECISIONS/2026-09-02-no-glx-under-xvfb.md`); viewport verification stays manual at the gate.
  - verify: test green in `build-and-test`; USD link/runtime issues surface here rather than in the GUI.
  - size: M

**Verification gate:** CI green with USD pinned and linked; ReadScene loads USD and Alembic; Viewport3D displays and scrubs a reference scene via Storm (manual check on real hardware); picking and look-through work; GL-failure path degrades gracefully; pre-existing ctest suite green; packaging (M15 tarball/AppImage) still builds with the new dependency.
