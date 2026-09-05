# Milestone 11: OFX plugin integration test (post-release hardening)

The existing smoke test proves the four OFX bundles (IO, Misc, CImg, Arena)
load via dlopen and that IO.ofx renders pixels through reader→writer chains.
This milestone closes two genuine gaps: (1) no check that Natron's OFX host
actually *enumerates* plugin IDs from each bundle, and (2) no render path
exercises a non-IO plugin — Misc generators, CImg filters, and Arena effects
are loaded but never asked to process pixels.

Video I/O (ReadFFmpeg/WriteFFmpeg) remains out of scope: the ASWF
ci-vfxall:2027 image ships no ffmpeg, so those plugins are not compiled
into IO.ofx.bundle.

## Phase 11.1: Plugin integration coverage

- [x] M11.P1.T1 — Assert Natron's OFX host sees expected plugin IDs from every bundle
  - files: tools/ci/smoke_test.py
  - approach: Add `check_plugin_id_enumeration()`. Call
    `NatronEngine.natron.getPluginIDs()` and assert a representative set of
    IDs from each bundle is present: IO (`fr.inria.openfx.ReadOIIO`,
    `fr.inria.openfx.WriteOIIO`), Misc (`net.sf.openfx.ConstantPlugin`,
    `net.sf.openfx.GradePlugin`, `net.sf.openfx.MergePlugin`), CImg
    (`net.sf.cimg.CImgBlur`, `net.sf.cimg.CImgPlasma`), Arena
    (`net.fxarena.openfx.Text`, `net.fxarena.openfx.Texture`). Log total
    count and per-bundle hits. Wire the call into `main()` after
    `check_ofx_plugin_bundle_set()`.
  - verify: `tools/ci/local/test.sh smoke debug` prints the new OK line with
    per-bundle counts. Temporarily add a bogus ID and confirm it raises
    AssertionError.
  - size: S

- [x] M11.P1.T2 — Render through Misc.ofx: Constant generator → Grade filter → Writer
  - files: tools/ci/smoke_test.py
  - approach: Add `check_misc_effect_render()`. Create a Constant
    (`net.sf.openfx.ConstantPlugin`, 16×16, color (0.5, 0.25, 0.125, 1.0)),
    wire it into a Grade (`net.sf.openfx.GradePlugin`, multiply
    (2.0, 2.0, 2.0, 1.0)), then into a Writer. Render frame 1 to a temp
    PNG. Read with `QImage` and assert the centre pixel: red ≈ 255 ± 2
    (scene-linear 1.0 → sRGB), green ≈ 186 ± 4 (sRGB of 0.5), blue ≈ 137
    ± 4 (sRGB of 0.25). This proves both Constant generated the colour AND
    Grade's multiply was applied. Wire the call into `main()` after the
    enumeration check.
  - verify: `tools/ci/local/test.sh smoke debug` — output PNG exists and
    three channel assertions pass.
  - size: M

- [~] M11.P1.T3 — ~~Render through CImg.ofx~~ (dropped)
  - CImgBlur produces all-black output in headless mode; CImgPlasma and
    CImgNoise lack standard OFX params (`extent`, `sigma`) via getParam().
    Root cause unresolved — likely a CImg-specific clip/RoD initialization
    issue in NatronRenderer's background mode. Plugin ID enumeration (T1)
    already proves CImg.ofx loads and registers its plugins.

- [~] M11.P1.T4 — ~~Render through Arena.ofx~~ (dropped)
  - Arena's Texture generator lacks the standard `extent` param via
    getParam(), same as CImgPlasma. Plugin ID enumeration (T1) already
    proves Arena.ofx loads and registers its plugins.

**Verification gate:** A full CI run (push to a branch targeting main) passes
with all 8 smoke checks (6 existing + 2 new: enumeration + Misc render)
green and no regressions in the ctest cases. T3/T4 were dropped as known
gaps — see above.
