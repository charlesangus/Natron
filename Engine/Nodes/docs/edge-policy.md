# Edge policy: what an image is outside its bounds

Status: approved 2026-11-13 (M136 - Edge Policy Investigation, Phase 136.1), with the answers in
section 3. Refreshed to `main` at b59ff3fdf.

Today each native node decides for itself what lies outside an input's region of
definition (RoD). This note surveys every native node that reads outside its own
output position, then fixes one rule: every image carries a binary *edge mode*
in its `NodeMetadata`, and every consumer reads outside an input's RoD through one
CPU helper and one GPU sampler driven by that mode. Every input of a node follows
its own bit, not only the source (Q1).

Paths are relative to the repository root. Line numbers are as of `main` at
b59ff3fdf.

## 1. Survey

Terms used below:

- **Zero**: taps outside the source RoD read 0 (Dirichlet).
- **Nearest**: taps outside the source RoD read the nearest source pixel (Neumann, clamp to edge).
- **Line ends**: separable IIR and box filters see a whole buffer line. Past its ends they read
  either 0 or the end sample (`BlurKernels::LineFilter` `neumann`, `Engine/Nodes/Filter/BlurKernels.h:58-59`).
  Under Nearest the buffer is already filled with clamped pixels, so Neumann line ends
  continue the same edge.
- **Window clamp**: a gather window clipped to the RoD, whose taps past the buffer are clamped
  to it or left out. The fill beyond the source is then never read unless the RoD grows.

### 1.1 Shared machinery

| Piece | CPU | GPU | Outside rule today |
|---|---|---|---|
| Pass executor buffer fill | `PassExec::fillBuffer`, `Engine/Nodes/Image/PassExecutor.cpp:336-424`; `PassExec::Boundary {Zero, Nearest}`, `PassExecutor.h:218-224`; `PassList::boundary` defaults to Zero, `PassExecutor.h:435` | No shared fill. Each kernel fills its own buffer (see 1.3). | Per node: `list.boundary`; a node that sets none gets Zero |
| Resampler (Transform family, warps) | `Resampler::interpolateT`, `Engine/Nodes/Image/Resampler.cpp:600-616`, with `blackOutside` threaded through `readOffset` (`:428-437`), `clampCoord` (`:334-338`), the coordinate form (`:576-585`) and the super-sampling tests (`:871, 909, 1155`) | `resamplecore.slang` (`kFlagBlackOutside` at :35, `ResampleSource::blackOutside` at :235, clamped tap at :281, `readOffset` at :672), imported by `resample.slang`, `coordmap.slang`, `stmap.slang`, `iwarp.slang`, `lensdistortion.slang`, `lenscorrect.slang`, `splinewarp.slang` | `blackOutside` true: Zero; false: Nearest |
| Resample executor copy path (whole-pixel offset: Crop, Position, Mirror) | `readSourceImageRow`, `Engine/Nodes/Image/ResampleExecutor.cpp:142-164` | Copy kernel through `GpuResample` | Always Zero |
| Black-outside RoD growth | `Resampler::expandRoD`, `Resampler.cpp:1947-1965`, called at `:2047`; also `IWarp.cpp:473`, `GodRays.cpp:562`; LensDistortion's margin, `LensDistortion.cpp:925` | n/a | One output pixel per finite side when `blackOutside` |
| Edge-mode flags in GPU params | `ResampleSettings::blackOutside`, `ResampleExecutor.h:59`; `ResampleParams` and `WarpSettings` (`Resampler.h:621, 706`) | `GpuResample.cpp:18, 281` and `GpuResample.h:100`; `Transform/WarpGpu.h:91`; `IWarp.cpp:69`; `LensDistortion.cpp:1182`; `LensCorrect.cpp:1639`; `SplineWarp/SplineWarpGpu.h:80`; `DirBlur.cpp:71` and `GodRays.cpp:70` (both `= 32`); the slang copies `stmap.slang:16`, `iwarp.slang:15`, `lensdistortion.slang:13`, `lenscorrect.slang:11`, `splinewarp.slang:18`, `dirblur.slang:17`, `resamplecore.slang:35` | Fifteen copies of the same bit |
| `InputBoundary {Zero, Nearest}` | `Engine/Nodes/NodeSpec.h:56-59`, field at `:102` | n/a | **Dead.** No code reads `InputSpec::boundary`; only `Tests/Native/NodeSpec_Test.cpp:67` sets it. |
| Shared knob strings | `kResamplerParamFilterBlackOutside*`, `Resampler.h:50-52` | n/a | n/a |

### 1.2 Transform and warp nodes (Resampler family)

