# M19 - USD/Hydra Foundation

Full title: USD/Hydra foundation — ScenePayload, USDRead, 3D viewer mode

"M-Scene-1" in the design doc: land the USD dependency, the `ScenePayload`/
`SceneOps` transport, one input node, and Storm in the Viewer. *Proves the
dependency and the viewport before any node vocabulary is built* — M20 waits on
this milestone's gate. Requires M17 (done), M161 and M107 (see `## Decisions`).

Design authority: `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md`
("Part 2 — 3D system"). The model decision (USD as substrate, model A) is
recorded in `PLAN/DECISIONS/2026-09-05-adopt-typed-edges-and-usd-substrate.md`.
The viewport design here departs from that doc's "new 4.5 core-profile widget":
the whole GUI moves to one GL 4.5+ compatibility context instead (grilled
2026-10-24).

## Scope

**In:**
- USD pinned and linked, built against the dev image's one Python with Python bindings ON (`import pxr` works in Natron's Python); CMake asserts USD's Python matches Natron's.
- The GUI requests one global GL 4.5+ **compatibility** context, with glad regenerated to load 4.5 entry points; every existing GL path (ViewerGL, overlays, OFX interacts, OFX GL suite) keeps working unchanged.
- `ScenePayload`/`SceneOps`, per-node layer memoization, the consumer `StageCache`.
- A scene evaluation action dispatched by the render engine, modelled on `renderDeep`: node hash, abort, threading, Dot/group traversal. One time-independent payload per layer stack.
- `USDRead`: `.usd`/`.usda`/`.usdc`/`.usdz` and `.abc`; Reload knob; file mtime in the node hash; frame-offset knob; seconds-based time mapping.
- The existing Viewer node accepts scene inputs; a viewer tab shows Storm (HgiGL) when its active input is a scene and ViewerGL otherwise. Modes unshaded/shaded/wireframe, picking, camera look-through, free tumble camera following the stage's up-axis.
- An automated Storm render test under the Xvfb/llvmpipe recipe, plus a manual check on AMD and NVIDIA hardware.

**Out:**
- A bridge handing a node's composed stage to Python (Shiboken ↔ pxr object hand-off, GIL interplay with Natron's Python threads) — none.
- A manual 2D/3D toggle, and showing a scene in 2D through a camera — M20 - 3D Node Vocabulary (`USDRender`).
- Hydra rendering into an Image, and handing Storm output to Vulkan compute — M20 - 3D Node Vocabulary.
- HgiVulkan / Vulkan Storm — none.
- Moving the GUI to a GL core profile, or to QRhi/Vulkan — none.
- Up-axis or unit conversion of scene data — none (a future node).
- Gizmos and transform handles — M20 - 3D Node Vocabulary (`USDTransform`).
- MaterialX — none.

**Edge cases:**
- No GL 4.5 compatibility context available: the GUI refuses to start and names the GL version it got. NatronRenderer and ctest have no such requirement; scene nodes evaluate headless without a viewport.
- Missing, unreadable or malformed file, or an `.abc` the plug-in can't read: USDRead sets a persistent node error carrying USD's diagnostic, the scene action returns failure, and the Viewer shows that error instead of an empty scene.
- File re-exported on disk: the Reload knob calls `SdfLayer::Reload` on the file and its sublayers and bumps the node hash; mtime in the hash means reopening a project picks the change up.
- Stage `timeCodesPerSecond` ≠ project fps: timecode = (frame + offset) × tcps / projectFps — identical to 1:1 when the rates match.
- Z-up vs Y-up, cm vs m: no data conversion; the viewport's tumble camera and grid follow the composed stage's `upAxis`/`metersPerUnit`.
- Viewer input kind changes (image ↔ scene) while open: the tab switches view automatically.

**Assumptions:**
- Storm (HgiGL) renders correctly in a 4.5 compatibility-profile context on Mesa radeonsi, NVIDIA proprietary and llvmpipe (usdview has historically run that way). M19.P2.T5 proves it before anything builds on it.
- llvmpipe under the `build/deeprepro` Xvfb recipe provides a 4.5 compatibility context; otherwise the refuse-to-start rule breaks every Xvfb GUI check. M19.P2.T5 proves it.
- GUI ctest cases that build widgets without going through GUI app startup are unaffected by the refuse-to-start check.
- `aswf/ci-vfxall:2027-clang21.1` may already ship a usable USD ≥ 24.11 built against its Python; unverified (no Docker in the grilling session).
- USD's default `ArResolver` handles sublayer/reference paths relative to each layer, and `PXR_PLUGINPATH_NAME` lets pipeline resolver plug-ins load with no Natron code.
- USDRead's knobs serialize through M16 - Project Format Redesign's generic knob path with no scene-specific work.
- Unconfirmed: a Viewer whose A and B inputs are of different kinds (image vs scene) shows the A input's kind and disables wipe/compare.

