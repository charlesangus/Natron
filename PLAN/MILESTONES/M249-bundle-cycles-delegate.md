# M249 - Bundle Cycles Delegate

Build Cycles and its Hydra render delegate against the pinned USD, CPU only, and ship them
in the natron-dev image and the release AppImage so M251 - Cycles Rendering has a delegate
to discover. No Natron node code changes here; the deliverable is a delegate that loads.

## Scope

**In:**
- Cycles built from a pinned upstream tag with its Hydra delegate, against the same USD, Python and TBB the image already carries.
- CPU devices only. CUDA, HIP, OptiX, oneAPI and Metal are off (user, 2026-10-27: GPU is planned for later).
- Installed into the dev image, the CI image and the AppImage; found through `PXR_PLUGINPATH_NAME` or the install's plugInfo path.
- A ctest that the delegate is listed and loads.

**Out:**
- Rendering with it — M251 - Cycles Rendering.
- Cycles GPU devices — none (a future named milestone).
- Deep output patches — M257 - Cycles Deep Output.

**Edge cases:**
- The upstream Cycles repo lacks a Hydra delegate at the chosen tag: pick the nearest tag that has it, or stop and report; do not write a delegate.
- Cycles bundles its own copies of libraries (OIIO, OpenEXR, OpenVDB, Embree): they must link the image's copies, or be symbol-isolated, so the AppImage never loads two versions.

**Assumptions:**
- Unconfirmed: upstream Cycles (standalone repo) builds `hdCycles` behind a CMake option. Read its tree and CMake before building; mark recollection.
- Cycles and MoonRay licences (Apache 2.0) permit bundling; confirm in each repo's LICENSE.

## Phase 249.1: Build and ship

- [x] M249.P1.T1 — Inspect upstream and pin a tag
  - files: `tools/ci/local/Dockerfile`, `tools/release/container/Dockerfile` (read only), a note in the report
  - approach: clone Cycles at candidate tags, read `CMakeLists.txt`, `src/hydra/` and `src/app/` to confirm the delegate exists, its CMake option, its USD version range and its dependency list. Compare against the image's libraries. Report the tag, option names, dependency conflicts, and licence. No source is committed.
  - verify: the report cites file paths and line numbers in the cloned tree for each claim and flags recollection.
  - size: M

- [x] M249.P1.T2 — Build recipe in the image
  - files: `tools/ci/local/Dockerfile`, `tools/release/container/Dockerfile`, a build script under `tools/ci/`
  - approach: build Cycles with the Hydra delegate, CPU-only, installed under the same prefix scheme USD uses, reusing the image's USD, OIIO, OpenEXR, TBB and Embree. Pin by tag and checksum. Keep layers cacheable so a Natron-only change does not rebuild Cycles.
  - verify: image builds; `ls` of the install shows the delegate library and its plugInfo.json; `ldd` shows no duplicate OpenEXR/OIIO.
  - size: L

- [x] M249.P1.T3 — Package and discover
  - files: `tools/ci/local/package.sh`, `tools/release/` packaging scripts, `Engine/AppManager.cpp` (plug-in path setup)
  - approach: copy the delegate, its plugInfo and runtime libraries into the AppImage and tar.xz; at start-up, append the bundled plugin dir to `PXR_PLUGINPATH_NAME` before USD loads.
  - verify: the packaged AppImage lists the Cycles renderer via the M248 - Render Delegate Selection registry; extracted bins run with `LD_LIBRARY_PATH` unset.
  - size: M