| Node | CPU path | GPU path | Outside source today | Knob (default) | Task |
|---|---|---|---|---|---|
| Transform | ResampleExecutor to Resampler | `resample.slang` | Knob | `black_outside` (on), `Transform.cpp:222-227` | M136.P3.T2 |
| Reformat | ResampleExecutor to Resampler | `resample.slang` | Knob | `black_outside` (off), `Reformat.cpp:457-462` | M136.P3.T3 |
| Crop | ResampleExecutor copy path plus a matte, `Crop.cpp:178-230` | `GpuResample` copy | Zero beyond the source, always (`ResampleExecutor.cpp:142`), whatever the input is; with Black Outside the RoD grows by 1 px (`Crop.cpp:455-463`) and the outermost crop row and column are forced black (`Crop.cpp:224-230`); Reformat's toggle drives the knob (`Crop.cpp:362-365`) | `blackOutside` (off), `Crop.cpp:296-301`, name `Crop.h:46` | M136.P3.T4 |
| CornerPin | ResampleExecutor to Resampler | `resample.slang` | Knob | `blackOutside` (on), `CornerPin.cpp:307-312`, name `CornerPinKnobs.h:66` | M136.P4.T4 |
| ITransform | IWarp, `resampleRowFromCoords` | `iwarp.slang` | Transform's knob, `ITransform.cpp:238` | Transform's `black_outside` (on) | M136.P4.T4 |
| ICornerPin | IWarp, `resampleRowFromCoords` | `iwarp.slang` | CornerPin's knob, `ICornerPin.cpp:226` | CornerPin's `blackOutside` (on) | M136.P4.T4 |
| IWarp (base) | `IWarp.cpp:884, 966`; RoD `:473` | `iwarp.slang:224` | `WarpSettings::blackOutside`, `IWarp.h:125` | via ITransform/ICornerPin | M136.P4.T4 |
| IDistort | `resampleRowFromCoords`, `IDistort.cpp:663, 679` | `coordmap.slang` via `WarpGpu` (`:126-131`) | Knob | `black_outside` (off), `IDistort.cpp:196-201` | M136.P4.T4 (UV input: P4.T12) |
| STMap | `resampleRowFromCoords`, `STMap.cpp:762` | `stmap.slang:243` | Knob, only when Wrap U/V is None (`Resampler.h:345`); Clamp/Repeat/Mirror fold the coordinate first | `black_outside` (on), `STMap.cpp:274-279`; Wrap U/V `STMap.cpp:210, 219` | M136.P4.T4 (UV input: P4.T12) |
| LensDistortion | `resampleRowFromCoords`, `LensDistortion.cpp:1128` | `lensdistortion.slang:73` | Knob; adds 1 px margin when on, `LensDistortion.cpp:925` | `black_outside` (on), `LensDistortion.cpp:420-425` | M136.P4.T4 |
| LensCorrect | `LensCorrect.cpp:1473-1478` source, `resampleRowFromCoords` | `lenscorrect.slang:521` | Knob; an edge fade (`:1073`), an outside shortcut (`:1091`), and a source-region bound that counts footprints only where they overlap the Source (`:1219-1304`) | `black_outside` (on), `LensCorrect.cpp:483-488` | M136.P4.T13 |
| SplineWarp | `SplineWarp.cpp:1934-1941`; `cropRowToFormat` sends positions outside the format outside every image, or onto the format's edge pixels when not black (`:140-156`) | `splinewarp.slang:172` | Knob | `black_outside` (on), `SplineWarp.cpp:507-512` | M136.P4.T13 |
| AutoStraighten | ResampleExecutor to Resampler, `AutoStraighten.cpp:543, 570` | `resample.slang` | Knob | `black_outside` (on), `AutoStraighten.cpp:878-883` | M136.P4.T13 |
| Mirror | ResampleExecutor, impulse filter | `resample.slang` | Hard-wired `blackOutside = true`, `Mirror.cpp:83`. Whole-pixel flips of the RoD: no output pixel inside the RoD reads outside the source. | none | M136.P4.T4 (drops the flag, forwards the bit) |
| Position | ResampleExecutor offset | copy | Whole-pixel shift of the RoD: no outside reads | none | none: forwards the bit |
| NeuralUpscale | separable resample whose taps clamp into the source frame, `NeuralUpscale.cpp:137-190` | n/a | Nearest, hard-wired; reads only the source frame's own taps past its border | none | **none yet** (reported to the pm) |

### 1.3 Filters

A node that sets no `list.boundary` gets the `PassList` default, Zero.

