# Spike: the vendored OFX plugin tests DO work on `ci-vfxall` — if we build the plugins

2026-08-31. Empirical spike run in `aswf/ci-vfxall:2027-clang21.1` (pulled via
`mirror.gcr.io`), against the existing `build/debug` tree on
`milestone/m2-qt6-migration`. Result:

> **`Tests` goes 28/28 green with `BaseTest` intact**, including all three
> plugin IDs it asserts, when `IO.ofx` is built from source in the container
> instead of downloaded as a prebuilt Ubuntu 22 binary.

This reopens `PLAN/DECISIONS/2026-08-31-drop-vendored-ofx-from-ci.md` and the
whole M9/M11 split. Two of that decision's three reasons do not survive.

## What was actually run

1. Pulled `aswf/ci-vfxall:2027-clang21.1`. **13.9 GB** vs ci-baseqt's 12.5 GB
   -- a 1.4 GB delta, far smaller than assumed when the switch was made.
2. Confirmed the *existing* `Tests` binary (compiled in `ci-baseqt:2027.0`)
   runs in `ci-vfxall` with **zero missing shared libraries** -- direct
   evidence the image swap is ABI-compatible for our own artifacts.
3. Built `openfx-io` (HEAD `1bce1f1`, 2026-07-24) from source in the
   container. Configure found OIIO 3.1.16.0, OCIO 2.5.2, OpenEXR 3.4.15,
   LibRaw 0.22.2, PNG 1.6.37 -- all from the image. Build exit 0, warnings
   only. Produced a 2.3 MB `IO.ofx` whose `DT_NEEDED` list is the container's
   own `libOpenColorIO.so.2.5`, `libOpenImageIO.so.3.1`,
   `libOpenEXR-3_4.so.33`, `libraw_r.so`. `ldd` resolves everything.
4. Built SeExpr (`wdas/SeExpr`, branch `v1-2.11`) from source, per
   openfx-io's own CI recipe, and rebuilt `IO.ofx` against it. All three IDs
   `BaseTest` asserts now export: `fr.inria.openfx.ReadOIIO`,
   `fr.inria.openfx.WriteOIIO`, `net.sf.openfx.SeNoise`.
5. Ran the whole `Tests` binary under `xvfb-run` with
   `OFX_PLUGIN_PATH` pointed at the native bundle:
   `28 tests from 14 test cases ran. [ PASSED ] 28 tests.`

## Corrections to the drop-vendored-ofx decision

- **Reason 3 is factually wrong.** `SeNoise` does **not** live in
  openfx-misc. It is `openfx-io/SeExpr/SeNoise.cpp`, built into the same
  `IO.ofx` bundle as ReadOIIO/WriteOIIO. Verified two ways: the source file
  exists in openfx-io and nothing named SeNoise exists in openfx-misc; and
  the prebuilt bundle `fetch-assets.sh` already downloads *does* export
  `net.sf.openfx.SeNoise` (`strings` on
  `IO.ofx.bundle/Contents/Linux-x86-64/IO.ofx`). The premise that "one of the
  three required plugins was never fetched by anything" is not true -- all
  three were being fetched, in one bundle, the whole time.

- **Reason 2 was correct about the prebuilt binary, and is now moot.**
  Reproduced verbatim in the container:
  `couldn't open library .../IO.ofx because /lib64/libstdc++.so.6: version
  'GLIBCXX_3.4.30' not found`. The image's system libstdc++ tops out at
  `GLIBCXX_3.4.29`, exactly as claimed. It is worse than GLIBCXX, in fact:
  that bundle also wants `libOpenColorIO.so.1`, `libOpenImageIO.so.2.2`,
  `libIlmImf-2_5.so.25` and `libavformat.so.58` -- an entire ABI generation
  behind CY2027. **No Ubuntu 22 prebuilt will ever load here.** But "it
  cannot work in the target container" was a claim about *that download*, not
  about plugin testing, and `ci-vfxall` removes the "no OIIO/OCIO to build one
  in place" half of it outright.

- **Reason 1 still stands on its own merits.** Asserting that someone else's
  binary exports three symbols is a weak test, and that is a judgement call
  this spike does not touch. It is now, however, the *only* surviving reason.

## What it would cost to keep the tests

Not free, and not a one-line revert:

- **`fetch-assets.sh` becomes a build, not a download.** Clone openfx-io
  (pinned SHA) + `wdas/SeExpr` (`v1-2.11`), build both, assemble the bundle.
  On 4 cores that was a few minutes; it needs caching to not be felt on every
  CI run.
- **SeExpr must be vendored.** ASWF ships no SeExpr at any VFX Platform year
  (confirmed: no `ASWF_SEEXPR_*` in `versions.yaml`). Without it, ReadOIIO and
  WriteOIIO still work and `BaseTest` still fails on SeNoise alone -- verified
  as an intermediate step in this spike.
- **openfx-io's CMake SeExpr path is broken upstream.** `CMakeLists.txt`
  reads `${SEEXPR2_INCLUDES}` / `${SEEXPR2_LIBRARIES}` (plural), but its own
  `cmake/Modules/FindSeExpr2.cmake` only sets `SEEXPR2_INCLUDE_DIR` /
  `SEEXPR2_LIBRARY` (singular), so the SeExpr sources compile but never link
  -- it fails with `undefined symbol: _ZTI11SeExprFuncX`. This is why
  openfx-io's own CI uses the Makefile build with `SEEXPR_HOME` rather than
  CMake. Workaround used here: pass both spellings on the command line. Worth
  upstreaming as a one-line fix (`PLAN/DECISIONS/2026-08-29-upstream-agnostic-fixes.md`).
- **No FFmpeg in the image**, so ReadFFmpeg/WriteFFmpeg are absent from the
  bundle. Irrelevant to `BaseTest`, relevant to M11's scope if it wants to
  cover video I/O.
- `cmake_minimum_required(VERSION 3.1)` in both openfx-io and SeExpr needs
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` under the image's CMake 4.3.3.

## Recommendation

Reopen the M9/M11 split as a real choice rather than a forced one, because
"it cannot work" is no longer true:

- If reason 1 (don't unit-test third-party binaries on the merge gate) is
  still the position, keep M9 as planned -- but fix its stated rationale,
  because two thirds of it is wrong, and a future reader will otherwise
  "know" that SeNoise lives in openfx-misc.
- If plugin loading is wanted back, this spike is most of M11 already: build
  from source, pin the SHAs, and the test that has been red since M1 goes
  green. M11's premise ("with pinned, EL9-compatible bundles") is satisfied by
  building them, and the artifacts to do it are proven.

Reproduction scripts and logs from this spike are under the job tmp dir; the
essential recipe is the five numbered steps above.
