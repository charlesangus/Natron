# M250 - Bundle MoonRay Delegate

Build DreamWorks' MoonRay and its Hydra delegate (hdMoonray) against the pinned USD, CPU
only, and ship them in the dev image and the AppImage. MoonRay has a far larger dependency
set than Cycles, so this milestone is mostly dependency work; no Natron node code changes.

## Scope

**In:**
- MoonRay and hdMoonray built from a pinned tag against the image's USD, TBB and Python, on the Rocky 9 / gcc 14 baseline.
- Its non-system dependencies built or reused (expect Embree, ISPC, OpenVDB, Imath/OpenEXR, OIIO, Boost or its replacements, the Arras/Moonlight stack and Qt-free tools): read the repo's own build docs for the list.
- CPU only (user, 2026-10-27); the XPU/GPU path stays off.
- Installed into the image and the AppImage; delegate listed and loadable.

**Out:**
- Rendering with it — M252 - MoonRay Rendering.
- GPU/XPU — none (a future named milestone).
- Distributed rendering through Arras — none.
- Deep output — M258 - MoonRay Deep Output.

**Edge cases:**
- A MoonRay dependency conflicts with the image's version (Boost, OpenVDB, TBB): isolate it in a private prefix with rpath, so the Natron process still loads one copy of each public library.
- The dependency build exceeds a reasonable image-build time: stop and report numbers to the user before continuing.
- The delegate needs a MoonRay runtime (`rdl2` DSOs, shader library) at a fixed path: relocate it with the install and set the search path at load.

**Assumptions:**
- Unconfirmed: hdMoonray lives in the openmoonray tree (`moonray/hydra/hdMoonray`) and builds as part of the main CMake. Verify against the repo; mark recollection.
- MoonRay builds with the image's compiler. If it needs a different one, report.

## Phase 250.1: Build and ship

- [ ] M250.P1.T1 — Inspect upstream and inventory dependencies
  - files: report only (no commits)
  - approach: read openmoonray's top-level CMake, build docs and `hydra` tree at candidate tags. Produce: the tag, the full dependency list with versions, which the image already has, which conflict, the hdMoonray CMake option, the USD version it targets, the runtime layout and the licence.
  - verify: each claim cites file and line in the cloned tree; recollection flagged.
  - size: M

- [ ] M250.P1.T2 — Build dependencies in the image
  - files: `tools/ci/local/Dockerfile`, `tools/release/container/Dockerfile`, a build script under `tools/ci/`
  - approach: add the missing dependencies in cacheable layers in a private prefix, pinned by tag and checksum; reuse the image's copies wherever the versions fit.
  - verify: image builds; dependency prefix lists the libraries from the inventory; Natron's own build is unaffected (no new public library shadows an image one).
  - size: L

- [ ] M250.P1.T3 — Build MoonRay and hdMoonray
  - files: the same Dockerfiles and build script
  - approach: CPU-only configure with the XPU/GPU options off; build moonray, its DSOs and the delegate against the image's USD; install with a relocatable layout.
  - verify: `ls` of the install shows the delegate library, plugInfo.json and the rdl2 DSOs; `ldd` shows no unresolved or duplicate libraries.
  - size: L

- [ ] M250.P1.T4 — Package and discover
  - files: `tools/ci/local/package.sh`, `tools/release/` packaging scripts, `Engine/AppManager.cpp`
  - approach: ship delegate, DSOs and private libraries in the AppImage and tar.xz with rpaths; at start-up append the bundled plugin dir to `PXR_PLUGINPATH_NAME` and set MoonRay's DSO search path.
  - verify: the packaged AppImage lists MoonRay in the M248 - Render Delegate Selection registry; extracted bins run with `LD_LIBRARY_PATH` unset.
  - size: M

- [ ] M250.P1.T5 — Delegate load test
  - files: `Tests/Scene/MoonRayDelegate_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: ctest that finds the MoonRay plug-in and instantiates its render delegate headless.
  - verify: passes in CI debug and release.
  - size: M

**Verification gate:** CI green; image and AppImage contain a MoonRay delegate that is listed and instantiates; no duplicate public libraries load; image-size and build-time deltas are reported to the user.

## Decisions

- 2026-10-28: Implemented (image build + gate pending). openmoonray v2026.29.1 (Apache-2.0) built CPU-only into /opt/moonray (CUDA/OptiX off; ISPC v1.21.0 downloaded for the build only; -DUDPSYSLOG_NO_BOOST). Dropped: distributed Arras, sdr plug-ins (USD 26.08 removed Ndr), Houdini plug-ins, moonshine_usd, profile viewer, upstream tests. hdMoonray's removed IsSupported override stripped by the build script. Plug-in priority patched 99 → -1 so Storm stays the default. MoonRay is compiled -march=core-avx2: registered only on AVX2 CPUs. Registered directly at start-up (PXR_PLUGINPATH_NAME is read once at USD load, too late); MoonRay dirs first on RDL2_DSO_PATH/ARRAS_SESSION_PATH/PATH. Debug delegate in the image, not the bundle. Estimate +60–100 MB compressed AppImage. Will conflict with M249 in stage-bundle.sh and check-startup.sh (both add an OpenImageDenoise block).
- 2026-10-27 — **Bundle in the AppImage (user):** MoonRay ships in the release, accepting its heavy dependency set.
- 2026-10-27 — **CPU first (user):** XPU/GPU deferred.
