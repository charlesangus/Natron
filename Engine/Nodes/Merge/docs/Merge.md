# Merge

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Merge | exact / arithmetic / transcendental, per operator | `toleranceOf()` (`NativeMergeGpu_Test`), `mergegolden::operatorTolerance()` (`MergeOpsOracle_Test`), `kTolerance` (`NativeMerge_Test`) | No OFX reference: the CPU operators are held to the Slang module, the GPU to the CPU, and the node to expected pixels computed in its test |

- **CPU against the Slang module** (`Tests/Gpu/MergeOpsOracle_Test.cpp`, the `CPP`-compiled
  `mergeops_oracle.slang`): exact for every operator except color-burn, color-dodge,
  conjoint-over, disjoint-over, divide, geometric and reflect (1e-6 relative, they divide) and
  freeze, hypot, hue, saturation, color and luminosity (1e-5 relative, roots and colour blends).
- **GPU against CPU** (`Tests/Native/NativeMergeGpu_Test.cpp`): exact for the Porter-Duff
  operators that only multiply and add (atop, copy, in, mask, matte, out, over, stencil, under,
  xor), since `precise` keeps the GPU's multiplies apart from their adds as the CPU has them;
  arithmetic for conjoint-over and disjoint-over, which divide (Vulkan division is 2.5 ulp);
  transcendental for every blend mode.
- **The node against expected pixels** (`Tests/Native/NativeMerge_Test.cpp`): 1e-6 relative, the
  rounding of a fused or reordered operation, for folds, layers, alpha sources, mask and mix.
- **Goldens** (`NativeGolden_Test`, `MergeOver`, `MergePlusOperation`, `MergeA2`, `MergeA2Plus`,
  `MergeMultiply`): bit for bit on the recording configuration.

`Merge` (`Merge.h`, class `NativeMerge` with op `NativeMergeOp`, version 1, grouping Merge) is a
clean-sheet node: Nuke's Merge feature for feature, minus the pieces listed under "Dropped", plus
the openfx-misc extra operators and merging several layers at once. It is an `OpKind::Composite`
op with the `MaskMix` and `PassThroughUnprocessed` stages, so it runs on the CPU and on the GPU,
and a GPU-resident region flows through it. It is in the inspector list and bound to the M
shortcut.

## Inputs

| Number | Label | Role |
|---|---|---|
| 0 | B | The background, the base of the fold |
| 1 | A | The first foreground |
| 2 | Mask | `MaskMix` mask |
| 3..65 | A2..A64 | More foregrounds (Variadic) |

A is merged over B, then A2 over the result, then A3, and so on: one operator call, and on the
GPU one dispatch, per connected A input. Mask and mix apply once, to the result.

- **No A connected.** For an operator that leaves B unchanged under a transparent A (atop,
  color-dodge, exclusion, from, matte, over, plus, screen, stencil, under, xor) the node is an
  identity of B, or black with no B either. Any other operator merges a transparent A (A = 0,
  a = 0) over B.
- **No B connected.** B reads zero, as in Nuke.
- **A stretch with nothing in A.** For the operators above, a chunk of row where every A value
  and alpha is zero, such as one outside every A's region of definition, copies B with no
  operator call.

## Operators

A and B are the premultiplied channel values, a and b their alphas. The option names are Nuke's
exactly; the openfx-misc extras keep their openfx-misc names. The default is `over`.

| Operator | Formula |
|---|---|
| atop | Ab + B(1 - a) |
| average | (A + B) / 2 |
| color | SetLum(b·A, a·Lum(B)) |
| color-burn | 1 - (1 - B)/A; A if A <= 0 |
| color-dodge | B/(1 - A); A if A >= 1 |
| conjoint-over | A if a >= b; A + B if b <= 0; else A + B(1 - a/b) |
| copy | A |
| difference | \|A - B\| |
| disjoint-over | A if a >= 1; A + B if a + b <= 1; else A + B(1 - a)/b |
| divide | A/B; 0 if B = 0 |
| exclusion | A + B - 2AB |
| freeze | 1 - ssqrt(1 - A)/B, with ssqrt the signed square root; 0 if B <= 0 |
| from | B - A |
| geometric | 2AB/(A + B); 0 if A + B = 0 |
| grain-extract | B - A + 0.5 |
| grain-merge | B + A - 0.5 |
| hard-light | 2AB if A < 0.5; else screen(2A - 1, B) |
| hue | SetLum(SetSat(A, a·Sat(B)), a·Lum(B)) |
| hypot | sqrt(A² + B²) |
| in | Ab |
| luminosity | SetLum(a·B, b·Lum(A)) |
| mask | Ba |
| matte | Aa + B(1 - a) |
| max | max(A, B) |
| min | min(A, B) |
| minus | A - B |
| multiply | AB |
| out | A(1 - b) |
| over | A + B(1 - a) |
| overlay | 2AB if B < 0.5; else screen(A, 2B - 1) |
| pinlight | min(max(B, 2A - 1), 2A) |
| plus | A + B |
| reflect | A²/(1 - B); B if B >= 1 |
| saturation | SetLum(SetSat(B, b·Sat(A)), a·Lum(B)) |
| screen | A + B - AB if A <= 1 and B <= 1; else max(A, B) |
| soft-light | B(2A + B(1 - AB)) if AB < 1; else 2AB |
| stencil | B(1 - a) |
| under | A(1 - b) + B |
| xor | A(1 - b) + B(1 - a) |

