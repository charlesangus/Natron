# LensCorrect

Corrects lens distortion, transverse chromatic aberration (TCA) and vignetting from the lensfun
database, as darktable's lens correction module does. `mode` Correct removes the lens's defects;
Distort adds them, for instance to match a CG element to a plate. `correctDistortion`, `correctTCA`
and `correctVignetting` choose which of the three apply. TCA is never applied to a monochrome image.

The camera and lens are looked up in the database bundled with Natron. An empty `camera` reads the
Source's `exif/Make` and `exif/Model` metadata (a raw file's `raw/make` and `raw/model` otherwise),
and an empty `lens` reads `exif/LensModel`, cut at " or " or " (" as darktable does. A fixed-lens
camera needs no lens name. `cropFactor`, `focalLength`, `aperture` and `distance` at 0 also come
from the camera, `exif/FocalLength`, `exif/FNumber` and `exif/SubjectDistance`, else from the lens
(its shortest focal length, widest aperture) or 1000 m. `info` shows what was searched, what was
found and where each value came from. A lens that is not found, or a camera whose crop factor is
unknown and not typed, passes the image through unchanged and posts a warning on the node.

`targetGeometry` reprojects to another projection, for instance a fisheye to rectilinear; Same As
Lens keeps the lens's own. `autoScale` scales the result so that no empty border shows, as lensfun
computes it with every enabled correction; `scale` sets it by hand. `tcaOverride` replaces the
database's TCA with a linear scale of red (`tcaRed`) and blue (`tcaBlue`) about the lens centre.

`vignetteStrength`, `vignetteRadius` and `vignetteSteepness` are darktable's manual vignette, a
radial brightening applied to the input before the lensfun corrections, 0 at the image centre and
largest at the corners. `method` Manual Vignette Only applies just it. `showVignetteMask` writes
its gain, minus one, to alpha.

Each output pixel is mapped to the Source with lensfun's arithmetic, once per channel when TCA
applies, and sampled with the chosen filter. The filter footprint follows the map's Jacobian, taken
from green's positions by central differences. The model is built at the Source format's full-
resolution size, so proxy renders map the same way. The whole Source is read: lensfun's map has no
bound tighter than the image. Lensfun's vignetting gain is evaluated where each channel is read
when correcting (darktable applies it to the input) and at the output pixel when distorting.

Differences from darktable: the embedded lens corrections that darktable reads from raw makernotes
are not available, and so are not its fine-tune knobs; `camera` and `lens` are typed, not picked
from menus; the manual vignette is evaluated exactly rather than from a lookup table; negative
values are not clamped; pixels outside the Source are black when `blackOutside` is on, where
darktable repeats the edge.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| LensCorrect (LensCorrect) | custom | relative 1e-5 | Profile against `lfModifier` coordinates and vignetting gain on a 13 x 9 grid; flat field against `lfModifier`'s gain |
| LensCorrect (LensCorrect) | exact | `ASSERT_FLOAT_EQ` | Tiles against the whole frame; an unknown lens against the Source |
| LensCorrect (LensCorrect) | custom | `ParityTolerance::make(2e-4, 1e-4)` | GPU against CPU: the kernel normalises in float where the CPU uses double |
