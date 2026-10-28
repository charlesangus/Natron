# Edge policy: what an image is outside its bounds

Status: draft for approval (M136 - Edge Policy Investigation, Phase 136.1), 2026-10-28.

Today each native node decides for itself what lies outside an input's region of
definition (RoD). This note surveys every native node that reads outside its own
output position, then proposes one rule: every image carries a binary *edge mode*
in its `NodeMetadata`, and every consumer reads outside an input's RoD through one
CPU helper and one GPU sampler driven by that mode.

Paths are relative to the repository root. Line numbers are as of `main` at
9185c8bae.

## 1. Survey

Terms used below:

- **Zero**: taps outside the source RoD read 0 (Dirichlet).
- **Nearest**: taps outside the source RoD read the nearest source pixel (Neumann, clamp to edge).
- **Line ends**: separable IIR and box filters see a whole buffer line. Past its ends they read
  either 0 or the end sample (`BlurKernels::LineFilter` `neumann`, `Engine/Nodes/Filter/BlurKernels.h:58-59`).
  Under Nearest the buffer is already filled with clamped pixels, so Neumann line ends
  continue the same edge.

### 1.1 Shared machinery

| Piece | CPU | GPU | Outside rule today |
|---|---|---|---|
| Pass executor buffer fill | `PassExec::fillBuffer`, `Engine/Nodes/Image/PassExecutor.cpp:336-400`; `PassExec::Boundary {Zero, Nearest}`, `PassExecutor.h:217-222`; `PassList::boundary` defaults to Zero, `PassExecutor.h:432` | No shared fill. Each kernel fills its own buffer (see 1.3). | Per node: `list.boundary` |
| Resampler (Transform family, warps) | `Resampler::interpolateT` with `blackOutside`; `clampCoord`, `Engine/Nodes/Image/Resampler.cpp:200-204, 256-258, 279-283, 325-333`; `outsideBounds`, `Resampler.cpp:521-526` | `resamplecore.slang` (`kFlagBlackOutside` at :19, `ResampleSource::blackOutside` at :219, clamped tap at :321-338), imported by `resample.slang`, `coordmap.slang`, `stmap.slang`, `iwarp.slang`, `lensdistortion.slang` | `blackOutside` true: Zero; false: Nearest |
| Resample executor copy path (whole-pixel offset: Crop, Position, Mirror) | `Engine/Nodes/Image/ResampleExecutor.cpp:142` | Copy kernel through `GpuResample` | Always Zero |
| Black-outside RoD growth | `Resampler::expandRoD`, `Resampler.cpp:1780-1800`, called at `:1875`; also `IWarp.cpp:464`, `LensDistortion.cpp:914` | n/a | One output pixel per finite side when `blackOutside` |
| Edge-mode flags in GPU params | — | `GpuResample.cpp:18, 272`; `Transform/WarpGpu.h:86`; `IWarp.cpp:69`; `LensDistortion.cpp:1167`; `stmap.slang:16`; `iwarp.slang:15`; `lensdistortion.slang:13` | Five copies of the same `= 2` bit |
| `InputBoundary {Zero, Nearest}` | `Engine/Nodes/NodeSpec.h:56-59`, field at `:102` | — | **Dead.** No code reads `InputSpec::boundary`. |
| Shared knob strings | `kResamplerParamFilterBlackOutside*`, `Resampler.h:50-52` | — | — |

### 1.2 Transform and warp nodes (Resampler family)

