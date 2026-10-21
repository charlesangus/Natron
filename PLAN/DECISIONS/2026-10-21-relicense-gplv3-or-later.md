# Relicense Natron to GPL-3.0-or-later

**Decision (user, 2026-10-21):** relicense the fork's first-party code from GPL-2.0-or-later to GPL-3.0-or-later, so Apache-2.0 code can be integrated outright. Delivered by M157 - Relicense To GPLv3 Or Later.

**Why it is allowed:** every Natron header grants "version 2 of the License, or (at your option) any later version", and no first-party file is GPLv2-only. Qt 6 and PySide6 are LGPL-3.0, so release binaries were already effectively GPL-3.0-or-later. The 2026-10-16 glslang decision says the same about its Apache-2.0 parts.

**Dependency audit (2026-10-21):**
- Vendored libs: BSD, MIT, ISC, MPL-2.0, Apache-2.0, LGPL-2.1-or-later and SGI-B. All are compatible.
- System deps: Qt6, PySide6, Boost, OIIO, OCIO, OpenEXR, cairo, harfbuzz, Freetype, TBB, Vulkan, Slang and glslang. All are compatible.
- FFmpeg, x264, x265, fdk-aac, CUDA and USD do not ship.
- openfx-io and openfx-misc, at their pinned fork SHAs: every GPL header is v2-or-later.
- openfx-arena: the project LICENSE and README say v2-or-later. But `Magick/Sketch.cpp` and `Magick/MagickPlugin.h` say "version 2" only, and `MagickPlugin.h` is the base of Swirl, Morphology, Roll, HaldCLUT and Wave. The OCL plug-ins carry the same header but are not built.

**Choices made:**
- **Headers:** an `SPDX-License-Identifier: GPL-3.0-or-later` line plus a short block that keeps the existing copyright lines and points to `LICENSE.txt`. This replaces the full v2 boilerplate.
- **The six arena plug-ins:** Sketch, Swirl, Morphology, Roll, HaldCLUT and Wave are dropped from the `charlesangus/openfx-arena` fork build now. They are not rewritten, and no confirmation is sought from the author.
- **Left as GPL-2.0-or-later:** openfx-io, openfx-misc, openfx-arena (minus the dropped files) and `libs/SequenceParsing`. That licence is already compatible with a GPL-3.0-or-later Natron.
- **Third-party code in first-party directories keeps its own licence:** SeExpr noise (Apache-2.0), QtColorTriangle, glad, FloatingPointExceptions and similar.