| Node | CPU source read | CPU line ends | GPU | Outside source today | Knob | Task |
|---|---|---|---|---|---|---|
| Blur | `fillBuffer`, default Zero (`Blur.cpp:219`, no `list.boundary`) | Dirichlet: `neumann = false`, `Blur.cpp:237-262` | `blur.slang` (FIR), `blur_lines.slang:248` (zero outside source), `:297-355` (line ends from `neumann`) | Zero | none | M136.P4.T1 |
| EdgeDetect | Nearest, `EdgeDetect.cpp:939` | Neumann, `EdgeDetect.cpp:916-917` | `edgedetect.slang:252-253` (own clamped fill), `blur_lines` with `neumann` | Nearest, fixed | none | M136.P4.T2 |
| ErodeDilate | `fillBuffer`, Zero; the min/max window clamps to the buffer, which is clipped to the RoD (`ErodeDilate.h:112-120`) | window clamp (`ErodeDilate.h:58-63`) | `morphology.slang:12-13, 114` (`paddedAt`, zero outside source), `:161-167` | Zero beyond the source, window clamped at the RoD; the RoD grows with `expandRoD` (`ErodeDilate.h:46, 94`). **CPU and GPU agree today**: `NativeErodeDilateGpu_Test.cpp:376` (`MatchesTheCpuRender`) checks them bit for bit with the source off the format origin. The knob help no longer names Neumann. | none | M136.P4.T3 |
| Sharpen | Nearest, `Sharpen.cpp:503` | Neumann, `Sharpen.cpp:471` (used at `:483-499`) | `sharpen.slang:62-79` (own clamped fill), then `recordSeparableBlur` / `recordLineBlur`, `Sharpen.cpp:424-442` | Nearest, fixed | none | M136.P4.T5 |
| EdgeExtend | Nearest, `EdgeExtend.cpp:516` | Neumann, `EdgeExtend.cpp:511` | `edgeextend.slang:65-66, 116-117` (own clamped fills) | Nearest, fixed | none | M136.P4.T5 |
| Bloom | Nearest, `Bloom.cpp:75` | `BloomKernels.cpp:162-169` (`resolveIndex`, nearest or zero), `plan->nearest` defaults true at `:224` | `bloom.slang:40` (nearest or zero reads past the valid pixels) | Nearest, fixed | none | M136.P4.T6 |
| Glow | Zero, `Glow.cpp:1016`; its downsample reads zero outside, `Glow.cpp:351, 839` | n/a | `glow.slang:142`, `glow_frame.slang` | Zero, fixed | none | M136.P4.T6 |
| IBlur | `fillBuffer`, default Zero (`IBlur.cpp:651`); summed-area table zero outside, `VariableBlurKernels.h:43`, `VariableBlurKernels.cpp:38` | n/a | `iblur.slang` | Zero, fixed ("black outside the image", `IBlur.cpp:76`). Its Map input is in 1.4a. | none | M136.P4.T7 |
| IErodeDilate | `fillBuffer`, default Zero (`IErodeDilate.cpp:1731`) | n/a | `ierodedilate.slang:34-35, 243` | Zero, fixed. Its Map input is in 1.4a. | none | M136.P4.T7 |
| EdgeBlur | Matte filled with EdgeDetect's boundary (Nearest), `EdgeBlur.cpp:506-530`; blur through IBlur, whose Map reads 0 outside EdgeDetect's RoD (`EdgeBlur.cpp:619-620`) | as EdgeDetect, IBlur | as EdgeDetect, IBlur | Nearest for the matte, Zero for the blurred source | none | M136.P4.T7 |
| DirBlur | `Resampler::SourceImage` over the source, `DirBlur.cpp:825-830` | n/a | `dirblur.slang:17, 50, 75` | Knob; `DirBlur::sourceRegion` (`DirBlur.cpp:656-662`) keys off it | `black_outside` (on), `DirBlur.cpp:331-336` | M136.P4.T14 |
| GodRays | as DirBlur, `GodRays.cpp:642-647`; RoD growth `:562` | n/a | `GodRays.cpp:70, 788` (flag into the DirBlur kernel) | Knob | `black_outside` (on), `GodRays.cpp:348-353` | M136.P4.T14 |
| Median | `fillBuffer`, Zero (`Median.cpp:249`); the window's taps clamp to the buffer, which is clipped to the RoD (`:110-116`, `:134`), so the Zero fill is never read: the RoD does not grow (`growX = 0`) | n/a | `median.slang` (reads the source straight over the domain) | Window clamp at the RoD edge (Nearest-like), fixed | none | M136.P4.T15 |
| Bilateral | `fillBuffer`, Zero (`Bilateral.cpp:951`); sums run over the samples inside the buffer, clipped to the RoD (`:210, 270, 474`), so the Zero fill is never read | n/a | `bilateral.slang` | Window clamp at the RoD edge (shrinking window), fixed | none | M136.P4.T15 |
| Defocus | `fillBuffer`, default Zero (`Defocus.cpp:782, 834`) | n/a | `defocus.slang:85` (source "0 outside the source") | Zero, fixed ("black outside the image", `Defocus.cpp:80`). The Kernel input is read over its own RoD, not at the output position: not an edge read. | none | M136.P4.T16 |
| IDefocus | `fillBuffer`, default Zero (`IDefocus.cpp:497`; weights count the black outside, `:341-343`) | n/a | `idefocus.slang` | Zero, fixed (`IDefocus.cpp:87`). Kernel as Defocus; Map input is in 1.4a. | none | M136.P4.T16 |
| ZDefocus | `fillBuffer`, default Zero (`ZDefocus.cpp:564, 611`) | n/a | `zdefocus.slang` | Zero, fixed. Kernel as Defocus; Depth input is in 1.4a. | none | M136.P4.T16 |
| VectorBlur | the gather's `tap()` reads the source whole and bilinear, black outside its bounds (`VectorBlur.cpp:104-117`; source rows `:1017-1022`) | n/a | `vectorblur.slang:139` ("bilinear, black outside the Source") | Zero, fixed. UV and Depth inputs are in 1.4a. | none | M136.P4.T16 |
| DenoiseProfiled | `fillBuffer`, Zero (`DenoiseProfiled.cpp:1022`) over the node's own RoD, so the Zero fill is never read; the algorithm's own border rules are fixed: wavelet taps clamp to the image (`DenoiseProfiledWavelets.cpp:89, 99`), the non-local-means column sums are zero past it (`DenoiseProfiledNlm.cpp:140-145`) | n/a | `denoiseprofile_*.slang` | Image-internal border rules, fixed | none | M136.P4.T17 (see 1.5) |
| AdjustBBox | `list.boundary = Nearest` (`AdjustBBox.cpp:206`) over the source grown by the box; a choice knob picks what fills the border added around the input | n/a | `adjustbbox.slang:30-48` (the nearest kernel; Black takes the plain path) | **Knob, not fixed**: Boundary Black (default) or Nearest, `AdjustBBox.cpp:157-166` (read at `:178`) | `Boundary` choice (Black), `AdjustBBox.cpp:157-166` | M136.P4.T15 |