| Node | CPU path | GPU path | Outside source today | Knob (default) |
|---|---|---|---|---|
| Transform | ResampleExecutor → Resampler | `resample.slang` | Knob | `black_outside` (on), `Transform.cpp:258-263` |
| Reformat | ResampleExecutor → Resampler | `resample.slang` | Knob | `black_outside` (off), `Reformat.cpp:465-470` |
| Crop | ResampleExecutor copy path plus a matte, `Crop.cpp:178-230` | `GpuResample` copy | Zero beyond the source, always (`ResampleExecutor.cpp:142`); with Black Outside the RoD grows by 1 px (`Crop.cpp:455-463`) and the outermost crop row and column are forced black (`Crop.cpp:223-229`) | `blackOutside` (off), `Crop.cpp:296-301` |
| CornerPin | ResampleExecutor → Resampler | `resample.slang` | Knob | `blackOutside` (on), `CornerPin.cpp:316-321`, name `CornerPinKnobs.h:67` |
| ITransform | IWarp → `resampleRowFromCoords` | `iwarp.slang` | Transform's knob, `ITransform.cpp:279` | Transform's `black_outside` (on) |
| ICornerPin | IWarp → `resampleRowFromCoords` | `iwarp.slang` | CornerPin's knob, `ICornerPin.cpp:232` | CornerPin's `blackOutside` (on) |
| IWarp (base) | `IWarp.cpp:856, 938` | `iwarp.slang:224` | `WarpSettings::blackOutside`, `IWarp.h:125` | via ITransform/ICornerPin |
| IDistort | `resampleRowFromCoords`, `IDistort.cpp:670, 686` | `coordmap.slang` via `WarpGpu` | Knob | `black_outside` (off), `IDistort.cpp:193-198` |
| STMap | `resampleRowFromCoords`, `STMap.cpp:734` | `stmap.slang:243` | Knob, only when Wrap U/V is None (`Resampler.h:345`); Clamp/Repeat/Mirror fold the coordinate first | `black_outside` (on), `STMap.cpp:270-275`; Wrap U/V `STMap.cpp:208, 217` |
| LensDistortion | `resampleRowFromCoords`, `LensDistortion.cpp:1117` | `lensdistortion.slang:72` | Knob; adds 1 px margin when on, `LensDistortion.cpp:914` | `black_outside` (on), `LensDistortion.cpp:416-421` |
| Mirror | ResampleExecutor, impulse filter | `resample.slang` | Hard-wired `blackOutside = true`, `Mirror.cpp:83`. Whole-pixel flips of the RoD: no output pixel inside the RoD reads outside the source. | none |
| Position | ResampleExecutor offset | copy | Whole-pixel shift of the RoD: no outside reads | none |

### 1.3 Filters

| Node | CPU source read | CPU line ends | GPU | Outside source today | Knob |
|---|---|---|---|---|---|
| Blur | `fillBuffer`, default Zero (`Blur.cpp:219`, no `list.boundary`) | Dirichlet: `neumann = false`, `Blur.cpp:237-262` | `blur.slang` (FIR), `blur_lines.slang:248` (zero outside source), `:297-477` (line ends from `neumann`) | Zero | none |
| EdgeDetect | Nearest, `EdgeDetect.cpp:938` | Neumann, `EdgeDetect.cpp:915-916` | `edgedetect.slang:236-253` (own clamped fill), `blur_lines` with `neumann` | Nearest, fixed | none |
| ErodeDilate | `fillBuffer`, Zero; the min/max window clamps to the buffer, which is clipped to the RoD (`ErodeDilate.h:128-130`) | window clamp (`ErodeDilate.h:61-63`) | `morphology.slang:12-13, 114` (`paddedAt`, zero outside source), `:171` | Zero beyond the source, window clamped at the RoD. **CPU and GPU agree today**: `NativeErodeDilateGpu_Test.cpp:366` (`MatchesTheCpuRender`) checks them bit for bit with the source off the format origin. The node help still says "Neumann" (`ErodeDilate.h:141, 146`). | none |
| Sharpen | Nearest, `Sharpen.cpp:503` | Neumann, `Sharpen.cpp:252` | `sharpen.slang:61-81` (own clamped fill), then `recordSeparableBlur` / `recordLineBlur`, `Sharpen.cpp:328-333, 424-442` | Nearest, fixed | none |
| EdgeExtend | Nearest, `EdgeExtend.cpp:516` | Neumann, `EdgeExtend.cpp:511` | `edgeextend.slang:62-70` (own clamped fill), `EdgeExtend.cpp:375-378` | Nearest, fixed | none |
| Bloom | Nearest, `Bloom.cpp:66-75` | `BloomKernels.cpp:164-169` (`resolveIndex`, nearest or zero), `plan->nearest` defaults true at `:224` | `bloom.slang:40-41, 68` | Nearest, fixed | none |
| Glow | Zero, `Glow.cpp:1016`; its downsample reads zero outside, `Glow.cpp:351, 839` | — | `glow.slang:142`, `glow_frame.slang` | Zero, fixed | none |
| IBlur | `fillBuffer`, default Zero (`IBlur.cpp:650`); summed-area table zero outside, `VariableBlurKernels.h:43`, `VariableBlurKernels.cpp:38` | — | `iblur.slang` | Zero, fixed ("black outside the image", `IBlur.cpp:76`). Map input reads black outside its image. | none |
| IErodeDilate | `fillBuffer`, default Zero (`IErodeDilate.cpp:1565`) | — | `ierodedilate.slang:203` | Zero, fixed. Map input reads black outside its image (`IErodeDilate.cpp:895`). | none |
| EdgeBlur | Matte filled with EdgeDetect's boundary (Nearest), `EdgeBlur.cpp:506-530`; blur through IBlur, whose Map reads 0 outside EdgeDetect's RoD (`EdgeBlur.cpp:619`) | as EdgeDetect, IBlur | as EdgeDetect, IBlur | Nearest for the matte, Zero for the blurred source | none |

