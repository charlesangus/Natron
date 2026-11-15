# M441 - MoonRay MaterialX Shaders Build

Build and bundle DreamWorks' `materialx_shaders`: native C++/ISPC MoonRay map shaders, one rdl2 DSO per MaterialX nodedef (`ND_add_float`, `ND_noise3d_color3`, `ND_image_color3`, …). Upstream openmoonray added it as the `moonray/materialx_shaders` submodule in Release 2026.29.1, the release our fork is on. Our fork dropped it in `35379cc` ("Drop the unpublished materialx_shaders submodule") because its URL pointed at an unpublished `dreamworksanimation/materialx_shaders`. The repo has since moved to `OpenMoonRay/materialx_shaders`, public since 2026-11-02. Upstream's pin `9b83c0051418e09869c14e2a86abe788a7ab7dca` is that repo's HEAD. Its `ShaderStatus.md` lists 568 implemented nodedefs. The texture rows in that table are stale: 15 texture DSOs exist.

## Scope

**In:**
- The submodule restored on fork branch `natron/v2026.29.1` of charlesangus/openmoonray, at `OpenMoonRay/materialx_shaders` `9b83c00`.
- `build-moonray.sh` builds it with `-DBUILD_MATERIALX_SHADERS=ON` against the image's MaterialX, installs the DSOs in `rdl2dso/` and any library in `lib64/`, and checks that they load only image libraries.
- The deferred M325 - MoonRay Fork review cleanups to `build-moonray.sh` (recorded in M354 - MoonRay Native Shaders' Decisions), apart from dropping the flag, which this milestone turns on instead.
- The DSOs bundled in the AppImage.
- A MoonRay render of a `moonray`-context network built from `ND_*` shaders, from a file texture and a `natron://` live texture.

**Out:**
- Translating `mtlx` networks: M440 - MaterialX Translation Core, M442 - MoonRay MaterialX Backend.
- Fixing nodes upstream lists as missing (fractal2d, `separateN`, convolution): M442 - MoonRay MaterialX Backend works around or reports them.

**Edge cases:**
- The submodule's CMake wants a newer MaterialX than the image's: pin to an older upstream commit that builds, or fix it on a charlesangus fork of `materialx_shaders`. Never with a patch step in `build-moonray.sh` (M354 - MoonRay Native Shaders' Decisions, 2026-11-13).
- `ND_image_*` through MoonRay's texture path rejects `natron://`: M343 - Live Textures In Delegates' OIIO plug-in serves tiled, mip-mapped images, which should satisfy it. If it doesn't, the fix goes in the fork, and the test shows it.

**Assumptions:**
- hdMoonray instantiates each `ND_*` class by its Hydra node id with no extra mapping (`Material.cc:145`), and binds map outputs to inputs through `ValueConverter::setBinding`. P1.T4 proves it.

## Phase 441.1: Build and bundle

- [ ] M441.P1.T1 — Restore the submodule in the fork
  - files: charlesangus/openmoonray `natron/v2026.29.1` (`.gitmodules`, `moonray/materialx_shaders` gitlink), `tools/ci/local/Dockerfile`
  - approach: on the fork branch, restore the `moonray/materialx_shaders` entry dropped in `35379cc`, with URL `https://github.com/OpenMoonRay/materialx_shaders.git`, and pin it at `9b83c0051418e09869c14e2a86abe788a7ab7dca`. Push the fork and bump `NATRON_MOONRAY_COMMIT` in the Dockerfile to the new super-repository commit.
  - verify: `git ls-tree <new commit> moonray/materialx_shaders` shows the pinned gitlink, and a fresh shallow fetch of the new commit checks out the submodule.
  - size: M

- [ ] M441.P1.T2 — Build the shaders in the image
  - files: `tools/ci/local/build-moonray.sh`
  - approach: fetch the submodule with the others and set `-DBUILD_MATERIALX_SHADERS=ON`. MaterialX resolves to the image's, through the existing `natron-mtlx-aliases.cmake` include. Install the `ND_*` DSOs into `rdl2dso/` and `materialx_shaders`' library into `lib64/`, and extend the end-of-build checks so every new `.so` resolves only image or prefix libraries. While in the script, do whichever of the three remaining deferred cleanups M345 - VDB Rendering Fixes or M354 - MoonRay Native Shaders has not already done: fix the header's stale reason for not fetching materialx_shaders (~:12), reword the SOURCE text so the unpatched upstream submodules don't read as Natron forks (~:196), and delete or justify the `check_origin` comment (~:58). Rebuild and publish the dev image.
  - verify: the image build passes its checks; `ls /opt/moonray/rdl2dso | grep -c '^ND_'` is at least 500; `ldd` on three sample DSOs shows no missing libraries.
  - size: L

- [ ] M441.P1.T3 — Bundle the DSOs in the AppImage
  - files: `tools/ci/local/stage-bundle.sh`
  - approach: confirm the `moonray/` copy carries the new DSOs and `lib64` library, and add them if it copies a fixed list. Keep the bundle relocatable (RUNPATH `$ORIGIN/../lib64`).
  - verify: in an extracted AppImage, `moonray/rdl2dso` holds the same `ND_*` count as the image, and `execComp` loads `ND_noise3d_color3` in a smoke render (P1.T4's test run against the AppImage).
  - size: M

- [ ] M441.P1.T4 — Render a MoonRay network of MaterialX shaders
  - files: `Tests/Scene/USDRenderMoonRayMtlxShaders_Test.cpp`, `Tests/CMakeLists.txt`, two reference crops (≤256 px, half-float, zip)
  - approach: author a `moonray`-context material by hand in the test: `ND_noise3d_color3` into `ND_multiply_color3` into `DwaBaseMaterial`'s diffuse colour, and a second material whose `ND_image_color3` reads a file texture, then a `natron://` live texture (M343 - Live Textures In Delegates). Render a sphere through `USDRender` with MoonRay.
  - verify: gtest — both renders match their reference crops and give no delegate warning; the live-texture case updates when its source image changes.
  - size: M

**Verification gate:** release gate green per `.local/build-mode` on the rebuilt image; `USDRenderMoonRayMtlxShaders_Test` passes in the build tree and against the AppImage; the image's `ND_*` DSO count is recorded in the milestone report. User acceptance is at the UAT of F26 - MaterialX Rendering.