### 1.4 Nodes that read no pixel outside their own position

Colour nodes (`Engine/Nodes/Color/*`, except as noted in 1.5), keyers (`Keyer/*`),
Switch, FrameHold, FrameRange, TimeOffset, TimeWarp, Retime, AppendClip, NoTimeBlur
(they pick a whole frame), channel nodes other than ShuffleCopy (`AddLayers`,
`RemoveLayers`), metadata nodes, OCIO nodes, Clamp, Posterize, Premult/Unpremult,
generators (Constant, CheckerBoard, Noise), Read and the Scene/USD render nodes. They
only forward or originate the bit (section 2). `DepthOfField` and `LensAberration` are
helper classes of the Defocus family, not nodes. Merge, Dissolve, KeyMix, ShuffleCopy,
FrameBlend and every node's mask input still read *another* input at the same position
outside that input's RoD. Section 1.4a lists them.

### 1.4a Same-position reads

A node reads input *k* at the pixel it is writing. Where input *k*'s image does not cover
that pixel, every site below reads zero today (alpha 0, channel 0). Under Q1 each input
follows its own edge bit, so these sites move onto the shared samplers, and the rule
below each table is what it becomes.

**Mask inputs.** The mask, and the op mask of a node that applies it inside its passes.

| Site | CPU | GPU | Zero rule today |
|---|---|---|---|
| MaskMix stage, pass executor | `PassExecutor.cpp:520` (`readChannelRow`, fill 0) | `pixelwrap.slang:128-134` (`wrapMaskValue`) | 0 outside the mask's image |
| Op mask (`list.opMask`) | `PassExecutor.cpp:1076` | `PassGpuIO::mask` (`PassExecutor.h:389-392`) | 0 outside, 1 everywhere when no mask is applied |
| MaskMix stage, staged plan | `StagePlan.cpp:1268` | `pixelwrap.slang:128-134` | 0 outside |
| MaskMix stage, resample executor | `ResampleExecutor.cpp:609` | same | 0 outside |
| IWarp, IDistort, SplineWarp, VectorBlur masks | `IWarp.cpp:990`, `IDistort.cpp:703`, `SplineWarp.cpp:2060`, `VectorBlur.cpp:1103` | the node's kernel | 0 outside |
| Mask-misses-window identity shortcut | `StagePlan.cpp:653` (`stageMaskMissesWindow`), used at `StagedImageEffect.cpp:825` | n/a | A non-inverted mask that does not reach the window leaves the source unchanged: sound only because the mask reads 0 there |
| Divisor (the (un)premult-by channel, a channel of the node's own source) | `PassExecutor.cpp:517`, `StagePlan.cpp:1250` | `pixelwrap.slang:105` | 1 outside |

**Composite sources and alphas.** Merge's A inputs and B, KeyMix, Dissolve and the per-input alpha.

| Site | CPU | GPU | Zero rule today |
|---|---|---|---|
| Composite source rows | `readStageSourceRow`, `StagePlan.cpp:937-967`, read at `:1277` (base) and `:1288` (further inputs) | `compositestep.slang:30-34` (`stepReadInput`) | 0 outside the input's image |
| Composite alpha | `StagePlan.cpp:912, 1210, 1216` | `compositealpha.slang:39-45` (`stepReadAlpha`) | 0 outside; an input with no alpha reads 0 |
| Merge: inputs folded | `Merge.cpp:524-545` (B is the base, A and the further inputs fold in) | `merge.slang` | as above |
| Merge: transparent-A skip | `Merge.cpp:577-584` | `merge.slang:13, 36` | An all-zero A stretch with all-zero alpha keeps B without an operator call when the operator leaves B unchanged for a transparent A. Sound only because A reads 0 outside its RoD. |
| KeyMix | `KeyMix.cpp:97-109` (B base, A, mask) | `compositestep.slang` | as above |
| Dissolve | `Dissolve.cpp:105-111` (previous and next inputs), mask `:261, :286` | `dissolve.slang` | as above |

**Channel routes.** ShuffleCopy, and Shuffle through the same route.

| Site | CPU | GPU | Zero rule today |
|---|---|---|---|
| Channel copy | `ChannelCopy.cpp:84-92` (`copyRow`; an input with no pixel at x, y writes 0) | n/a | 0 outside the routed input's image |
| Channel route | `ChannelCopy.cpp:145-152` (`routeRow`), `applyChannelRoute` at `:426` | `channelroute.slang:64-78`; valid rect `ChannelCopy.cpp:757-760` | 0 outside |

**Map and UV inputs.**

| Site | CPU | GPU | Zero rule today |
|---|---|---|---|
| IBlur, IErodeDilate Map | `MapPlane`, `MapInput.cpp:155-195` (`outside()`, `tileRange`), read at `:413` | `iblur.slang:46-52, 365`; `ierodedilate.slang:34-35, 243` | A pixel outside the Map's image reads `remap(0)`, as a black pixel inside it does |
| IDefocus Map | `IDefocus.cpp:1312` (`MapPlane`) | `idefocus.slang` | as IBlur |
| EdgeBlur Map | the matte over EdgeDetect's RoD, `EdgeBlur.cpp:619-620` | as IBlur | as IBlur |
| IDistort UV | `IDistort.cpp:597` (`readChannelRow`) | `coordmap.slang` | 0 outside the UV image |
| STMap UV | `STMap.cpp:86-106` (`UVPlane`, `outside` fill) | `stmap.slang:114-121, 217` (`uvInside`) | `outside` outside the UV image |
| ZDefocus Depth | `ZDefocus.cpp:2185` | `zdefocus.slang:124` | 0 outside the depth image |
| VectorBlur UV and Depth | `VectorBlur.cpp:989, 1000` | `vectorblur.slang:79, 115` | 0 outside |

**Time reads.** Frames of the same input at other times, read at the same pixel.

| Site | CPU | GPU | Zero rule today |
|---|---|---|---|
| FrameBlend | `FrameBlend.cpp:431` (`pixelAt` null reads 0) | `timeblend.slang` | 0 outside a frame's image; the output RoD is the union over times |
| TimeBlur and the time-sampled effects | `TimeBlend.cpp:96`, through `TimeSampledEffect.cpp:288` | `timeblend.slang`, `TimeSampledEffect.cpp:361` | as FrameBlend |
| SlitScan | frames `SlitScan.cpp:617`, map `:540` | `slitscan.slang:108-113` | 0 outside |

### 1.5 Not covered by this milestone

- **Raw nodes** (`Engine/Nodes/Raw/*`, plus the `Filter/EdgeAware/*` helpers). They work on
  the sensor frame before `RawToImage`. Their borders are fixed choices of the algorithm they
  port: `Raw/RawHotPixels.cpp:189-204` skips a border strip, `EdgeAware/BoxFilter.h:38-41` is a
  shrinking window, `EdgeAware/Wavelets.h:46-48` mirrors and clamps. They forward the bit and
  keep their own borders (Q2).
- **Image-internal border rules.** These nodes process one image over its own RoD, so they read
  nothing past it. Their own fixed rules at the image border, the same kind as the Raw nodes',
  stay: ContrastTexture (`ContrastTexture.cpp:303` takes the whole RoD; the edge-aware helpers'
  borders), Retouch (crop inside the source, `Retouch.cpp:2196, 2452`), NeuralDenoise (frame is the
  RoD, `NeuralDenoise.cpp:859-862`), ToneEqualizer and ColorEqualizer (whole-RoD buffer filled at
  `ToneEqualizer.cpp:721` and `ColorEqualizer.cpp:1721`, so the fixed Zero is never read; borders
  from the edge-aware helpers), DenoiseProfiled (1.3). **They get no Edge Mode stage.** That
  settles the question M136.P4.T17 left open for the two colour nodes.
- **Deep nodes**: out of scope (M21 - Deep Tier-2 And Bridges).
- **Roto, Bezier, tracker**: out of scope.

## 2. The model

### 2.1 The bit

`NodeMetadata` gains `enum class EdgeMode : unsigned char { Stretch, Black }` and one
output-level field `edgeMode`, default `Stretch`, in `NodeMetadataPrivate`
(`Engine/NodeMetadata.cpp:50`). Since M167 - Per-Frame Node Metadata, metadata
is per (time, view), so the bit rides that. `NodeMetadata::operator==`
(`NodeMetadata.h:57`, `NodeMetadata.cpp:130`) compares it. That comparison is what makes a
metadata change visible.

Cache invalidation needs nothing extra. The bit only changes through a knob or a
connection, and either one moves the node's content hash. The hash folds in its
inputs' hashes (`Node::computeHashInternal`, `Engine/Node.cpp:1033-1063`, via
`computeContentHash(projectDigest, inputs, ...)` at `:1049`). So every downstream image key
moves with it. This settles the milestone's first assumption.