### 1.4 Nodes that read no pixel outside their own position

Colour nodes (`Engine/Nodes/Color/*`), keyers (`Keyer/*`), channel nodes
(`Channel/*`), metadata nodes, OCIO nodes, Clamp, Posterize, Premult/Unpremult,
Merge, Dissolve, Switch, generators (Constant, CheckerBoard, Noise), Read and
the Scene/USD render nodes. They only forward or originate the bit (section 2).
Merge, Dissolve and every node's mask input still read *another* input outside
that input's RoD at the same position. Question Q1 covers that case.

### 1.5 Not covered by this milestone (proposed)

- **Raw nodes** (`Engine/Nodes/Raw/*`, plus the darktable helpers in
  `Filter/EdgeAware/*`, which only `Raw/RawHighlightsLaplacian.cpp` uses). They
  work on the sensor frame before `RawToImage`. Their borders are
  darktable-parity choices: `RawHotPixels.cpp:352` Zero, `EdgeAware/BoxFilter.h:38-41`
  shrinking window, `EdgeAware/Wavelets.h:46-48` mirror and clamp. They forward
  the bit and keep their own borders. See Q2.
- **Deep nodes**: out of scope (M21 - Deep Tier-2 And Bridges).
- **Roto, Bezier, tracker**: out of scope.

## 2. The model

### 2.1 The bit

`NodeMetadata` gains `enum class EdgeMode : unsigned char { Stretch, Black }` and one
output-level field `edgeMode`, default `Stretch`, in `NodeMetadataPrivate`
(`Engine/NodeMetadata.cpp:50-71`). Since M167 - Per-Frame Node Metadata, metadata
is per (time, view), so the bit rides that. `NodeMetadata::operator==`
(`NodeMetadata.h:57`) compares it. That comparison is what makes a metadata change
visible.

Cache invalidation needs nothing extra. The bit only changes through a knob or a
connection, and either one moves the node's content hash. The hash folds in its
inputs' hashes (`Node::computeHashInternal`, `Engine/Node.cpp:848-874`, via
`computeContentHash(projectDigest, inputs)`). So every downstream image key moves
with it. This settles the milestone's first assumption.

### 2.2 Who sets it

