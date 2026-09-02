# Build openfx-arena, with four of its plugins deliberately absent

2026-09-02. This fork now builds three of upstream Natron's four OFX plugin
repositories — `openfx-io`, `openfx-misc` and `openfx-arena` — from pinned
source. `Arena.ofx.bundle` exposes **22 plugins**. `openfx-gmic` remains
deferred for reasons unrelated to this decision; see
`2026-09-02-openfx-gmic-source-unavailable.md`.

## The four plugins that do not ship, and why

- **`ReadSVG`** (librsvg) and **`ReadCDR`** (libcdr + librevenge) — the
  `aswf/ci-vfxall` image ships none of those libraries, and neither librevenge
  nor libcroco has source this build can reach.
- **`ReadPDF`** (poppler) — excluded on purpose. Poppler is GPL-2.0, and
  linking it would make `Arena.ofx` GPL-2.0 outright; upstream ships a
  `LICENSE=COMMERCIAL` switch specifically to avoid that. Natron is already
  GPL-2.0 so nothing breaks today, but packaging is still an open question
  (`2026-08-29-defer-packaging-decision.md`) and this keeps the options open.
  It also sidesteps upstream issue #23, still open: `ReadPDF.cpp` includes
  poppler's private `GlobalParams.h`, which needs C++17 while arena pins C++11.
- **`AudioCurve`** (sox) — optional, and off in upstream's own bundle.

Restoring any of them is a matter of providing the dependency and flipping the
corresponding option back on; the fork's defaults are unchanged from upstream.

## Why the fork rather than a local patch

Upstream's `CMakeLists.txt` calls `pkg_search_module(... REQUIRED ...)` for
librsvg, poppler, libcdr and librevenge unconditionally, so configure fails
before it reaches any source we can build. `charlesangus/openfx-arena` adds
`WITH_SVG`, `WITH_PDF` and `WITH_CDR`, **each defaulting `ON`**, gating both the
`pkg_search_module` call and the matching source-list entry. Defaulting them
`ON` keeps upstream behaviour byte-identical for anyone who does not set them,
which is what makes the patch offerable upstream unchanged. This follows
`2026-08-31-fork-and-fix-natrongithub-repos.md`; the pin is
`d29ac7f180e1ea5cca2b8c0492686638836b605c`.

## The non-obvious build flags

- **ImageMagick must be Q32/HDRI.** Every Magick-based plugin round-trips OFX
  float buffers through `Magick::FloatPixel`. An integer-quantum or non-HDRI
  build silently clips linear values — a correctness requirement, not a
  packaging preference.
- **`--without-jxl`.** The image's libjxl changed
  `JxlDecoderGetColorAsICCProfile`'s signature; ImageMagick's `coders/jxl.c`
  will not compile against it.
- **The FreeType override** (`CPPFLAGS=-I/usr/local/include/freetype2`,
  `LDFLAGS=-L/usr/local/lib -Wl,-rpath,/usr/local/lib`). The image carries
  FreeType 2.10.4 at `/usr/lib64` and 2.13.x at `/usr/local`; `pkg-config
  freetype2` resolves the former, but `/usr/local/lib/libharfbuzz.so.0` needs
  the latter's `FT_Get_Paint*` symbols. **This split is an image-level property
  and will bite any future font-touching dependency built in this container.**
- **lcms2 is rebuilt from source despite being in the image**, which ships
  `liblcms2.so.2.0.19` and a header but no `lcms2.pc` — and arena resolves it
  via `pkg_search_module(LCMS2 REQUIRED lcms2)`. Hand-writing a `.pc` for a
  library we do not control would drift silently against the image. libzip is
  absent from the image entirely.
- **The CMake path, not `Makefile.master`.** The Makefile runs `pkg-config
  OpenColorIO` unconditionally, and this image ships OCIO as a CMake config
  package with no `.pc` file at all — it would fail before touching a single
  arena source. The CMake build never needs OCIO: `GenericOCIO.cpp` compiles
  only under `OFX_IO_USING_OCIO`, which nothing here defines.

## Staging, and where the RUNPATH actually points

Magick++/Wand/Core, lcms2 and libzip are copied into
`Arena.ofx.bundle/Libraries/` — **not** `Contents/Libraries/`. The binary's
`$ORIGIN` is `Contents/Linux-x86-64`, so its existing
`RUNPATH=$ORIGIN/../../Libraries` resolves two levels up, at the bundle root.
Verified with `LD_DEBUG=libs`, and by `ldd` with `LD_LIBRARY_PATH` cleared:
all five resolve inside the bundle and nothing is `not found`. The bundle is
therefore self-contained with no `LD_LIBRARY_PATH` anywhere — the same property
static linking bought for `IO.ofx`'s SeExpr.

## Licence consequence

`tools/license/components/` gains `LICENSE-libzip.txt` (BSD-3-Clause);
ImageMagick and Little CMS were already carried. The four unbuilt readers'
dependencies — librsvg, poppler, librevenge, libcdr — were removed from the
`openfx-arena` section of the manifest, because the manifest is a claim about
what ships, and none of them is linked into anything this fork produces.
