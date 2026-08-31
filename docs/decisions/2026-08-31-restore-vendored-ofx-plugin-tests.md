# Keep the OFX plugin tests: build the bundle, don't download it

2026-08-31. **Supersedes `PLAN/DECISIONS/2026-08-31-drop-vendored-ofx-from-ci.md`.**
`Tests/BaseTest.cpp`, the plugin fetch, and the `OFX_PLUGIN_PATH` plumbing all
stay. M9 (drop vendored OFX) is cancelled; M11 (plugin integration test) is
largely delivered by this change.

The drop decision rested on "it cannot work in the target container." That was
true of `aswf/ci-baseqt:2027.0`, and it is not a property of the tests -- it
was a consequence of the image choice made back in M1.P1.T3, when `ci-baseqt`
was picked for Qt/PySide alone. `PLAN/DECISIONS/2026-08-31-switch-ci-image-to-vfxall.md`
moved CI to `aswf/ci-vfxall:2027-clang21.1`, which ships OpenColorIO 2.5.2,
OpenImageIO 3.1.16.0, OpenEXR 3.4.15, OpenFX 1.5.1 and LibRaw 0.22.2. The
spike in `PLAN/DECISIONS/2026-08-31-spike-ofx-plugins-on-vfxall.md` then proved
the tests pass there. Reversing the drop is the honest consequence.

## What changed in the repo

`tools/ci/local/fetch-assets.sh` no longer downloads
`openfx-io-build-ubuntu_22-testing.zip`. It builds the bundle:

- **openfx-io** pinned at `59318530ed1c6b78e6a85dc7c4cf60366520ba7f` on our
  fork `charlesangus/openfx-io` (two commits ahead of upstream, zero behind;
  the delta is the SeExpr CMake fix), built against the image's own
  OIIO/OCIO/OpenEXR/LibRaw. `cmake --install` already
  emits the `IO.ofx.bundle/Contents/{Linux-x86-64,Resources,Info.plist}`
  layout `OFX_PLUGIN_PATH` expects, so there is no hand-assembly.
- **wdas/SeExpr** pinned at `a5f02bb03199630759b0b94a64f37ce56c08675a`
  (branch `v1-2.11` -- the header layout openfx-io's `SeNoise.cpp` targets),
  built **statically with `POSITION_INDEPENDENT_CODE`**. ASWF ships no SeExpr
  at any platform year, and `SeNoise` is one of the three IDs `BaseTest`
  asserts. Static linking is the load-bearing choice here: the resulting
  `IO.ofx` has no `libSeExpr` in `DT_NEEDED`, so neither `test.sh` nor either
  workflow needs an `LD_LIBRARY_PATH`, and the bundle is relocatable.
- Both pins are exact SHAs, not branches: a test fixture that moves under you
  turns an unrelated upstream commit into a mystery failure on someone's PR.
- The script verifies the built bundle exports all three asserted IDs and
  fails loudly if not -- a build that silently drops `ReadOIIO` would
  otherwise surface as BaseTest's much vaguer "Couldn't find a plugin".
- A stamp file (`Plugins/.natron-plugin-pins`) makes re-runs instant, and both
  workflows gained a `Cache test assets` step keyed on `fetch-assets.sh`'s
  hash, so CI pays the compile only when a pin or the recipe changes.

`ci.yml`, `nightly.yml` and `test.sh` needed no other change: they already
call `fetch-assets.sh` and already point `OFX_PLUGIN_PATH` at
`build/assets/Plugins`.

## Verified

Clean run in `aswf/ci-vfxall:2027-clang21.1`, real scripts, no manual steps:

- `fetch-assets.sh` from an empty `build/assets/Plugins`: builds SeExpr, then
  openfx-io, then installs and validates the bundle.
- Second run: `[Plugins] already built at the pinned refs -- skipping`, 0.02s.
- `tools/ci/local/test.sh ctest debug`: **`100% tests passed`**, and inside it
  `28 tests from 14 test cases ran. [ PASSED ] 28 tests.` -- `BaseTest.GenerateDot`,
  `BaseTest.SetValues` and `BaseTest.SimpleNodeConnections` all green.

That is 28/28 against the 25/25 the drop decision would have produced: the
three `BaseTest` cases are the difference, and they now pass rather than being
deleted.

## The one reason that survived, and why we are overriding it

The drop decision's reason 1 -- "it tests someone else's binaries" -- is not
factually wrong the way its other two were, but it no longer describes what
this is. We are no longer asserting facts about a downloaded binary; we build
a known plugin from a pinned source revision against our own container and
check that Natron's OFX host loads it and instantiates nodes from it. That is
a test of *our* host implementation, using a plugin as the fixture. Given the
Qt6 migration touches exactly the machinery that loads and drives plugins,
deleting the only test that exercises it was the wrong trade.

## Known gaps

- **No FFmpeg in the bundle.** ASWF ships no ffmpeg, so `ReadFFmpeg` and
  `WriteFFmpeg` are absent. Nothing in the suite needs them today; covering
  video I/O means sourcing ffmpeg first.
- ~~An upstream bug is worked around, not fixed.~~ **Resolved same day.**
  The openfx-io `SEEXPR2_INCLUDES`/`SEEXPR2_LIBRARIES` variable-name bug is
  fixed on our fork and we now pin it; the two extra `-D` flags are gone from
  `fetch-assets.sh`. See
  `PLAN/DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md`.
- **`cmake_minimum_required(VERSION 3.1)`** in both projects needs
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` under the image's CMake 4.x. Harmless,
  but it will need revisiting if CMake drops that escape hatch.

**Landed** in `3b4d09f8b` on `milestone/m2-qt6-migration`.