- **Sources** (Read, generators, USD render, RawToImage, DeepToImage) report Stretch.
- **Forwarding** happens in `EffectInstance::getDefaultMetadataFromInputs`
  (`Engine/EffectInstance.cpp:6730-6900`). That is where the format is chosen:
  the first non-optional connected input, else the first optional one (`:6819`).
  The edge mode uses the same input. For Merge that input is B, so Merge, Keymix
  and Copy need no code. Native nodes inherit this default, so the work is here
  rather than in `NativeEffectBase`.
- **Identity**: `EffectInstance::applyIdentityRule` (`EffectInstance.cpp:7314`) already
  returns the picked input's metadata for Switch, Dissolve at an end, and any node at
  identity, so the bit follows the picked input.
- **OpenFX** effects report Stretch in `OfxEffectInstance::getPreferredMetadataForInputs`
  (`Engine/OfxEffectInstance.cpp:2155-2240`). One exception: an OpenFX node at identity
  goes through the identity rule first (`EffectInstance.cpp:7314`) and forwards its
  input's bit. That is the input's own image, so this note keeps it.
- **Nodes with an Edge knob** set their output bit to the knob's resolved mode.

### 2.3 The Edge knob

One shared helper (`ExtentKnobs`) creates a choice knob `edge_mode`, labelled
"Edge Mode", with Input (default), Black and Stretch. It also resolves the knob
against the primary input's bit. It replaces `kResamplerParamFilterBlackOutside*`,
Crop's `blackOutside` and CornerPin's `blackOutside`.

The knob goes on every node in 1.2 except Mirror and Position (they read nothing
outside), and on every node in 1.3. A filter's output bit is its resolved mode
too. A Blur set to Black on a Stretch input fades to black over its grown RoD, and
downstream must then see black beyond it.

Default behaviour changes this causes (no backward compatibility):

- Transform, CornerPin, ITransform, ICornerPin, STMap and LensDistortion default to
  Black today. Under Input on a Read they now stretch.
- Blur, Glow, IBlur, IErodeDilate and ErodeDilate read Zero today. Under Input on a
  Read they now stretch, so a blur no longer darkens a Read's border.
- EdgeDetect, Sharpen, EdgeExtend and Bloom are Nearest today. Under Input on a Black
  input (after a Crop set to Black, say) they now see the black border.

### 2.4 Bit-driven sampling

**CPU.** One inline helper resolves a source coordinate against a rect: clamp to the
nearest pixel for Stretch, or report "outside" (read zero) for Black, with an empty
rect always outside. `PassExec::fillBuffer` uses it in place of `Boundary`.
`Resampler::interpolateT` and its relatives use it in place of `clampCoord` and the
`blackOutside` branches. `PassExec::Boundary`, `InputBoundary` (dead) and the
`blackOutside` booleans become `EdgeMode`.

For separable line filters the mapping is fixed: Stretch means a Nearest fill plus
Neumann line ends, and Black means a Zero fill plus Dirichlet line ends. EdgeDetect
already pairs them this way, so `LineFilter`'s `neumann` is derived from the mode,
not passed separately. ErodeDilate's window clamp at the RoD-clipped buffer stays.
Only the fill beyond the source follows the mode.

**GPU.** Slang kernels share code through `import` modules, not includes
(`resample.slang:16`, `stmap.slang:1-4`). A new `edgesample.slang` module holds the
coordinate rule and one mode constant. These import it:

- `resamplecore.slang`, which covers resample, coordmap, stmap, iwarp and lensdistortion;
- the per-kernel buffer fills: `blur_lines.slang:248`, `blur.slang`,
  `edgedetect.slang:236-253`, `sharpen.slang:61-81`, `edgeextend.slang:62-70`,
  `bloom.slang:68`, `glow.slang`, `morphology.slang:114` (`paddedAt`), `iblur.slang`
  and `ierodedilate.slang`.

The five `kFlag*BlackOutside = 2` copies collapse into the one constant.

