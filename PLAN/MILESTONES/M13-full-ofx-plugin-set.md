# Milestone 13: Build the full upstream OFX plugin set

Upstream Natron bundles four OFX plugin repositories. This fork builds two —
`openfx-io` and `openfx-misc`, from pinned source per
`DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md`. This milestone adds
**openfx-arena**. **openfx-gmic is deferred**, not for want of dependencies but
because its pinned source no longer exists anywhere the build can reach — see
`DECISIONS/2026-09-02-openfx-gmic-source-unavailable.md`.

A scout proved the arena path end to end inside `aswf/ci-vfxall:2027-clang21.1`
on 2026-09-02 rather than estimating it: ImageMagick, lcms2, libzip and arena
all built, and `dlopen` + `OfxGetNumberOfPlugins` on the installed bundle
returned **22 plugins**. The numbers below are measured, not projected.

`tools/ci/local/fetch-assets.sh` is the pattern to mirror throughout: exact
commit SHAs (never tags or branches), `clone_at_ref()` for raw-SHA fetches with
recursive submodules, `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` because CMake 4.x
rejects the `cmake_minimum_required(3.1)` these projects declare, each project's
own `install` target emitting the bundle, and plugin IDs verified with `strings`
to a file then `grep -Fxq` — deliberately not a pipeline, since `grep -q` under
`pipefail` gives false negatives via SIGPIPE.

## Phase 13.1: Vendor the ImageMagick dependency chain

- [x] M13.P1.T1 — Build lcms2 and libzip from pinned source
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: add a `DEPS_PREFIX="${PLUGINS_SRC_DIR}/deps-install"`, then build
    `mm2/Little-CMS` at the `lcms2.16` SHA (autotools, `--disable-static`) and
    `nih-at/libzip` at the `v1.11.4` SHA (CMake, with `BUILD_TOOLS`,
    `BUILD_REGRESS`, `BUILD_EXAMPLES`, `BUILD_DOC` and the BZIP2/LZMA/ZSTD
    options all `OFF`). libzip is absent from the image entirely. lcms2 is
    present as `/usr/local/lib/liblcms2.so.2.0.19` with a header but **no
    `.pc`**, and arena calls `pkg_search_module(LCMS2 REQUIRED lcms2)` — build
    it rather than hand-write a `.pc` for a library we do not control. Extend
    `PLUGINS_WANT` with both pins so the stamp invalidates correctly.
  - verify: `PKG_CONFIG_PATH=$DEPS_PREFIX/lib/pkgconfig pkg-config --modversion
    lcms2 libzip` prints `2.16` and `1.11.4`; a second run hits the stamp and
    compiles nothing.
  - size: S

- [x] M13.P1.T2 — Build ImageMagick 7.1.1-6 with Q32/HDRI
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: fetch `ImageMagick/ImageMagick` at the `7.1.1-6` SHA. Upstream's
    own flags no longer live in this tree — M3 deleted `tools/jenkins/`;
    recover them with `git show
    576c2078^:tools/jenkins/include/scripts/pkg/imagemagick7.sh` and start from
    that `./configure` line (`--with-quantum-depth=32 --enable-hdri
    --with-magick-plus-plus=yes --without-modules --enable-zero-configuration
    --without-x --disable-static --enable-shared --with-lcms --with-png
    --with-zlib --with-freetype --with-fontconfig` plus its `--without-*` list,
    and `CFLAGS/CXXFLAGS=-DMAGICKCORE_EXCLUDE_DEPRECATED=1`).
    **Q32/HDRI is a correctness requirement, not a
    packaging preference:** every Magick plugin round-trips OFX float buffers
    through `Magick::FloatPixel`, so a non-HDRI build silently clips linear
    values. Two container-specific deltas, each needing a comment saying why:
    `--without-jxl`, because the image's libjxl changed
    `JxlDecoderGetColorAsICCProfile`'s signature and `coders/jxl.c` will not
    compile against it; and `LDFLAGS="-L/usr/local/lib
    -Wl,-rpath,/usr/local/lib" CPPFLAGS="-I/usr/local/include/freetype2"`,
    because the image carries FreeType 2.10.4 at `/usr/lib64` and 2.13.x at
    `/usr/local`, `pkg-config freetype2` resolves to the former, and
    `/usr/local/lib/libharfbuzz.so.0` needs the latter's `FT_Get_Paint*`. That
    FreeType split will bite any future font-touching dependency in this image.
  - verify: `pkg-config --modversion Magick++` prints `7.1.1`; `magick -version`
    reports `Q32 HDRI`. Measured ~3m36s at `-j4`, ~35 MB installed.
  - size: M