- **Separable operators** treat every channel alike, the alpha included: the output alpha is the
  operator applied to a over b.
- **hue, saturation, color and luminosity** are the PDF non-separable blend modes, rewritten on
  premultiplied values through SetLum's and SetSat's scaling so they never divide by an alpha.
  They blend the colour channels with the first channel's alphas, and their alpha follows over,
  a + b(1 - a). A layer with fewer than three channels reads its missing colour channels as zero.
  With a per-channel alpha layer on either side they are a render error naming the operator,
  since no one channel's alpha is the alpha of a whole colour.

`Engine/Gpu/Kernels/mergeops.slang` is the canonical source of the formulas and
`Engine/Nodes/Merge/MergeOps.{h,cpp}` their CPU twin; each operator's formula is also its option
hint in the Operation menu.

### HDR-safe deviations

The formulas are Nuke's (and openfx-misc's for the extras), made HDR-safe: where a formula clamps,
flips sign, divides by zero or jumps for values above 1 or below 0, Merge uses a continuous,
unbounded variant, so scene-linear values pass through. There is no legacy toggle. The same table
heads `mergeops.slang`.

| Operator | Nuke / openfx-misc | Merge |
|---|---|---|
| color-burn | 1 - min(1, (1-B)/A), A if A <= 0 | 1 - (1-B)/A, A if A <= 0 (no clamp) |
| color-dodge | min(1, B/(1-A)), A if A >= 1 | B/(1-A), A if A >= 1 (no clamp) |
| divide | A/B, 0 if A < 0 and B < 0 | A/B, 0 if B = 0 (two negatives give a positive) |
| multiply | AB, A if A < 0 and B < 0 | AB (two negatives give a positive) |
| screen | A+B-AB if A or B <= 1, else max(A, B) | A+B-AB if A and B <= 1, else max(A, B) |
| geometric | 2AB/(A+B) | 2AB/(A+B), 0 if A+B = 0 |
| hard-light | multiply (negatives jump), screen | plain multiply, screen as above |
| overlay | multiply (negatives jump), screen | plain multiply, screen as above |
| soft-light | openfx-misc: SVG piecewise | Nuke's: B(2A+B(1-AB)) if AB < 1, else 2AB |
| pinlight | max(B, 2A-1) if A >= 0.5, else min(B, 2A) | min(max(B, 2A-1), 2A) |
| reflect | min(1, A²/(1-B)), 1 if B >= 1 | A²/(1-B), B if B >= 1 (no clamp) |
| freeze | max(0, 1 - sqrt(max(0, 1-A))/B), 0 if B <= 0 | 1 - ssqrt(1-A)/B, 0 if B <= 0, ssqrt signed |
| hue, saturation, color, luminosity | unpremultiplied blend, colour clipped to [0, a] | premultiplied form, no clip |

Every guard that remains sits on a pole of its formula, where no finite choice is continuous.
In screen, both forms equal 1 on the edge of the unit square, so taking max(A, B) as soon as
either side exceeds 1 joins them without the jump Nuke's "or" leaves where both cross 1.

## Layers

- **A Layer** (`aLayer`) is the layer every A input is read in, and **B Layer** (`bLayer`) the
  layer B is read in; both default to `rgba`. The output is the node's **Layer** knob
  (`layer`), a target that may create a layer and that follows B Layer whenever the user changes
  B Layer. A layer an input lacks reads as zero.