## Phase 19.1: Dependency and transport

- [x] M19.P1.T1 — Pin and build USD in the toolchain and CI
  - files: `tools/ci/local/Dockerfile`, CI workflow files (`.github/workflows/`), top-level `CMakeLists.txt` (find-package wiring), `cmake/` (a USD find/check module if needed)
  - approach: USD ≥ 24.11 (boost-free, oneTBB), **monolithic** build (`libusd_ms`), **Python bindings ON**, built against the same Python the image gives Natron; MaterialX off; Alembic file-format plug-in ON. First check whether `aswf/ci-vfxall:2027-clang21.1` already carries a usable USD built against its Python and prefer consuming it; otherwise build it in the Dockerfile at a pinned exact tag — same discipline as `PLAN/DECISIONS/2026-08-29-pin-exact-aswf-tag.md` — aligned with the CY2027 target. CMake fails configure if USD's Python version differs from `Python3_VERSION`.
  - verify: CI builds link against USD; a trivial `UsdStage::Open` smoke test passes in `build-and-test`; `import pxr` succeeds from Natron's embedded Python in a test; local incremental build still works.
  - size: L

- [x] M19.P1.T2 — `ScenePayload` value type and `SceneOps` seam
  - files: `Engine/ScenePayload.h`, `Engine/Nodes/Scene/ScenePayloadImpl.cpp`, `Engine/Nodes/Scene/SceneOps.h`/`.cpp`
  - approach: immutable value — ordered `layerStack` (strongest last), combined `stackHash` (U64), metadata (up-axis, units, default prim, time codes per second). Pimpl: USD types appear only in `Engine/Nodes/Scene/` internals, never in `Engine/` core headers. `SceneOps` is the thin author-layer/compose/query interface nodes use — deliberately *not* a full abstraction over USD semantics.
  - verify: unit tests — appending a layer changes stackHash; identical stacks hash identically; `Engine/ScenePayload.h` includes no pxr header (grep-enforced in the test).
  - size: M

- [x] M19.P1.T3 — Per-node layer memoization and the consumer `StageCache`
  - files: `Engine/Nodes/Scene/SceneOps.cpp`, `Engine/Nodes/Scene/StageCache.h`/`.cpp`, `Engine/Nodes/NativeEffectBase.h` (scene-node helpers)
  - approach: each 3D node owns one anonymous `SdfLayer` holding exactly its edits, memoized keyed by (knob hash, upstream stack hash) — unchanged nodes re-emit the same layer handle, so a knob tweak at the end of a 30-node chain re-authors one layer, not thirty. Composition happens only at consumers through a bounded `StageCache` keyed by stackHash, yielding a composed read-only `UsdStage`. Time policy: animated knobs author USD time samples across the project range (re-authored on knob/range change); static knobs author defaults; vertex data is never baked. Layer authoring serialized per node; USD's TBB stays internal to USD.
  - verify: unit tests — layer handle identity across unchanged re-evaluations; StageCache hit on repeated composition; knob change invalidates exactly one node's layer.
  - size: L

- [x] M19.P1.T4 — Scene evaluation action in the render engine
  - files: `Engine/EffectInstance.h`/`.cpp` (scene action args + virtual, beside `DeepRenderActionArgs`/`renderDeep`), the engine dispatch that routes `renderDeep` today, `Engine/Nodes/Scene/SceneOps.cpp`, `Tests/` (new scene-action test)
  - approach: add a `renderScene`-style action that returns a `ScenePayload` for a node, dispatched exactly the way `renderDeep` is — node hash, abort, threading, Dot/NoOp/group pass-through of `eDataKindScene` — so no consumer walks inputs itself. The payload is time-independent: one per layer stack, not per frame; consumers pick the time when they query the composed stage. A node's failure propagates as an action failure carrying its message. Mirror the deep path's structure; no side channel.
  - verify: unit test — a test scene source behind a Dot and a group yields the same payload through the action as directly; abort mid-evaluation returns aborted; a failing source makes the action fail with its message.
  - size: L