**No materialised border.** Crop's 1-pixel RoD growth and its forced-black outer row
and column (`Crop.cpp:223-229, 455-463`) go: the bit says black. Transform's
`expandRoD` (`Resampler.cpp:1780-1800`) and LensDistortion's extra margin
(`LensDistortion.cpp:914`) are a different thing. Under Black, the output pixels
just past the transformed edge hold the filter's real anti-aliased fade, not a fill.
Both stay and key off the resolved mode. IWarp's call (`IWarp.cpp:464`) does the same.

### 2.5 Crop

Crop's RoD is the crop rectangle. Its output bit is the resolved mode.

- **Crop larger than its source**: pixels between the source RoD and the crop
  rectangle come from the shared sampler under the *input's* bit. A Stretch source
  repeats its edge. A Black source shows black. This replaces the always-zero copy
  path at `ResampleExecutor.cpp:142` for Crop.
- **Crop smaller than its source**: nothing is rendered outside the rectangle.
  The output bit tells downstream readers to repeat the rectangle's edge (Stretch)
  or read black (Black).
- Softness stays a matte inside the rectangle.

Section 3 asks how "larger than its source" interacts with a forced Black or Stretch
(Q3).

### 2.6 Where the CPU and GPU differ today

They don't, in any surveyed node. Each GPU kernel copies its CPU twin's rule, and
the parity tests hold them to it. ErodeDilate was the suspected exception. It isn't
(see 1.3), so the milestone's statement that its GPU kernel reads zero where the CPU
clamps is stale. The risk is structural instead. There are eleven hand-written GPU
fills and five flag copies, against one CPU fill and one CPU resampler. Section 2.4
removes that duplication.

## 3. Open questions for approval

- **Q1. Same-position reads of other inputs.** Merge's A, Dissolve's inputs, Copy,
  every mask input, and the Map inputs of IBlur, IErodeDilate, IDistort and STMap
  read zero outside that input's RoD today. In Nuke these repeat the edge
  (`.local/docs/nuke/html/content/reference_guide/transform_nodes/blackoutside.html`:
  "stripes or other anomalies caused by the edge pixels of the bounding box being
  replicated"), which is why Nuke users add BlackOutside before a Merge.
  Proposed: the bit governs only a node's *source* input when that node samples
  outside its position (sections 1.2 and 1.3). Masks, maps and same-position merge
  inputs keep reading zero. That avoids smearing a Read's edge across a Merge.
  Confirm, or choose Nuke's behaviour, which would bring Merge, Dissolve, Copy and
  the MaskMix stage into scope.
- **Q2. Raw nodes stay out** (section 1.5): they forward the bit and keep their
  darktable borders. Confirm.
- **Q3. Crop larger than its source with a forced mode.** Proposed: the knob sets
  only the output bit (outside the crop rectangle). The area between the source RoD
  and the rectangle always follows the input's bit, as the Scope's acceptance case
  states. The alternative: a forced knob also governs that area.
- **Q4. Filters' default.** With Input as the default, Blur, Glow, IBlur and
  IErodeDilate on a Read stop darkening the border. That follows from "sources
  stretch", but it changes every existing golden for those nodes at the border.
  Confirm before Phase 136.4 re-records them.

## 4. Coverage against the phases

| Node | Task |
|---|---|
| Transform | M136.P3.T2 |
| Reformat | M136.P3.T3 |
| Crop | M136.P3.T4 |
| Blur | M136.P4.T1 |
| EdgeDetect | M136.P4.T2 |
| ErodeDilate (Erode, Dilate) | M136.P4.T3 |
| CornerPin, ITransform, ICornerPin, IDistort, STMap, LensDistortion; Mirror's hard-wired flag | M136.P4.T4 (new) |
| Sharpen, EdgeExtend | M136.P4.T5 (new) |
| Glow, Bloom | M136.P4.T6 (new) |
| IBlur, IErodeDilate, EdgeBlur | M136.P4.T7 (new) |
| Position | none: forwards the bit |
