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

- [ ] M11.P1.T3 — Render through CImg.ofx: Reader → CImgBlur filter → Writer
  - files: tools/ci/smoke_test.py
  - approach: Add `check_cimg_effect_render()`. Generate a 16×16 PNG with a
    sharp vertical stripe (column 0 white, rest black). Wire
    reader → CImgBlur (`net.sf.cimg.CImgBlur`, size 3.0) → writer. Render
    to a temp PNG. Assert two properties: pixel at (4, 8) red > 5 (blur
    spread energy away from the stripe), and pixel at (0, 8) red < 245
    (blur reduced the peak). Fails both if CImgBlur was a passthrough and
    if it zeroed everything. Wire into `main()` after the Misc check.
  - verify: `tools/ci/local/test.sh smoke debug` — both pixel assertions
    pass. Set blur size to 0.0 to confirm the far-pixel assertion fails.
  - size: M

- [ ] M11.P1.T4 — Render through Arena.ofx: Texture generator → Writer
  - files: tools/ci/smoke_test.py
  - approach: Add `check_arena_effect_render()`. Create a Texture generator
    (`net.fxarena.openfx.Texture`, 16×16), wire to a writer, render frame 1.
    Assert the output exists, is non-empty, and sample 8+ pixels across the
    image — assert not all are the same RGB value (a procedural texture must
    produce spatial variation). This proves Arena's ImageMagick render path
    is functional (libMagickCore/libMagickWand/liblcms2/libzip staging in
    Arena.ofx.bundle/Libraries/ is correct). Wire into `main()` after the
    CImg check.
  - verify: `tools/ci/local/test.sh smoke debug` — file exists, non-empty,
    pixel-variation assertion passes.
  - size: M

**Verification gate:** A full CI run (push to a branch targeting main) passes
with all 10 smoke checks (6 existing + 4 new) green and no regressions in
the ctest cases.