**Verification gate:** from an empty `build/assets`, `fetch-assets.sh` yields a
prefix where `pkg-config --exists Magick++ lcms2 libzip` succeeds and a second
run is a no-op. No bundle has changed yet, so `test.sh ctest debug` is still
28/28.

## Phase 13.2: Ship openfx-arena

- [x] M13.P2.T1 — Fork arena and gate the unbuildable readers behind options
  - landed: `charlesangus/openfx-arena` PR #1, merged as
    `d29ac7f180e1ea5cca2b8c0492686638836b605c` — this is the SHA `M13.P2.T2`
    pins.
  - files: `charlesangus/openfx-arena` (`CMakeLists.txt`) — out of tree, per
    `DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md`
  - approach: fork `NatronGitHub/openfx-arena` at
    `49e4d5fc197a637196c1a7decab7f97751b867f6`. Add `WITH_SVG`, `WITH_PDF` and
    `WITH_CDR`, each defaulting **`ON`** so upstream behaviour is unchanged and
    the patch stays offerable upstream. Each gates both its
    `pkg_search_module(...)` call and the matching `Extra/Read{SVG,PDF,CDR}.cpp`
    entry in the source list. Leave `OpenRaster.cpp` and `ReadKrita.cpp` alone —
    they need only libzip, libxml2 and the bundled lodepng, all available. Keep
    the existing `INSTALL_RPATH "$ORIGIN/../../Libraries"`.
  - verify: with all three `OFF`, CMake configures on a box lacking librsvg,
    poppler, libcdr and librevenge; with all three `ON` and those present, the
    generated source list is byte-identical to upstream's.
  - size: S

- [x] M13.P2.T2 — Build Arena.ofx and stage its private libraries
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: `clone_at_ref()` the fork at an exact SHA with recursive
    submodules (`OpenFX`, `SupportExt`, `OpenFX-IO`, `lodepng`). Configure with
    `-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH=/usr/local
    -DWITH_SVG=OFF -DWITH_PDF=OFF -DWITH_CDR=OFF` and `PKG_CONFIG_PATH` pointing
    at Phase 13.1's prefixes. **Use the CMake path, not `Makefile.master`** —
    the Makefile runs `pkg-config OpenColorIO` unconditionally, and this image
    ships OCIO as a CMake config package with no `.pc` at all. After install,
    copy `libMagick{Core,Wand,++}-7.Q32HDRI.so.*`, `liblcms2.so.2*` and
    `libzip.so.5*` into `Arena.ofx.bundle/Contents/Libraries/`; the binary
    already carries `RUNPATH $ORIGIN/../../Libraries`, so the bundle becomes
    self-contained with no `LD_LIBRARY_PATH` anywhere — the same property the
    static-SeExpr choice bought for `IO.ofx`.
  - verify: `ldd` on the installed `.ofx` reports nothing `not found` with no
    environment set; extend the existing `strings`-to-file plus `grep -Fxq`
    check to assert `net.fxarena.openfx.Text`, `net.fxarena.openfx.ReadPSD` and
    `fr.inria.openfx.ReadMisc`. Measured ~3m14s at `-j4`, 5.3 MB `.ofx`, 22
    plugins.
  - size: M

