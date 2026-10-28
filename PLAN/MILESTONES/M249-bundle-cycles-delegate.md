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

- [ ] M249.P1.T1 — Inspect upstream and pin a tag
  - files: `tools/ci/local/Dockerfile`, `tools/release/container/Dockerfile` (read only), a note in the report
  - approach: clone Cycles at candidate tags, read `CMakeLists.txt`, `src/hydra/` and `src/app/` to confirm the delegate exists, its CMake option, its USD version range and its dependency list. Compare against the image's libraries. Report the tag, option names, dependency conflicts, and licence. No source is committed.
  - verify: the report cites file paths and line numbers in the cloned tree for each claim and flags recollection.
  - size: M

- [ ] M249.P1.T2 — Build recipe in the image
  - files: `tools/ci/local/Dockerfile`, `tools/release/container/Dockerfile`, a build script under `tools/ci/`
  - approach: build Cycles with the Hydra delegate, CPU-only, installed under the same prefix scheme USD uses, reusing the image's USD, OIIO, OpenEXR, TBB and Embree. Pin by tag and checksum. Keep layers cacheable so a Natron-only change does not rebuild Cycles.
  - verify: image builds; `ls` of the install shows the delegate library and its plugInfo.json; `ldd` shows no duplicate OpenEXR/OIIO.
  - size: L

- [ ] M249.P1.T3 — Package and discover
  - files: `tools/ci/local/package.sh`, `tools/release/` packaging scripts, `Engine/AppManager.cpp` (plug-in path setup)
  - approach: copy the delegate, its plugInfo and runtime libraries into the AppImage and tar.xz; at start-up, append the bundled plugin dir to `PXR_PLUGINPATH_NAME` before USD loads.
  - verify: the packaged AppImage lists the Cycles renderer via the M248 - Render Delegate Selection registry; extracted bins run with `LD_LIBRARY_PATH` unset.
  - size: M

- [ ] M249.P1.T4 — Delegate load test
  - files: `Tests/Scene/CyclesDelegate_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: ctest that asks `HdRendererPluginRegistry` for the Cycles plug-in and instantiates a render delegate headless; skipped with a clear message (not failed) only when the build has no delegate (not on Linux CI).
  - verify: passes in CI debug and release.
  - size: M

**Verification gate:** CI green; the image and the AppImage contain a Cycles delegate that the registry lists and instantiates; no duplicate library versions are loaded; image-size and build-time deltas are reported to the user.

## Decisions

- 2026-10-28: implemented (build pending). Cycles standalone v5.2.0 pinned by tag+sha256 (Apache-2.0), CPU only (GPU later per the user), OSL/OpenPGL/GL display off; built by tools/ci/local/build-cycles.sh in the dev image against the image's USD 26.08/OIIO/OpenEXR/TBB/Embree/OIDN; patch drops `override` on IsSupported(bool) removed in USD 26.08. Installed into USD's plugin folder (no PXR_PLUGINPATH_NAME). stage-bundle copies OIDN's CPU module; check-startup requires the Cycles plugin to load. Estimated AppImage growth: OIDN ~+46 MB, Embree ~+8 MB compressed. Open to user: keep OIDN (denoising) or build without it.
- 2026-10-27 — **Bundle in the AppImage (user):** both delegates ship in the release, not discovery-only.
- 2026-10-27 — **CPU first (user):** GPU devices are planned for later.
