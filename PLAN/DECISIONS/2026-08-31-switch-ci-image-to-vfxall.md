# CI moves from `aswf/ci-baseqt:2027.0` to `aswf/ci-vfxall:2027-clang21.1`

2026-08-31. Supersedes the image *choice* in
`PLAN/DECISIONS/2026-08-29-pin-exact-aswf-tag.md` (that decision's method --
pin an exact published tag, verified against the releases list rather than the
changelog -- still stands and is what selected the tag below).

M1.P1.T3 picked `ci-baseqt` because it was the smallest ASWF image carrying
Qt 6.8 + PySide6, which was all M2 needed. That was the right call for M2 and
the wrong one from M3 onward: `ci-baseqt` ships **no OpenColorIO, no
OpenImageIO, no OpenFX**. The cost has already been paid once --
`PLAN/DECISIONS/2026-08-31-drop-vendored-ofx-from-ci.md` lists "`ci-baseqt`
ships no OIIO/OCIO to build one in place" as reason 2 for dropping the OFX
plugin tests, and M3.P1.T3 (swap the default OCIO config to ACES 2.0) and M11
(pre-release plugin integration test) both need those libraries present.

`ci-vfxall` is the same image family with builds of *every* ASWF package
deployed into `/usr/local` instead of just the Qt/PySide subset.

## What actually changes, verified against aswf-docker

Both images are `FROM ci-common:7-clang21` and neither adds distro packages of
its own, so:

- **RPM layer: identical.** `cairo-devel`, `wayland-devel`, `wget`, `unzip`,
  `xorg-x11-server-Xvfb`, `epel-release` all come from `ci-common`'s shared
  `scripts/common/install_yumpackages.sh`. The package table in
  `tools/ci/local/Dockerfile` carries over unchanged.
- **Toolchain: identical.** `ci-common:7` installs `gcc-toolset-14` and puts
  it ahead of Clang on `PATH`, satisfying the gcc 14.2 / glibc 2.34 target in
  `PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md`.
- **Package versions: identical.** Both images read the single `"2027"`
  `package_versions` block in aswf-docker's `versions.yaml`, so Qt 6.8.3,
  PySide 6.8.3, CPython 3.13.14, Boost 1.91.0, Expat 2.8.2 are the same
  builds of the same Conan packages. The two images cannot drift apart on a
  shared package.
- **Gained:** OpenColorIO 2.5.2, OpenImageIO 3.1.16.0, OpenEXR 3.4.15,
  OpenFX 1.5.1, Imath 3.2.3, Alembic 1.8.11, OpenVDB 13.0.0, OSL 1.15.6.0,
  MaterialX 1.39.5, OpenUSD 26.08, OpenSubdiv 3.7.0, OTIO 0.18.1, partio,
  rawtoaces, moonray.
- **Still not covered:** `extra-cmake-modules` (EPEL, needed for
  `FindWayland.cmake`) -- the known local/CI divergence documented in
  `tools/ci/local/Dockerfile` is unaffected by this switch.

Boost is not in `ci-vfxall`'s explicit package list but *is* in its Conan
dependency graph (Alembic, OpenVDB, OpenUSD and moonray all `requires` it),
so it is deployed to `/usr/local` like any other transitive dependency --
same for FreeType and HarfBuzz, which arrive via Qt. This is the one property
worth re-checking if a configure step starts failing to find a base library:
`ci-baseqt` lists those packages explicitly, `ci-vfxall` relies on transitive
deployment of the same Conan packages.

## Why this tag and not `2027.0`

There is no `aswf/ci-vfxall:2027.0`. Unlike `ci-baseqt` (plain `2027.0`,
`2027.1`), `ci-vfxall` publishes only clang-variant tags for CY2027: the
releases list shows `2027-clang21.0`, `2027-clang21.1`, `2027-clang22.0`,
`2027-clang22.1`. `ci-baseqt:2027.0` was itself built on
`ci-common:7-clang21` (`versions.yaml`: `"2027"` has
`parent_versions: ["7", "7-clang21"]`), so **`2027-clang21.1`** is the
minimal-delta choice -- same ci-common variant we have been building on, at
the latest published patch. `2027-clang22.*` also exists and is marked
`draft`; picking it would change the Clang major version alongside the image,
which is a second variable we have no reason to move right now.

## What this does not fix

The `NatronRenderer` FreeType/HarfBuzz link failure. `ci-vfxall` pulls the
identical FreeType 2.14.3 / HarfBuzz 14.2.1 packages and has the same
`/usr/local` vs `/usr/lib64` dual-library layout, so the analysis and the
CMake fix in `FREETYPE_HARFBUZZ_FINDINGS.md` (link `harfbuzz::harfbuzz`
explicitly, before `Freetype::Freetype`) are unchanged by this switch.

## Cost

Image pull time, and less than feared. **Measured after pulling both: 13.9 GB
for `ci-vfxall:2027-clang21.1` vs 12.5 GB for `ci-baseqt:2027.0` -- a 1.4 GB
delta**, not the multiple this decision originally assumed. Most of the image
is the shared `ci-common:7-clang21` base. If pull time ever does dominate the
merge gate, the answer is a Natron-specific image (an `image.yaml`-style
subset: qt, pyside, opencolorio, openimageio, openfx) rather than going back
to `ci-baseqt` and re-opening the M3/M11 blockers.

Verified in `PLAN/DECISIONS/2026-08-31-spike-ofx-plugins-on-vfxall.md`: the
`Tests` binary compiled in `ci-baseqt:2027.0` runs in `ci-vfxall` with zero
missing shared libraries, and the full suite passes there.

## Files changed

`.github/workflows/ci.yml`, `.github/workflows/nightly.yml`,
`tools/ci/local/Dockerfile`, `tools/ci/local/README.md`,
`tools/ci/local/devshell.sh` (local image tag `natron-dev:2027-clang21.1`;
existing dev containers need `devshell.sh --recreate`),
`tools/ci/local/fetch-assets.sh` (comment only).

**Landed** in `3b4d09f8b` on `milestone/m2-qt6-migration`, together with the
plugin-test restore (the two are one change: building the plugins is only
possible because of this image).
