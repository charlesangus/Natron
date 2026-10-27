# ContrastTexture

Scene-referred local contrast, texture and clarity. It is a port of darktable's contrast & texture
module (`src/iop/contrastntexture.c`, params version 2, at `DARKTABLE_MASTER_REF`).

The luminance is the RGB euclidean norm over sqrt(3), floored at 2^-16. It is blurred by a pyramid
of exposure-invariant guided filters (`EdgeAware::eigfSurfaceBlur`, linear blending). The coarsest
blur's radius is the image's larger side over 2^(Base Detail Level + 1), and each finer level
halves it, down to 2 pixels. At each level the detail is the log2 ratio of the level's input to its
blur, clamped to [-5, 5] EV. It is weighted by a Wiener gain that hides detail below the level's
noise bias. The detail is then scaled by a blend of Coarse, Medium and Fine Details: squared sine
and cosine crossfades over the levels. The final blur sets Shadows and Highlights, a log curve
pivoting on 0.1845 plus the noise bias. The summed correction multiplies the colour channels, so
ratios between them are kept, and alpha is left alone. Halo Control scales the filter's feathering
by 2^-halo. Noise Bias is added to the luminance at the finest level and lowered level by level towards the coarsest.

The whole source is filtered in every render: the node does not tile, and its region of interest is
the source's region of definition (cut to the format when infinite). The detail levels follow the
full-resolution size and the radii the render scale, so a proxy filters the same features. Mask,
mix and the unprocessed channels are the host's stages.

Controls (darktable's ranges and defaults; the soft range is the slider's):

| Knob | Label | Default | Range | Soft range |
| --- | --- | --- | --- | --- |
| `baseDetailLevel` | Base Detail Level | 5 | 1 .. 15 | 1 .. 10 |
| `highlights` | Highlights | 0 | -5 .. 5 | -2 .. 2 |
| `shadows` | Shadows | 0 | -5 .. 5 | -2 .. 2 |
| `coarseDetails` | Coarse Details | 0 | -1 .. 5 | -1 .. 1.5 |
| `mediumDetails` | Medium Details | 0 | -1 .. 5 | -1 .. 1.5 |
| `fineDetails` | Fine Details | 0 | -1 .. 5 | -1 .. 1.5 |
| `haloControl` | Halo Control | 0 | -10 .. 10 | -5 .. 5 |
| `noiseBias` | Noise Bias | 0.001 | 0 .. 1 | 0 .. 0.2 |
| `maskDisplay` | Mask Display | Off | Off, Coarse Details, Medium Details, Fine Details, Low-Pass | |

The detail gains are plain factors, where darktable shows them in per cent. darktable's mask
toggles are GUI state on each slider. Here they are the Mask Display choice, which renders the
preview instead of the image: the band's detail as grey around 0.5, or the final low-pass tinted
blue below the pivot and yellow above it. A plane with fewer than three channels takes the
luminance of the channels it has, scaled to match a grey pixel.

The GPU path (`Engine/Gpu/Kernels/contrastntexture.slang`, with the edge-aware library's
`eigf.slang`) runs the same pyramid in device scratch buffers.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ContrastTexture (ContrastTexture) | darktable reference | `kDarktableClass` | the kernels on make-reference.py's plate against darktable `DARKTABLE_MASTER_REF`, mean error 0.1 % and largest error 1 % of the reference's mean level |
| ContrastTexture (ContrastTexture) | node against kernels | `kKernelTolerance` | 1e-5 relative, the CPU node against `ContrastTextureKernels` on its source |
| ContrastTexture (ContrastTexture) | GPU against CPU | `kContrastTextureTolerance` | `ParityTolerance::make(0., 1e-4)`, the EIGF blurs' own bound |

The knob audit, identity at the defaults, the node against its kernels and the darktable
references are in `Tests/Native/NativeContrastTexture_Test.cpp`; the GPU parity is in
`Tests/Native/NativeContrastTextureGpu_Test.cpp`. The references are described in
`Tests/fixtures/raw/README.md`.