- [x] M249.P1.T4 — Delegate load test
  - files: `Tests/Scene/CyclesDelegate_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: ctest that asks `HdRendererPluginRegistry` for the Cycles plug-in and instantiates a render delegate headless; skipped with a clear message (not failed) only when the build has no delegate (not on Linux CI).
  - verify: passes in CI debug and release.
  - size: M

**Verification gate:** CI green; the image and the AppImage contain a Cycles delegate that the registry lists and instantiates; no duplicate library versions are loaded; image-size and build-time deltas are reported to the user.

## Phase 249.2: Deep-capable Cycles source (user, 2026-11-03)

- [ ] M249.P2.T1 — Build Cycles from Niik-l/cycles feature/cycles-deep
  - files: tools/ci/local/build-cycles.sh, tools/ci/local/Dockerfile, tools/ci/local/dev-image-tag.sh (hash input), licence notes
  - approach: replace the blender/cycles v5.2.0 tag archive with a pinned commit archive of https://github.com/Niik-l/cycles branch feature/cycles-deep (3a20cc0b at 2026-10-24); new sha256; confirm both build-time patches (IsSupported override, plugInfo priority 99) still apply; confirm the base Cycles version and that it builds hdCycles against USD 26.08, OIIO, OpenEXR, TBB, Embree, OIDN (kept, user); record the fork's licence (Apache 2.0 upstream) in the bundle notes.
  - verify: the dev image builds on CI with the new pin; the Cycles delegate tests (M249, M251) pass; the startup check registers hdCycles.
  - size: M

## Decisions

- 2026-10-28: Storm tests failed with Cycles installed: the GL engine path never set rendererPluginId, so USD picked its default, and Cycles (priority 0, name sorts before HdStormRendererPlugin) won the tie. Every Hydra engine now names its renderer; Cycles' plugInfo priority patched 0 → 99 (USD defaults to the LOWEST priority); a test checks Storm leads the renderer list.
- 2026-10-28: stacked on M248 - Render Delegate Selection: once hdCycles is installed, USD's plugin priority picks Cycles when no renderer id is given, so every Storm path must pass Storm's id explicitly (M248 does). The libjpeg .62/.8 pair is allowed in build-cycles.sh's duplicate check (system libtiff; Natron already ships it).
- 2026-10-28: implemented (build pending). Cycles standalone v5.2.0 pinned by tag+sha256 (Apache-2.0), CPU only (GPU later per the user), OSL/OpenPGL/GL display off; built by tools/ci/local/build-cycles.sh in the dev image against the image's USD 26.08/OIIO/OpenEXR/TBB/Embree/OIDN; patch drops `override` on IsSupported(bool) removed in USD 26.08. Installed into USD's plugin folder (no PXR_PLUGINPATH_NAME). stage-bundle copies OIDN's CPU module; check-startup requires the Cycles plugin to load. Estimated AppImage growth: OIDN ~+46 MB, Embree ~+8 MB compressed. Open to user: keep OIDN (denoising) or build without it.
- 2026-10-27 — **Bundle in the AppImage (user):** both delegates ship in the release, not discovery-only.
- 2026-10-27 — **CPU first (user):** GPU devices are planned for later.
- 2026-11-02 — Audit: all tasks committed (680e291ca, fa6ddbbcb, 80e44ed97) and pushed; worktree clean; no PR; 23 behind origin/main. The branch still carries M248's pre-squash commits (M248 merged as #152), so merging origin/main in needs the squash-stack handling. The OIDN keep/drop question to the user is still open (audit).
- 2026-11-02 — OIDN is bundled (user: "yes please").
- 2026-11-03 — **OIDN stays in the Cycles build (user).** **Cycles comes from the deep-capable fork/branch (user, 2026-11-03):** build the bundled Cycles from the branch with deep image support, as the Niik-l fork did, so M257 - Cycles Deep Output can add deep later without re-plumbing (investigating the current pin).
- 2026-11-03 — Fork created (user): https://github.com/charlesangus/cycles (parent blender/cycles); branch natron/deep starts at Niik-l feature/cycles-deep 3a20cc0b (Cycles 5.0.0 base, 7 commits, +2957/-21). Analysis: rebasing onto v5.2.0 conflicts in 8 files; the 5.0 base lacks upstream USD 25.11+ fixes (backported in the draft build script). PM call: rebase natron/deep onto v5.2.0 (keeps upstream Hydra/USD fixes, fewer backports). Licence: two deep files are Blender GPL-2.0-or-later code, so the bundled hdCycles is GPL; with Apache-2.0 Cycles that requires distribution under GPLv3 (flagged to the user).
- 2026-11-03 — natron/deep-5.2 pushed to charlesangus/cycles (c8d449e6): Niik-l's 5 deep/lens commits cherry-picked (-x) onto v5.2.0; MinGW commits 9e906ae and 2850378 dropped (9e906ae also flipped a primitive_volume_attribute flag and added volume flags in scene/shader.cpp, now v5.2.0 behaviour). Static checks only; KernelFilm layout hand-checked (448 bytes). Risks: GPU texture-cache retries could double-write deep volume samples; CPU kernel-globals ownership changed in 5.2 (needs a CPU deep render). M249 build being pointed at it (no USD backports needed on the 5.2 base). natron/deep stays at 3a20cc0 until the build proves deep-5.2.