### 2.2 Who sets it

- **Sources** (Read, generators, USD render, RawToImage, DeepToImage) report Stretch.
- **Forwarding** happens in `EffectInstance::getDefaultMetadataFromInputs`
  (`Engine/EffectInstance.cpp:7276-7446`). That is where the format is chosen:
  the first non-optional connected input, else the first optional one (`:7365`).
  The edge mode uses the same input. For Merge that input is B, so Merge, KeyMix
  and ShuffleCopy need no code. Native nodes inherit this default, so the work is here
  rather than in `NativeEffectBase`.
- **Identity**: `EffectInstance::applyIdentityRule` (`EffectInstance.cpp:7536`) already
  returns the picked input's metadata for Switch, Dissolve at an end, and any node at
  identity, so the bit follows the picked input.
- **OpenFX** effects report Stretch in `OfxEffectInstance::getPreferredMetadataForInputs`
  (`Engine/OfxEffectInstance.cpp:2130-2215`). One exception: an OpenFX node at identity
  goes through the identity rule first (`EffectInstance.cpp:7536`) and forwards its
  input's bit. That is the input's own image, so this note keeps it.
- **Nodes with an Edge Mode knob** set their output bit by the rule in 2.3.

### 2.3 The Edge Mode knob

A pipeline stage, `Stage::EdgeMode` (most affected nodes are `StagedImageEffect`s), creates a
choice knob with script name `edgeMode` and label "Edge Mode", with options Input (default),
Black and Stretch. It resolves the choice against the primary input's bit and hands the mode
to the pass list, the resampler and the warps. Nodes that are not on the staged pipeline use
the same helper directly. It replaces `kResamplerParamFilterBlackOutside*`, Crop's
`blackOutside` and CornerPin's `blackOutside`.