## Phase 19.2: First node and the viewer

- [x] M19.P2.T1 — `USDRead` node
  - files: `Engine/Nodes/Scene/USDRead.cpp`, CMake source list
  - approach: `NativeEffectBase` subclass, scene output kind, implementing M19.P1.T4's action; authors a layer that sublayers/references the chosen file. `.usd`/`.usda`/`.usdc`/`.usdz` natively; `.abc` via USD's Alembic file-format plug-in (M19.P1.T1 builds it). Knobs: file, frame offset, Reload. Reload calls `SdfLayer::Reload` on the file and its sublayers and bumps the hash; the file's mtime is folded into the node hash. Time mapping: timecode = (frame + offset) × stage tcps / project fps. Open failures set a persistent node error with USD's diagnostic and fail the action. Scene RoD reports the stage's world bbox (informational).
  - verify: unit tests — a reference USD file yields a payload whose composed stage has the expected prims; `.abc` loads through the same path; a missing or malformed file fails the action and sets the node error; rewriting the file then Reload changes the payload; a 24-tcps stage in a 25-fps project maps frame 25 to timecode 24.
  - size: M

- [ ] M19.P2.T5 — Spike: Storm in a docked compatibility-profile widget
  - files: a throwaway test program under `Tests/` or `build/` (not shipped), the result recorded in this file's `## Decisions`
  - approach: with a hand-requested 4.5 compatibility `QSurfaceFormat` and a 4.5-capable loader, put a `UsdImagingGLEngine` (HgiGL) `QOpenGLWidget` docked in the same top-level window as a compat widget drawing immediate-mode GL, and render the reference `.usda`. Run it on radeonsi, NVIDIA proprietary, and llvmpipe under the `build/deeprepro` Xvfb recipe. Record, per driver, the context version received and whether both widgets draw. If Storm rejects compat or llvmpipe gives no 4.5 compat, stop and put it to the user before M19.P2.T8.
  - verify: the recorded result covers all three drivers, with a screenshot of each.
  - size: M

- [ ] M19.P2.T8 — GUI on one GL 4.5 compatibility context
  - files: `Global/gladRel/`, `Global/gladDeb/`, `Global/glad_source.c` (regenerated: `--profile=compatibility --api=gl=4.5`, current extension list), `App/NatronApp_main.cpp` or the GUI app startup (`QSurfaceFormat::setDefaultFormat`), `Gui/GuiApplicationManager*.cpp` (startup version check)
  - approach: after M19.P2.T5 passes, request a 4.5 compatibility context as the global default format before any widget exists, and regenerate glad for 4.5 compat. After the first GL context is current, check the version: below 4.5, the GUI refuses to start with a message naming the version it got. NatronRenderer and ctest skip the check.
  - verify: existing GL ctest cases pass under `build/m63-gui/run-gl-tests.sh`; the GUI under Xvfb starts and draws viewer, overlays and an OFX interact (Transform) as before; forcing a 2.1 context makes the GUI exit with the message while NatronRenderer still renders.
  - size: L

- [ ] M19.P2.T2 — Storm view inside the Viewer tab
  - files: `Gui/SceneViewport.h`/`.cpp` (new `QOpenGLWidget`), `Gui/ViewerTab.h`/`.cpp`, `Gui/ViewerTabPrivate.h`, `Gui/CMakeLists.txt`
  - approach: a `QOpenGLWidget` on the shared global compat context (M19.P2.T8) that renders a composed stage through `UsdImagingGLEngine` (Storm, HgiGL) via the Hydra 2.0 scene-index path, at the time mapped from the viewer's frame. Modes: unshaded / shaded / wireframe. The tumble/pan/dolly camera and grid follow the stage's `upAxis`/`metersPerUnit`. `ViewerTab` holds it beside `ViewerGL` and shows one or the other; M19.P2.T6 decides which. An action failure shows its message in place of the scene.
  - verify: manual — load a USD file via USDRead into a Viewer, see it in shaded/wireframe, scrub the timeline (time-sampled animation plays); a Z-up file tumbles around Z; a broken USDRead shows its error in the view.
  - size: L

