# M212 - Tone Equalizer

A native `ToneEqualizer` node ported from darktable's `src/iop/toneequal.c` (about 4k lines, with OpenCL) at `DARKTABLE_REF`. It applies an exposure gain per tonal zone: nine bands from −8 EV to 0 EV, joined by a Gaussian radial-basis-function curve. The gain is driven by a smoothed luminance mask, built by a guided filter or EIGF from M209 - Edge-Aware Filter Library, so local contrast inside each zone is preserved. It works on unbounded linear data, before any view transform. darktable runs it ahead of its tone mapper, so it is scene-referred and in scope. For a compositor it is a relight or exposure-zone tool for plates.

The darkroom's hover-and-scroll zone editing becomes a viewer interaction here:
- Hovering shows the zone under the cursor and its gain.
- Scrolling adjusts that zone.

Every user-facing control and darktable default is ported (`port-means-every-control`).

Deps: M209 - Edge-Aware Filter Library. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`ToneEqualizer`** in `Engine/Nodes/Color/`, plain ID, major 1. Inputs: Source and an optional Mask (the host MaskMix).
- **Every control** of the newest params version:
  - the nine band gains (−8 … 0 EV)
  - `curveSmoothing`
  - the luminance estimator: RGB euclidean norm, mean, max, luminance Y, power norm, or geometric mean
  - `preserveDetails`: no, guided filter, averaged guided filter, EIGF, or averaged EIGF
  - `filterDiffusion` (iterations)
  - `smoothingDiameter`, `edgesRefinement` and `maskQuantization`
  - `maskExposureCompensation` and `maskContrastCompensation`, plus their auto-adjust buttons (darktable's magic wands)
  - darktable's presets, as Natron presets
- **`output`:** Result, Mask or Mask Exposure Histogram Zones. This replaces darktable's darkroom mask display.
- **The viewer interaction:** hover readout as overlay text of the zone and EV, and scroll to adjust that band. Shift-scroll gives fine steps.
- CPU and GPU paths, darktable reference parity, a params-to-knob audit, a doc, an icon and a cost seed.

**Out:**
- darktable's histogram graph widget and its advanced-tab curve graph: the graph only redraws the nine band gains, which stay plain sliders here. The Mask output and the hover readout replace the histogram.

**Edge cases:**
- **Zero or negative luminance:** floored as darktable floors it before log2. Negative channels pass through scaled by the gain.
- **Auto-adjust buttons:** they compute statistics from the current frame at the current render scale, set the knobs, and are undoable as one step.
- **Proxy scale:** the smoothing diameter is relative to the image size, as darktable's is, so proxies match.
- **Region-of-interest expansion:** by the guided-filter radius × `filterDiffusion`.

## Phase 212.1: Node

- [ ] M212.P1.T1 — Knobs, mask and gain (CPU)
  - files: new `Engine/Nodes/Color/ToneEqualizer.h`, `Engine/Nodes/Color/ToneEqualizer.cpp`, `Engine/Nodes/Color/ToneEqualizer.reg.inc`
  - approach:
    - Port `compute_luminance_mask`, the estimators, the guided-filter and EIGF variants (on M209's `GuidedFilter` and `Eigf`), `compute_correction` with the RBF band interpolation, and the mask compensation from `toneequal.c`.
    - Add every knob and preset, and the `output` modes.
  - verify: P1.T4's reference jobs pass.
  - size: L

- [ ] M212.P1.T2 — Auto-adjust buttons and viewer hover/scroll
  - files: `Engine/Nodes/Color/ToneEqualizer.cpp`, new `Engine/Nodes/Color/ToneEqualizerInteract.{h,cpp}`
  - approach:
    - The buttons port darktable's `auto_adjust_exposure_boost` and `auto_adjust_contrast_boost` on a frame render of the mask.
    - The interact samples the cached mask at the cursor (or renders a point), draws text through the host overlay, and maps a scroll to a band delta, darktable's step 0.1 EV and 0.02 EV fine. `Node::addPositionInteract` (`Engine/NodeOverlay.cpp:347`) and the RawTemperature spot picker (`RawTemperature.cpp:409`) show how a native node gets pointer events.
  - verify: P1.T5's GUI script passes.
  - size: L

- [ ] M212.P1.T3 — GPU path
  - files: new `Engine/Gpu/Kernels/toneequal.slang`, `toneequal.cmake`, `Engine/Nodes/Color/ToneEqualizer.cpp`, new `Tests/Native/NativeToneEqualizerGpu_Test.cpp`
  - approach: port from darktable's `data/kernels/toneequal.cl`, with the mask smoothing on M209's GPU guided filter and EIGF.
  - verify: `ctest -R NativeToneEqualizerGpu` passes on lavapipe within the doc's tolerance.
  - size: L

- [ ] M212.P1.T4 — Reference parity, knob audit and docs
  - files: `tools/raw/make-reference.py`, `Tests/fixtures/raw/reference/` (new), new `Tests/Native/NativeToneEqualizer_Test.cpp`, `Engine/Nodes/Color/docs/ToneEqualizer.md`, `Engine/Nodes/Color/ToneEqualizer.costseed.inc`
  - approach:
    - darktable-cli jobs on a developed fixture: each estimator, each detail mode, band edits, and the compensations.
    - The node is given the same pre-module image and working space, linear Rec.2020, converted through OCIO.
    - Audit `dt_iop_toneequalizer_params_t` against the knobs.
    - Write the doc with the tolerance, plus an icon (`Gui/Resources/Images/NativeNodes/`) and a cost seed.
  - verify: `ctest -R NativeToneEqualizer` passes within the doc's `Tolerance:`.
  - size: L

- [ ] M212.P1.T5 — Xvfb GUI script for the viewer interaction
  - files: new `Tests/gui/test_tone_equalizer.py`
  - approach: create ToneEqualizer on a fixture, hover over a known zone, check the overlay readout, scroll, and check that the matching band knob changed by one step. Take screenshots of the Result and Mask outputs.
  - verify: the script passes under Xvfb.
  - size: M

**Verification gate:**
- The full build and ctest pass, including both ToneEqualizer suites on lavapipe and RADV.
- The Xvfb script passes.
- Screenshots of a plate relit by zone, with its Mask output, are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.