- [ ] M13.P2.T3 — Record the arena decision and update the licence manifest
  - files: `PLAN/DECISIONS/2026-09-02-build-openfx-arena.md`,
    `PLAN/DECISIONS/INDEX.md`, `tools/license/components/README.md`,
    `tools/license/components/LICENSE-libzip.txt`
  - approach: record why Q32/HDRI is mandatory, why `--without-jxl`, why the
    FreeType override, why the CMake path over the Makefile path, and which four
    plugins are absent and what each would take. Add the missing licence entry
    for libzip (BSD-3-Clause); ImageMagick and lcms2 are already listed.
  - verify: the decision explains every non-obvious flag the script now carries;
    the manifest lists every library linked into a shipped bundle.
  - size: S

**Verification gate:** a cold `fetch-assets.sh` emits
`build/assets/Plugins/Arena.ofx.bundle` exposing 22 plugins; `test.sh ctest
debug` and `test.sh smoke debug` both still pass.

## Phase 13.3: Prove the bundle in the test suite

- [ ] M13.P3.T1 — Assert an arena plugin loads through the Natron host
  - files: `Tests/BaseTest.cpp`, `Tests/BaseTest.h`
  - approach: `BaseTest::SetUp` asserts exactly three plugin IDs today via
    `appPTR->getPluginBinary`. Add `net.fxarena.openfx.Text` — it needs no
    external file and exercises the fontconfig/pango path that the FreeType
    workaround in `M13.P1.T2` exists to make work. Loading is the claim; do not
    add a render case here.
  - verify: `test.sh ctest debug` green; removing `Arena.ofx.bundle` fails
    exactly the new assertion, with a message naming the missing plugin.
  - size: S

- [ ] M13.P3.T2 — Check the expected bundle set in the smoke test
  - files: `tools/ci/smoke_test.py`, `tools/ci/verify_plugin_loads.cpp`
  - approach: assert the bundle set under `OFX_PLUGIN_PATH` (`IO`, `Misc`,
    `CImg`, `Arena`) rather than enumerating plugin IDs. The failure this
    catches is a silently-skipped assets step, which the cache makes possible.
    `verify_plugin_loads.cpp` is already the `dlopen` +
    `OfxGetNumberOfPlugins`/`OfxGetPlugin` probe this needs, and M12 already
    moved it out of `.github/workflows/` to `tools/ci/`; wire
    `fetch-assets.sh` to build and run it rather than leaving it unreferenced.
  - verify: `test.sh smoke debug` passes; removing any one bundle directory
    fails it by name.
  - size: S

**Verification gate:** `ctest` and the smoke test both fail loudly when
`Arena.ofx.bundle` is missing or unloadable, and pass otherwise, with CI green
end to end.

## Phase 13.4: Reconcile the documentation with what ships

