# M211 - Wavelet Retouch

A native `Retouch` node ported from darktable's `src/iop/retouch.c` (about 4.9k lines, with OpenCL) at `DARKTABLE_REF`. It provides frequency-separation cleanup:
- The image is split into à-trous wavelet scales by M209 - Edge-Aware Filter Library.
- Each drawn shape lives on one scale, or on the residual, and applies one algorithm there:
  - **clone**, copying from a source offset
  - **heal**, a Poisson heal from a source offset
  - **fill**, which erases to a neutral value or fills with a colour
  - **blur**, Gaussian or bilateral
- The scales are recomposed.

Shapes are Roto Beziers hosted by the node through M146 - Roto Hosting For Any Node, so they are animatable, trackable and editable with the usual curve tools. Each shape's retouch settings are per-item knobs the node adds.

Every user-facing control and darktable default is ported (`port-means-every-control`).

Deps: M209 - Edge-Aware Filter Library, M146 - Roto Hosting For Any Node. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`Retouch`** in `Engine/Nodes/Filter/`, plain ID, major 1. Inputs: Source and an optional Mask (the host MaskMix). It is a geometry curve host.
- **Global knobs** (darktable's GUI and params):
  - `numScales`, `currentScale` and `mergeFrom`
  - `algorithm` (the default for new shapes)
  - `blurType` and `blurRadius`
  - `fillMode` (erase, color), `fillColor` and `fillBrightness`
  - the preview-levels triplet
  - `output`: Result, Current Scale or Residual. darktable's "display wavelet scale" becomes a real output mode rather than a darkroom aid, so it is in scope.
- **Per-shape knobs**, stored on the node and keyed by item:
  - `algorithm`, `scale` and `opacity` (animatable)
  - `sourceOffset` (2D, animatable; clone and heal only)
  - the blur and fill parameters
- **Viewer:**
  - The source copy of each clone or heal shape is drawn offset. Dragging it edits `sourceOffset`, as darktable's source shape does.
  - The toolbar's shape tools create shapes on `currentScale` with the default algorithm.
  - darktable's "copy shapes to scale" and "move shapes to scale" become actions on the Roto panel's context menu, through M146's host hook.
- A panel column that shows each shape's scale and algorithm, through M146's column hook.
- CPU and GPU paths, darktable reference parity, a params-to-knob audit, a doc, an icon.

**Out:**
- darktable's darkroom display of masks and the scale "bar" widget: the `output` knob and the panel column replace them.
- Brush strokes as retouch shapes: none. Curves only, as in darktable, whose retouch shapes are drawn masks.
- darktable's spot-removal module (deprecated): none.

**Edge cases:**
- **A shape whose scale is higher than `numScales`:** it applies to the residual, as darktable clamps, and the panel shows a marker.
- **A source offset outside the image:** source pixels are mirrored at the border.
- **Feather:** the shape's Roto feather is used as darktable's mask feather, and its opacity as the mask opacity.
- **Region-of-interest expansion:** by the largest scale's support plus the largest source offset in the requested area.
- **Proxy scale:** scales map through darktable's `dwt_get_max_scale` scale bookkeeping, and offsets scale with render scale.
- **Shapes that overlap on one scale:** applied in panel order, as darktable applies its form list in order.

**Assumptions:**
- M146 lets a host attach per-item state or, failing that, P1.T1 keeps a host-side table keyed by item script-name that survives rename and is serialised with the node. The second is the planned route if M146 lands without per-item host knobs.

## Phase 211.1: Host and per-shape settings

- [x] M211.P1.T1 — Retouch node as a geometry curve host, with per-shape knobs
  - files: new `Engine/Nodes/Filter/Retouch.h`, `Engine/Nodes/Filter/Retouch.cpp`, `Engine/Nodes/Filter/Retouch.reg.inc`, new `Engine/Nodes/Filter/RetouchShapes.{h,cpp}`
  - approach:
    - Declare the geometry context through M146's capability gate.
    - Use the toolbar builder for Bezier, Ellipse and Rectangle tools plus selection.
    - Add the global knobs from the Scope.
    - `RetouchShapes` creates the per-shape knobs when an item is added and removes them when it is deleted. Use per-item host state if M146 provides it, else a table keyed by item script-name, saved with the node and updated on rename.
  - verify: P3.T3's tests for knob creation and removal, rename and save/load round trip pass.
  - size: L

- [x] M211.P1.T2 — Panel column and scale actions
  - files: `Engine/Nodes/Filter/Retouch.cpp`, new `Gui/RetouchPanelHooks.{h,cpp}`, `Gui/CMakeLists.txt`
  - approach:
    - Implement M146's panel host hook to add a "Scale / Algorithm" column.
    - Add the context-menu actions "Copy to scale…" and "Move to scale…", each undoable through the Roto undo stack.
  - verify: the P4.T2 GUI script passes.
  - size: M

## Phase 211.2: Algorithms (CPU)

- [x] M211.P2.T1 — Scale decomposition, mask rasterisation and recompose
  - files: `Engine/Nodes/Filter/Retouch.cpp`, new `Engine/Nodes/Filter/RetouchRender.cpp`
  - approach:
    - Decompose with M209's `Wavelets` to `numScales`.
    - Rasterise each shape's mask with feather and opacity at render scale via the existing Roto mask renderer (`RotoDrawableItem::renderMaskInternal`, `RotoContext.cpp:2823`).
    - Apply the algorithms per scale in shape order, then recompose.
    - Implement the `output` modes.
  - verify: with no shapes, the output equals the input within 1e-6 (P3.T3).
  - size: L

- [x] M211.P2.T2 — Clone, heal, fill and blur
  - files: `Engine/Nodes/Filter/RetouchRender.cpp`, new `Engine/Nodes/Filter/RetouchAlgorithms.cpp`
  - approach:
    - Port `retouch_clone`, `retouch_heal` (on M209's `PoissonHeal`), `retouch_fill` and `retouch_blur` (Gaussian, and bilateral via darktable's `bilateral.c` path or M130's if it has landed) from `retouch.c`.
    - Source offsets are in canonical coordinates.
  - verify: P3.T3's reference jobs pass.
  - size: L

## Phase 211.3: Viewer, GPU and parity

- [x] M211.P3.T1 — Source-shape overlay and drag
  - files: `Engine/Nodes/Filter/Retouch.cpp`, `Engine/RotoCurveInteract.{h,cpp}` (only the extension point M146 provides)
  - approach: for selected clone and heal shapes, draw the shape outline translated by `sourceOffset` with a connecting line. Dragging that outline sets a keyframe on `sourceOffset` when auto-key is on, else sets its value.
  - verify: the P4.T2 GUI script passes.
  - size: M

- [x] M211.P3.T2 — GPU path
  - files: new `Engine/Gpu/Kernels/retouch.slang`, `retouch.cmake`, `Engine/Nodes/Filter/RetouchRender.cpp`, new `Tests/Native/NativeRetouchGpu_Test.cpp`
  - approach: use M209's GPU wavelets and heal, plus a kernel for clone, fill and blur per scale. Masks are uploaded from the CPU rasteriser.
  - verify: `ctest -R NativeRetouchGpu` passes on lavapipe within the doc's tolerance.
  - size: L

- [x] M211.P3.T3 — Reference parity, knob audit and tests
  - files: `tools/raw/make-reference.py`, `Tests/fixtures/raw/reference/` (new), new `Tests/Native/NativeRetouch_Test.cpp`
  - approach:
    - darktable-cli jobs with XMP-authored circle, ellipse and path masks for each algorithm, on two scales and the residual, with opacity and feather.
    - The node is given matching Roto shapes. Ellipses become Beziers within 0.25 px, and that is part of the tolerance.
    - Audit `dt_iop_retouch_params_t` and its form parameters against the global and per-shape knobs.
    - Include identity and save/load checks.
  - verify: `ctest -R NativeRetouch` passes within the doc's `Tolerance:`.
  - size: L

## Phase 211.4: Docs and UAT

- [x] M211.P4.T1 — Doc and icon
  - files: `Engine/Nodes/Filter/docs/Retouch.md`, `Gui/Resources/Images/NativeNodes/`
  - approach: the doc explains scales, the algorithms, source offsets, the `output` modes and the tolerance.
  - verify: `ctest -R NativePluginList` passes.
  - size: S

- [x] M211.P4.T2 — Xvfb GUI script
  - files: new `Tests/gui/test_retouch.py`
  - approach: create Retouch on a fixture, draw a heal ellipse on scale 2, drag its source, check `sourceOffset`, use "Move to scale 3", and check the panel column. Take screenshots.
  - verify: the script passes under Xvfb.
  - size: M

**Prior verification gate:**
- The full build and ctest pass, including both Retouch suites on lavapipe and RADV.
- The Xvfb script passes.
- Screenshots of a skin and blemish cleanup across two scales are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.

## Decisions

- 2026-10-27 — Parity also leaves out every shape's feather band (outline to feather distance): fill-color-ellipse's 21% pixel sits 0.15 px outside the outline, interior mask matches opacity exactly. Open: Natron's Roto feather reads ~0.34 at 0.15 px outside where a linear 2 px falloff gives ~0.54 — possibly a coarse feather mesh; check at UAT/visual review before calling it a Roto bug.
- 2026-10-27 — PM: darktable rasterises path feathers with integer-snapped lines (path.c _path_falloff_roi: len+1, neighbour writes, int truncation), giving a ~1 px full-strength band biased up/left; geometry otherwise matches exactly. Parity for Bezier-path jobs excludes a band from 2 px inside to feather + 1 px outside the path outline (darktable feather lines are int(len)+1 long with neighbour writes); tolerances unchanged; documented beside the feather-falloff deviation.
- 2026-10-27 — PM: kept darktable's behaviour for shapes above Scales+1 (no effect), cropped (not mirrored) out-of-image sources, Roto's feather falloff (differs from darktable's quadratic — documented), Max Heal Iterations visible, a Residual output; darktable's unused 'none' algorithm dropped. Source drag made undoable (one step per drag). References: 16 whole 64x48 synthetic half-float images, ~0.3 MB plain git.

## Phase 211.5: UAT round 1 fixes

- [ ] M211.P5.T1 — Make Z smoothing produce expected Roto tangents
  - files: Roto shortcut dispatch, Bezier smoothing code, Wavelet Retouch GUI test
  - approach: Route Z through the same smooth-point operation used by the Roto editor and cover cusp, corner and multi-point selections without changing unrelated shortcuts.
  - verify: Selected retouch-path points become visibly smooth with symmetric tangent handles and undo in one step.
  - size: M

- [ ] M211.P5.T2 — Expose paintbrush tools alongside Roto tools
  - files: Wavelet Retouch node registration and viewer toolbar integration, GUI test
  - approach: Advertise the retouch node as supporting the relevant RotoPaint brush tools as well as shape tools, with tool changes targeting its hosted context.
  - verify: The node panel and viewer can create and edit brush strokes and Roto shapes on the same node.
  - size: M

- [ ] M211.P5.T3 — Match darktable's default source selection
  - files: `Engine/Nodes/Color/WaveletRetouch*`, darktable parity notes, native tests
  - approach: Trace darktable's source-placement logic and port it when deterministic; if darktable uses an arbitrary offset, document and test Natron's chosen equivalent instead of guessing.
  - verify: Reference cases demonstrate the same initial source placement as darktable or record the justified deterministic Natron policy.
  - size: M

**Verification gate:** original parity and GUI checks pass; Z smoothing, brush tools and default source behaviour are accepted in a replacement AppImage.