- **Layers** (`layers`) picks what is merged beside the A layer into the output layer:
  - **Single** (default): only A Layer over B Layer into the output layer.
  - **All matching**: also every layer present on B and on any connected A input, paired by ID
    and each read and written in itself.
  - **List**: also the layers of **Also Merge** (`alsoMerge`, B's layers, Nuke's "also merge"),
    each read and written in itself; a layer a side lacks reads as zero there. Also Merge is
    shown in List mode only.
- In every mode, every other layer of B passes through unchanged. Each merged layer renders as
  its own plane, with A and B read in that plane, so the GPU path stays one layer per dispatch.

## Alpha sources

Each side has its own **Alpha Source** (`aAlphaSource`, `bAlphaSource`), which gives a, or b,
for each merged channel:

- **Universal** (default): one channel, **A Alpha** / **B Alpha** (`aAlpha`, `bAlpha`, default
  `rgba.A`), is the alpha of every merged channel of every merged layer. None gives an alpha of 0.
- **Per-layer**: each merged layer's own channel named `A` or `alpha` (in any case) is its alpha.
  A layer without one takes the universal channel.
- **Per-channel**: an alpha layer, **A Alpha Layer** / **B Alpha Layer** (`aAlphaLayer`,
  `bAlphaLayer`), whose channel *i* is the alpha of merged channel *i*, for example `alphaRGB`
  for `beauty.RGB`. None, or a layer the input lacks, gives an alpha of 0. Its channel count must
  match the merged layer's.

The output alpha:

- With **Universal**, the universal alpha channel is merged with the operator's alpha rule when
  it is part of the output layer.
- With **Per-layer**, each layer's alpha channel is merged as part of that layer.
- With **Per-channel** on B, B's alpha layer, which the node then also produces, is written the
  merged alpha the output layer's fold carries: each channel of B's alpha layer merged with A's
  alpha for that channel by the operator's alpha rule. A's alpha there is whatever its own source
  gives: its alpha layer's channel when per channel, else its universal or per-layer alpha
  broadcast, so Over with a universal A alpha a writes a + b × (1 − a).

An A or B input that is connected but lacks the layer it is read in has zero pixels there, yet
keeps the alpha its source gives: with several A inputs, one lacking **A Layer** still attenuates
the result under Over by its alpha. Only a disconnected input reads zero pixels and zero alpha.

## Other knobs

- **Set BBox To** (`bbox`): `union` (default) of B and every connected A input, their
  `intersection`, `A` (the union of every connected A input) or `B`. An identity, or a mix of 0,
  gives B's region.
- **Metadata From** (`metadataFrom`): `B` by default, or `A` (input 1, not A2 onwards). When the
  chosen input is not connected the other one's is used.
- **Range From** (`rangeFrom`): the frame range, from `B` by default or `A`, with the same
  fallback.
- **Mask** and **Mix**: the `MaskMix` stage's mask channel, invert and mix.

## Presets

Plus, Matte, Multiply, In, Out, Screen, Max, Min and Difference are Python items in the node
menu's Merge submenu (`Gui/Resources/PyPlugs/initGui.py`), so the toolbox, the node graph's
right-click menu and the tab menu all list them. Each creates `Merge` with its `operation` set;
they are not plug-ins, and projects only ever hold `Merge`.

## GPU

`canRenderOnGpu` accepts every operator. The kernel (`Engine/Gpu/Kernels/merge.slang`, importing
`mergeops`) is wrapped by the Composite fold, one dispatch per connected A input, and the result
stays resident. A non-separable operator needs three or four components and renders on the CPU
otherwise. A folded A input that is also the mask input renders on the CPU, the framework's rule.

## Dropped

- **Alpha masking** (`screenAlpha`, the old node's "Alpha masking"): blend operators process
  alpha like any other channel; mask or premultiply upstream instead.
- **Mask fringe, inject and video colorspace** (Nuke): masking is the `MaskMix` stage's mask
  channel, invert and mix only.
- **The A/B/output channel checkboxes** (`AChannels`, `BChannels`, `OutputChannels`): replaced
  by the layer knobs and the Layer knob's channels.
- **The nine preset plug-ins** (`net.sf.openfx.Merge<Plus|Matte|...>`) and
  `net.sf.openfx.MergePlugin` itself: there is no compatibility with the old node, whose
  formulas were openfx-misc's. A project that uses an old ID loads the missing-plugin node.
- **Metadata from A by default**: B, the main input, is now the default metadata source.