- [ ] M13.P4.T1 — Remove the pages for arena plugins that do not ship
  - files: `Documentation/source/plugins/net.fxarena.openfx.{ReadSVG,AudioCurve}.rst`,
    `Documentation/source/plugins/fr.inria.openfx.{ReadPDF,ReadCDR}.rst`, their
    `.png` assets, `Documentation/source/_groupExtra.rst`
  - approach: after Phase 13.2, 21 of the 25 documented arena plugins are real
    shipped capability — which is what `M12.P2.T4` was cancelled to preserve.
    Four are not: `ReadSVG` and `ReadCDR` (librevenge and libcroco have no
    reachable source), `ReadPDF` (poppler deliberately excluded, see this
    milestone's `## Decisions`), and `AudioCurve` (sox,
    optional and off in upstream's own bundle). Delete those four pages. They
    are generated artifacts — `tools/genStaticDocs.sh:43` deletes and
    regenerates `source/plugins/` wholesale — so they come back automatically if
    the plugins ever do.
  - verify: every remaining `net.fxarena.*` and arena `fr.inria.*` page
    corresponds to an ID in the shipped bundle's `dlopen` output.
  - size: S

- [ ] M13.P4.T2 — Remove the 360 gmic reference pages
  - files: `Documentation/source/plugins/eu.gmic.*` (360),
    `Documentation/source/_groupGMIC.rst`, `Documentation/source/index.rst`
  - approach: half of `Documentation/source/plugins/` documents a bundle this
    fork cannot currently build. Leaving it is exactly the failure M12 exists to
    stop — pages that read as authoritative while describing capability the tree
    does not have. Delete them, with `DECISIONS/2026-09-02-openfx-gmic-source-unavailable.md`
    recording why and what would restore them. As generated artifacts they
    regenerate from `genStaticDocs.sh` if gmic is ever unblocked; nothing is
    lost that a build would not recreate.
  - verify: no page under `Documentation/source/plugins/` describes a plugin the
    shipped bundles do not expose; no toctree or `:ref:` dangles.
  - size: S

**Verification gate:** every page under `Documentation/source/plugins/`
corresponds to a plugin ID the shipped bundles actually export.

## Decisions

- 2026-09-02 — `M13.P2.T2`'s brief named the wrong staging directory. It said
  `Arena.ofx.bundle/Contents/Libraries/`, but the binary's `$ORIGIN` is
  `Contents/Linux-x86-64`, so `$ORIGIN/../../Libraries` resolves to
  `Arena.ofx.bundle/Libraries` — one level higher. Confirmed with `LD_DEBUG=libs`
  and by `ldd` with `LD_LIBRARY_PATH` cleared, where all five staged libraries
  resolve inside the bundle and nothing is `not found`.

- 2026-09-02 — `fr.inria.openfx.ReadMisc` is asserted through the `dlopen` probe
  rather than the `strings` + `grep -Fxq` pattern used for the other plugin IDs.
  At `-Ofast` GCC builds that 24-byte identifier from a 16-byte `.rodata` prefix
  shared with the other `fr.inria.openfx.` IDs plus an 8-byte `movabs` immediate
  in the code, so it never exists as one contiguous run for `strings` to find.
  The probe sees what a real host sees, which is the stronger claim anyway.

- 2026-09-02 — freshness check at promotion (PLAN-FORMAT.md §5a) corrected three
  stale references without re-planning: `tools/jenkins/` was deleted by M3, so
  `M13.P1.T2` now recovers ImageMagick's upstream configure flags from git
  history; `verify_plugin_loads.cpp` already moved to `tools/ci/` in M12, so
  `M13.P3.T2` no longer asks for the move; and `M13.P4.T1` pointed at a poppler
  decision file that was never written — the rationale is in this section.

- 2026-09-02 — openfx-gmic is deferred rather than unblocked. Its dependency
  list is small and fftw3 is reachable; the blocker is that G'MIC 2.8.4's
  tarball exists nowhere the sealed network can reach, and upstream's own
  Makefile records that bumping the version breaks the plugin ("shows 0
  plugins"). Mirroring the tarball or widening the egress allowlist would both
  work and were considered; deferring keeps this milestone to work that is
  proven. See `DECISIONS/2026-09-02-openfx-gmic-source-unavailable.md`.

- 2026-09-02 — `ReadPDF` is excluded, so poppler is not linked. Poppler is
  GPL-2.0 and would make `Arena.ofx` GPL-2.0 outright; upstream ships a
  `LICENSE=COMMERCIAL` switch specifically to avoid that. Natron is already
  GPL-2.0 so nothing breaks today, but packaging is still deferred
  (`DECISIONS/2026-08-29-defer-packaging-decision.md`) and this keeps the
  options open. It also sidesteps upstream issue #23, still open: `ReadPDF.cpp`
  includes poppler's private `GlobalParams.h`, which needs C++17 while arena
  pins C++11.

**Verification gate:** `build/assets/Plugins/Arena.ofx.bundle` is built from
pinned source by a cold `fetch-assets.sh` and exposes 22 plugins; `ctest` and
the smoke test both fail when it is absent and pass when it is present; CI is
green; and no plugin reference page describes capability the shipped bundles do
not have.