- [ ] M19.P2.T6 — Viewer accepts scenes and switches view by input kind
  - files: `Engine/ViewerInstance.h`/`.cpp`, `Gui/ViewerTab.cpp`, `Tests/` (viewer kind test)
  - approach: the Viewer's inputs accept `eDataKindScene` as they accept deep today (`inputAcceptsDataKindViaAdapter`). When the active input's effective kind is scene, the viewer evaluates it through M19.P1.T4's action and the tab shows the Storm view; otherwise ViewerGL as now. It re-switches when the kind changes (reconnect, input switch, group output change). No manual 2D/3D toggle. Mixed-kind A/B: show A's kind and disable wipe (unconfirmed assumption — revisit if the user objects).
  - verify: unit test — a Viewer accepts a scene source and reports scene as its active kind; GUI test under Xvfb — connecting a USDRead then a 2D Read to the same viewer flips the visible widget each time.
  - size: M

- [ ] M19.P2.T3 — Viewport picking and camera look-through
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneViewport.h`
  - approach: `UsdImagingGLEngine` pick → prim selection (highlight in viewport); look-through any `UsdGeomCamera` on the stage plus the free tumble/pan/dolly camera. Kept minimal — gizmos arrive with `USDTransform` in M20.
  - verify: manual — clicking a prim highlights it; look-through a camera prim matches usdview's framing for the same file.
  - size: M

- [x] M19.P2.T4 — Scene integration smoke test in CI
  - files: `Tests/`, small reference `.usda` asset (hand-authored, tiny, committed)
  - approach: headless test — USDRead → scene action → composed stage → prim/bbox assertions, plus the M19.P1.T2/T3/T4 hashing, memoization and action tests wired into ctest. Viewport rendering is covered separately by M19.P2.T7.
  - verify: test green in `build-and-test`; USD link/runtime issues surface here rather than in the GUI.
  - size: M

- [ ] M19.P2.T7 — Automated Storm render test under Xvfb
  - files: `Tests/` (GL test beside the existing strict GL cases), `build/m63-gui/run-gl-tests.sh` if it needs the new case listed, the M19.P2.T4 reference `.usda`
  - approach: under the strict llvmpipe GL runner, render the reference `.usda` through `SceneViewport` into an FBO from a fixed camera, check a few pixel regions against expected coverage (tolerant, not bit-exact), and check that picking at a known pixel returns the expected prim path. Plain `xvfb-run` on the CI image has no GLX (`PLAN/DECISIONS/2026-09-02-no-glx-under-xvfb.md`), so it runs as a strict GL case and skips without GL like its siblings.
  - verify: passes under `build/m63-gui/run-gl-tests.sh` with `NATRON_TEST_REQUIRE_GL=1`; deleting the prim from the asset makes it fail.
  - size: M

**Verification gate:** CI green with USD pinned and linked and `import pxr` working in Natron's Python; USDRead loads USD and Alembic, reports errors as node errors, reloads on demand, and maps time by seconds; the GUI runs on the 4.5 compatibility context with every pre-existing GL path (viewer, overlays, an OFX interact) unchanged, and refuses to start below 4.5 while NatronRenderer still works; the Viewer switches to the Storm view for a scene input and back for an image; M19.P2.T7's Storm test passes under the strict Xvfb runner; manual check on AMD and NVIDIA hardware (display, scrub, picking, look-through), with screenshots shared before sign-off; pre-existing ctest suite green; packaging (M15 tarball/AppImage) still builds with the new dependency.

## Decisions

- 2026-10-25 — **Storm context spike: AMD and llvmpipe pass with a 4.5 compatibility request (bosley-native-nodes):** requesting a 4.5 compatibility-profile context gives 4.6 compatibility on AMD RX 7900 XTX (radeonsi, Mesa 25.2.7) and 4.5 compatibility on llvmpipe under Xvfb; on both, Storm (HgiGL) and the immediate-mode widget draw in shared contexts with zero GL errors (verdict PASS). The earlier llvmpipe run that got a core context did not set the compatibility profile correctly. The NVIDIA (RTX 3070 Ti, 580.178.04) run produced no output after building and is being re-run. Data: .local/20261025-094722-ms-E2087B87C9C8-display.tar.gz, .local/20261025-092254-ms-e2087b87c9c8-natron-gpu-xvfb.tar.gz, .local/20261025-095553-bosley-faux-display/.
- 2026-10-25 — **Bundle startup fixed (bosley-native-nodes):** linking USD moved libshiboken ahead of libpython, so shiboken's exported PyStaticMethod_New overrode CPython's during interpreter start-up and the bundled binaries segfaulted. `natron_link_embedded_python()` now links libpython first for Natron, NatronRenderer, Tests and GuiTests. The bundle stages USD's libraries, plug-ins and the pxr package (RUNPATHs reach plugin/usd); a missing USD runtime is an error, not an abort. Release build green with the bundle startup check passing.
- 2026-10-25 — **No region of definition for scene nodes yet (bosley-native-nodes):** computing a world bounding box means composing a stage on the main thread; scene nodes report none until an off-main-thread path exists.
- 2026-10-25 — **Storm context, llvmpipe result (bosley-native-nodes):** requesting a 4.5 compatibility (or 3.3) format from Qt gives a 4.5 core context, where immediate-mode GL draws nothing and Storm floods GL errors; requesting no version (as Natron does today) gives 4.5 compatibility with shared contexts, where both draw. Recommended T8 strategy: request no version and check for 4.5 compatibility at startup. Awaiting the radeonsi and NVIDIA runs of `tools/spikes/storm` before T8, T2, T3 and T7.
- 2026-10-24 — Storm runs on HgiGL, not HgiVulkan: HgiGL is USD's production-stable Hgi, and HgiVulkan would make M19 depend on M107 - GPU Viewer Hand-off's interop across two VkDevices. Handing Hydra output to Vulkan compute is `USDRender`'s concern in M20 - 3D Node Vocabulary.
- 2026-10-24 — The whole GUI moves to one GL 4.5+ compatibility context (glad regenerated), instead of a separate 4.5-core widget: compat keeps every immediate-mode path and every OFX overlay interact working, and it removes the risk of mixing profiles in one Qt window. Core profile and QRhi/Vulkan were weighed and rejected as large rewrites with little user-visible gain for 3D.
- 2026-10-24 — No GL 4.5 compatibility context means the GUI refuses to start: the user chose a hard requirement over a degraded 2D-only fallback.
- 2026-10-24 — The GL 4.5 requirement is GUI-only: NatronRenderer and ctest keep running on GPU-less farm nodes and CI.
- 2026-10-24 — The viewport gate is an automated llvmpipe Storm test plus a manual check on AMD and NVIDIA, with screenshots: llvmpipe catches regressions in CI, and real hardware proves the drivers.
- 2026-10-24 — USD is built with Python bindings ON against the image's single Python, with no stage bridge yet: the image already pins one interpreter for PySide6 and USD (usdview runs PySide6 and pxr in one process), so the bindings are cheap now. The Shiboken ↔ pxr hand-off and the GIL interplay are the real work, and they are deferred.
- 2026-10-24 — Scene evaluation is an engine action modelled on `renderDeep`, producing one time-independent payload per stack: it reuses hashing, abort and graph traversal and follows the principled-foundation direction (no `getDeepImage()`-style side channel). Time samples avoid re-authoring on scrub.
- 2026-10-24 — USDRead failures are node errors that fail the action, mirroring image Read.
- 2026-10-24 — USDRead reloads through a Reload knob, with mtime in the hash, mirroring image Read; file watching was rejected as noisy during long exports.
- 2026-10-24 — Time maps by seconds plus a frame-offset knob: identical to 1:1 when rates match, correct when they don't.
- 2026-10-24 — One Viewer node handles both 2D and 3D, picking its view from the active input's kind, with no manual toggle in M19 (user's call). This replaces the standalone `Viewport3D` panel that followed the selection.
- 2026-10-24 — Up-axis and units are honoured from stage metadata with no conversion; normalising would rewrite what the file says.
- 2026-10-24 — Deps are M17, M161, M107: fix the startup GL race (M161 - Startup Compositor Crash) before changing the context format so a regression can be attributed, and let M107 - GPU Viewer Hand-off land its ViewerGL/ViewerInstance edits before the viewer gains a scene view.
- 2026-10-24 — Scene nodes are named `USD<Whatever>` (so the old `ReadScene` becomes `USDRead`; native geometry nodes are `Geo<Whatever>`), matching `Deep<Whatever>` and `Raw<Whatever>`: user's naming rule, recorded project-wide in `PLAN/DECISIONS/2026-10-24-usd-node-naming.md`.