**The rule (Q3).** Under Input the node follows the primary input's bit, and the output bit is
that bit. A forced Black or Stretch overrides the input's bit for the whole output of the node,
including the band between the source region and a crop rectangle or format, and the node's
output bit is the forced value. The forced setting is passed on downstream, not the input's bit.
A Blur set to Black on a Stretch input fades to black over its grown RoD, and downstream
sees black beyond it.

The knob goes on every node in 1.2 except Mirror and Position (they read nothing outside), on
every node in 1.3 with a row naming a task (Median and Bilateral included: their window
clamp stays, and the mode governs only what lies past a grown RoD, if any), and nowhere in 1.5.
The inputs other than the source read by their own bit (1.4a), not by the knob.

Default behaviour changes this causes (no backward compatibility):

- Transform, CornerPin, ITransform, ICornerPin, STMap, LensDistortion, LensCorrect, SplineWarp,
  AutoStraighten, DirBlur and GodRays default to Black today. Under Input on a Read they now
  stretch.
- Blur, Glow, IBlur, IErodeDilate, ErodeDilate, Defocus, IDefocus, ZDefocus and VectorBlur read
  Zero today. Under Input on a Read they now stretch, so a blur no longer darkens a Read's
  border (Q4).
- EdgeDetect, Sharpen, EdgeExtend and Bloom are Nearest today. Under Input on a Black
  input (after a Crop set to Black, say) they now see the black border.
