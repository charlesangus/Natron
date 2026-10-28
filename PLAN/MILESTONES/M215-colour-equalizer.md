# M215 - Colour Equalizer

A native `ColorEqualizer` node ported from darktable's `src/iop/colorequal.c` (about 4k lines, with OpenCL) at `DARKTABLE_REF`. It makes hue-keyed edits in darktable UCS: hue shift, saturation and brightness curves over eight hue nodes (red, orange, yellow, green, cyan, blue, lavender, magenta). The selection is smoothed by a guided filter from M209 - Edge-Aware Filter Library, so edits don't break up across noisy chroma. The colour-space helpers come from M214 - Colour Balance RGB's `DtColorSpaces.h`.

Every user-facing control and darktable default is ported (`port-means-every-control`).

Deps: M209 - Edge-Aware Filter Library, M214 - Colour Balance RGB. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`ColorEqualizer`** in `Engine/Nodes/Color/`, plain ID, major 1. Inputs: Source and an optional Mask (the host MaskMix).
- **Every control** of the module's newest params version and GUI:
  - the hue, saturation and brightness value at each of the eight nodes
  - `whiteLevel`, `chromaSize`, `paramSize`, `useFilter` and `hueCurveSmoothing`
  - every remaining option P1.T1 lists
  - darktable's presets
- **Three periodic parametric curve knobs** (hue, saturation, brightness against hue). Each has eight fixed-x control points whose y values are the params, and they interpolate the way darktable does. Like `HueCorrect`, these use Natron's parametric knob in periodic mode. If a fixed-x constraint can't be expressed, each curve's eight values are sliders, with a read-only curve preview.
- **`output`:** Result, or Hue/Saturation/Brightness weight. This replaces darktable's darkroom mask display.
- **Working-space handling** as in M214: convert to linear Rec.2020 through OCIO and back.
- CPU and GPU paths, darktable reference parity, a params-to-knob audit, a doc, an icon.

**Out:**
- darktable's on-image colour picker that selects a node by hue: no milestone owns it. The doc notes it as a follow-up for M165 - Rich Viewer Overlays.

**Edge cases:**
- **Near-achromatic pixels:** their weights fall off by chroma as darktable's do, so greys are never hue-shifted.
- **Region-of-interest expansion:** by the guided-filter radius when `useFilter` is on.
- **Proxy scale:** the filter radius scales with render scale.

## Phase 215.1: Node

- [ ] M215.P1.T1 — ColorEqualizer node and curve knobs (CPU)
  - files: new `Engine/Nodes/Color/ColorEqualizer.h`, `Engine/Nodes/Color/ColorEqualizer.cpp`, `Engine/Nodes/Color/ColorEqualizer.reg.inc`
  - approach:
    - Port `process` with its guided-filter smoothing on M209's `GuidedFilter`, the hue-node interpolation and the chroma weighting from `colorequal.c`.
    - Build the three parametric knobs per the Scope. Find how the parametric knob's periodic mode is used before inventing one: the OFX HueCorrect plug-in is a host consumer, so grep `KnobParametric` and periodic.
  - verify: P1.T3's reference jobs pass.
  - size: L

- [ ] M215.P1.T2 — GPU path
  - files: new `Engine/Gpu/Kernels/colorequal.slang`, `colorequal.cmake`, `Engine/Nodes/Color/ColorEqualizer.cpp`, new `Tests/Native/NativeColorEqualizerGpu_Test.cpp`
  - approach: port from darktable's `data/kernels/colorequal.cl`, on `dtcolorspaces.slang` and M209's GPU guided filter.
  - verify: `ctest -R NativeColorEqualizerGpu` passes on lavapipe within the doc's tolerance.
  - size: L

- [ ] M215.P1.T3 — Reference parity, knob audit and docs
  - files: `tools/raw/make-reference.py`, `Tests/fixtures/raw/reference/` (new), new `Tests/Native/NativeColorEqualizer_Test.cpp`, `Engine/Nodes/Color/docs/ColorEqualizer.md`
  - approach:
    - darktable-cli jobs: per-node hue, saturation and brightness edits, filter on and off, and presets.
    - Audit `dt_iop_colorequal_params_t` against the knobs.
    - Run identity checks, then write the doc and add an icon (`Gui/Resources/Images/NativeNodes/`).
  - verify: `ctest -R NativeColorEqualizer` passes within the doc's `Tolerance:`.
  - size: L

**Verification gate:**
- The full build and ctest pass, including both ColorEqualizer suites on lavapipe and RADV.
- Under Xvfb, the panel's three curves and a hue-keyed edit on a plate are captured against darktable's render. Screenshots are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.

## Decisions

- 2026-10-27: release gate green (run 40620856879) with the whole-frame guided filter and the 640x480 Noise region test; next: GUI check and screenshots, then AppImage/UAT.
- 2026-10-28 — PM: output must not depend on the requested region. The guided filter's subsampling grid (factors 2–4 at large sizes) is anchored to the image origin and the input region padded to grid-aligned bounds, matching darktable's whole-image subsampling. Defaults are not an exact identity in darktable either (curve through eight 1s dips to 0.99854); tests check exact identity with flat curves and bound the default dip.