- A Read used as Merge's A now repeats its edge across a larger B, where it read zero
  past its RoD before (Q1). So does a mask, a Map, a UV or a Depth input. To keep an element's
  edge from smearing across a merge, set its upstream Crop or Transform to Black.
- Crop larger than its source follows the source's bit under Input, where it was always black.

### 2.4 Bit-driven sampling

**CPU.** One inline helper resolves a source coordinate against a rect: clamp to the
nearest pixel for Stretch, or report "outside" (read zero) for Black, with an empty
rect always outside. `PassExec::fillBuffer` uses it in place of `Boundary`.
`Resampler::interpolateT` and its relatives use it in place of `clampCoord` and the
`blackOutside` branches. `PassExec::Boundary`, `InputBoundary` (dead) and the
`blackOutside` booleans become `EdgeMode`. The same-position sites of 1.4a (`readChannelRow`
fill values, `readStageSourceRow`, `readSourceImageRow`, `routeRow` and `copyRow`, the time
nodes' `pixelAt`) read through it by the input's own bit.

For separable line filters the mapping is fixed: Stretch means a Nearest fill plus
Neumann line ends, and Black means a Zero fill plus Dirichlet line ends. EdgeDetect
already pairs them this way, so `LineFilter`'s `neumann` is derived from the mode,
not passed separately. ErodeDilate's, Median's and Bilateral's window clamp at the
RoD-clipped buffer stays. Only the fill beyond the source follows the mode.

**Stretch reads need the RoI clamped into the input's RoD.** `EffectInstance` clips an input's
RoI to its RoD, and fetches nothing when that is empty, so a Stretch read wholly outside the
input would have no pixels to repeat. Under Stretch each input's RoI is clamped into that
input's RoD as a band at least one pixel wide (M136.P2.T6). Under Black the input is not
fetched when the window misses it.

**Shortcuts hold only under Black.** Two identity shortcuts rest on "outside reads zero": the
mask-misses-window shortcut (`StagePlan.cpp:653`, `StagedImageEffect.cpp:825`) and Merge's
transparent-A skip (`Merge.cpp:577-584`, `merge.slang:36`). Under Stretch a mask or an A that
misses the window still reads its edge pixel there, so both apply only when the input's bit is
Black.

**Concatenation stops at a differing mode.** Transform concatenation resamples a whole chain
through one filter under one mode (`ResampleExecutor.cpp`, `Resampler::SamplingTransforms`).
It uses the resolved mode and stops at a node whose resolved mode differs from the chain's.

**GPU.** Slang kernels share code through `import` modules, not includes
(`resample.slang:16`, `stmap.slang:1-4`). A new `edgesample.slang` module holds the
coordinate rule and one mode constant. These import it:

- `resamplecore.slang`, which covers resample, coordmap, stmap, iwarp, lensdistortion,
  lenscorrect and splinewarp, and `dirblur.slang`;
- the per-kernel buffer fills: `blur_lines.slang:248`, `blur.slang`,
  `edgedetect.slang:252-253`, `sharpen.slang:62-79`, `edgeextend.slang:65-66, 116-117`,
  `bloom.slang`, `glow.slang`, `morphology.slang:114` (`paddedAt`), `iblur.slang`,
  `ierodedilate.slang`, `defocus.slang:85`, `vectorblur.slang:139` and `zdefocus.slang`;
- the same-position reads: `pixelwrap.slang` (`wrapReadSource`, `wrapMaskValue`),
  `compositestep.slang`, `compositealpha.slang`, `channelroute.slang`, `mapremap.slang`,
  `stmap.slang` (`uvInside`), `timeblend.slang` and `slitscan.slang`.

The fifteen `BlackOutside` flag copies of 1.1 collapse into the one constant.

**No materialised border.** Crop's 1-pixel RoD growth and its forced-black outer row
and column (`Crop.cpp:224-230, 455-463`) go: the bit says black. Transform's
`expandRoD` (`Resampler.cpp:1947-1965`) and LensDistortion's extra margin
(`LensDistortion.cpp:925`) are a different thing. Under Black, the output pixels
just past the transformed edge hold the filter's real anti-aliased fade, not a fill.
Both stay and key off the resolved mode. IWarp's call (`IWarp.cpp:473`) and GodRays' (`GodRays.cpp:562`)
do the same.

### 2.5 Crop

Crop's RoD is the crop rectangle. Its output bit is the resolved mode (2.3).

- **Crop larger than its source**: under Input, the pixels between the source RoD and the crop
  rectangle come from the shared sampler under the *input's* bit. A Stretch source repeats its
  edge. A Black source shows black. A forced Black or Stretch governs that band too, and becomes
  the output bit (Q3). This replaces the always-zero copy path at `ResampleExecutor.cpp:142` for
  Crop.
- **Crop smaller than its source**: nothing is rendered outside the rectangle.
  The output bit tells downstream readers to repeat the rectangle's edge (Stretch)
  or read black (Black). Inside the source but outside the rectangle, Black blacks and Stretch
  repeats the rectangle's edge.
- Softness stays a matte inside the rectangle.

### 2.6 Where the CPU and GPU differ today

They don't, in any surveyed node. Each GPU kernel copies its CPU twin's rule, and
the parity tests hold them to it. ErodeDilate was the suspected exception. It isn't
(see 1.3), so the milestone's statement that its GPU kernel reads zero where the CPU
clamps is stale. The risk is structural instead. There are many hand-written GPU
fills and fifteen flag copies, against one CPU fill and one CPU resampler. Section 2.4
removes that duplication.

## 3. Answers

Approved by the user on 2026-11-13.

- **Q1. Same-position reads of other inputs: every input follows its own edge bit.** Merge's A,
  Dissolve's inputs, ShuffleCopy's routed inputs, every mask input, the Map inputs of IBlur,
  IErodeDilate, IDefocus and EdgeBlur, the UV inputs of IDistort and STMap, ZDefocus's Depth,
  VectorBlur's UV and Depth, and the frames of FrameBlend and the other time nodes repeat their edge when their bit is Stretch
  and read black when it is Black (section 1.4a). The cost is that a Read's edge now repeats
  across a larger Merge B, and an element can smear across a merge, which is intended. To keep an
  element's edge from smearing across a merge, set its upstream Crop or Transform to Black. This
  brings Merge, Dissolve, KeyMix, ShuffleCopy, FrameBlend and the MaskMix stage into scope
  (M136.P4.T8 to P4.T12).
- **Q2. Raw nodes stay out** (section 1.5): they forward the bit and keep their own borders.
- **Q3. A forced mode overrides the input bit and passes on.** Crop, Reformat and Transform
  forced to Black or Stretch govern the whole output, the band between the source region and the
  crop rectangle or format included, and set the output bit to the forced value (2.3, 2.5).
- **Q4. Filters default to Input** (2026-11-02): with Input as the default, Blur, Glow, IBlur
  and IErodeDilate on a Read stop darkening the border. Their goldens are re-recorded once, with
  rounding-only differences re-recorded and not reopened.

## 4. Coverage against the phases

| Node | Task |
|---|---|
| The `NodeMetadata` field | M136.P2.T1 |
| Propagation, OpenFX opacity | M136.P2.T2 |
| Shared CPU sampler | M136.P2.T3 |
| Shared GPU sampler | M136.P2.T4 |
| Info readout | M136.P2.T5 |
| Stretch-aware RoI | M136.P2.T6 |
| The `Stage::EdgeMode` stage | M136.P3.T1 |
| Transform | M136.P3.T2 |
| Reformat | M136.P3.T3 |
| Crop | M136.P3.T4 |
| Blur | M136.P4.T1 |
| EdgeDetect | M136.P4.T2 |
| ErodeDilate (Erode, Dilate) | M136.P4.T3 |
| CornerPin, ITransform, ICornerPin, IWarp, IDistort, STMap, LensDistortion; Mirror's hard-wired flag | M136.P4.T4 |
| Sharpen, EdgeExtend | M136.P4.T5 |
| Glow, Bloom | M136.P4.T6 |
| IBlur, IErodeDilate, EdgeBlur | M136.P4.T7 |
| Mask inputs, the MaskMix stage, the mask-misses-window shortcut | M136.P4.T8 |
| Merge, KeyMix, Dissolve | M136.P4.T9 |
| ShuffleCopy and Shuffle | M136.P4.T10 |
| Map inputs of IBlur, IErodeDilate, IDefocus, EdgeBlur | M136.P4.T11 |
| UV inputs of IDistort and STMap | M136.P4.T12 |
| LensCorrect, SplineWarp, AutoStraighten | M136.P4.T13 |
| DirBlur, GodRays | M136.P4.T14 |
| Median, Bilateral, AdjustBBox | M136.P4.T15 |
| Defocus, IDefocus, ZDefocus, VectorBlur (source reads) | M136.P4.T16 |
| DenoiseProfiled; ToneEqualizer and ColorEqualizer (no stage: 1.5) | M136.P4.T17 |
| Position | none: forwards the bit |
| ZDefocus Depth, VectorBlur UV and Depth, FrameBlend, TimeBlur, SlitScan same-position reads | **none yet** |
| NeuralUpscale | **none yet** |
| Per-node matrix, GUI check | M136.P5.T1, M136.P5.T2 |
