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

## Phase 211.7: UAT round 3 fixes (user, 2026-11-02)

- [x] M211.P7.T1 — Shift-drag sets brush size
  - files: Retouch viewer interact
  - approach: shift-drag in the viewer resizes the brush, as in RotoPaint.
  - verify: GUI check shift-drags and reads the brush size knob change.
  - size: M
- [x] M211.P7.T2 — Strokes stay editable after painting
  - files: Retouch stroke storage and interact
  - approach: strokes/shapes can be selected and edited (points, settings) after they are painted, like RotoPaint strokes.
  - verify: GUI check paints a stroke, selects it, changes its settings and sees the render change.
  - size: L
- [x] M211.P7.T3 — One set of shape controls: next shape, or the selected shape
  - files: Retouch knobs/panel
  - approach: collapse the "selected shape" and "future shape" controls into one set. With nothing selected they set what the next shape/stroke gets; with a shape selected they show and edit that shape's settings.
  - verify: GUI check: settings with no selection apply to a new stroke; selecting a stroke loads its settings and editing them changes only that stroke.
  - size: L
- [x] M211.P7.T4 — Remove or wire the General-tab Color control
  - files: Retouch knobs
  - approach: the "color" control on the main/General tab has no effect. Wire it if it was meant to do something (fill colour), otherwise remove it.
  - verify: either a test shows the control changes the output, or the knob is gone and the panel check passes.
  - size: M

## Phase 211.8: Interactive performance (user, 2026-11-03)

Analysis (consultant, 2026-11-03): ~95% of the cost is PoissonHeal's 2000 red-black SOR sweeps over every masked pixel, mostly on one thread (work split by 256 runs), rerun from scratch on every pen-motion and offset-drag event, over the full frame (no RoI, no tiles), with full-frame wavelet decomposition whenever Scales > 0. Target: < 100 ms per interactive update on a modest workstation.

- [x] M211.P8.T1 — Headless bench and render-count test
  - files: Tests/Native/NativeRetouchBench_Test.cpp (new), NativeRetouch_Test.cpp
  - approach: call RC::process directly on 2K/4K synthetic images: one 100 px × 800 px heal brush (horizontal and diagonal), render scale 1 and 0.5, Scales 0 and 5, clone + heal; report ms per render. Node-level test counting renders per pen-motion and per offset-drag event.
  - verify: the bench prints per-case timings; the render-count test fails on today's code (several renders per event).
  - size: M
- [x] M211.P8.T2 — Multigrid heal solver (CPU)
  - files: Engine/Nodes/Filter/EdgeAware/PoissonHeal.{h,cpp}
  - approach: masked multigrid V-cycles (restriction/prolongation that respect the mask, red-black smoother) solved to convergence; work split by pixel count, not run count. Departs from darktable's unconverged 2000-iteration SOR on large masks (user-approved).
  - verify: NativeRetouch parity references hold within tolerance on small masks; a new test shows the converged residual ≤ SOR's; bench heal ≥ 20× faster.
  - size: L
- [x] M211.P8.T3 — Draft heal while dragging, full heal on release
  - files: Retouch.cpp (setShapeSourceOffset, renderOp), RetouchInteract.cpp
  - approach: one evaluation per gesture tick (group the offset setValues inside begin/endChanges); while a stroke is painted or an offset dragged, render a draft heal (coarse multigrid level / capped cycles, warm-started from the cached previous solution); run the full solve on pen-up. (user, 2026-11-03)
  - verify: render-count test ≤ 1 render per motion event; GUI script passes; the released result equals a fresh full render.
  - size: M
- [x] M211.P8.T4 — Region of interest and tiles
  - files: Retouch.{h,cpp} (getRegionsOfInterest, supportsTiles, renderOp)
  - approach: RoI = render window plus each intersecting shape's source bbox, padded by the wavelet support when Scales > 0; process that crop with mirror indices against the full RoD; supportsTiles true.
  - verify: a cropped render equals the full render within 1e-6.
  - size: M
- [x] M211.P8.T5 — Wavelet decomposition only where needed
  - files: RetouchRender.cpp, EdgeAware/Wavelets.cpp
  - approach: skip decomposition when no shape is on scale ≥ 1 and Output is Result; otherwise compute only the affected detail layers over shape + source bbox plus padding (output = input + Σ(detail′ − detail)); scale-0/residual/merge-from shapes keep the full path.
  - verify: existing scale and residual references hold within fp tolerance; bench Scales 5 case faster in proportion to area.
  - size: M
- [x] M211.P8.T6 — Per-shape result cache and faster mask raster
  - files: RetouchRender.cpp, Retouch.cpp
  - approach: cache each shape's patch keyed by input hash, geometry, settings, offset and the keys of earlier overlapping shapes, so editing one shape recomputes only it and later overlapping shapes; parallelise rasterizeBrush by rows, merge sub-strokes once, cache masks by geometry hash.
  - verify: editing shape A doesn't rerun shape B's solver (test); brush tests unchanged.
  - size: M
- [x] M211.P8.T7 — GPU multigrid heal
  - files: EdgeAware/EdgeAwareGpu.cpp, the heal Slang kernels
  - approach: port T2's multigrid to Slang (tens of dispatches instead of 6000), sweep masked pixels only, stop at convergence; removes the 500-iteration push-descriptor CPU fallback.
  - verify: NativeRetouchGpu within tolerance; dispatch count per heal recorded.
  - size: L

**Phase gate:** bench at 2K, render scale 0.5, < 100 ms per interactive update on CPU for the 100 × 800 heal stroke; new AppImage UAT on the user's workstation.


## Decisions

- 2026-11-02 — Audit: P6.T1–T3 committed and pushed (1c7e18874, 0d1bd238e, GUI check f4e856239). The round-3 UAT AppImage (03:30) was built after the branch head.
- 2026-10-27 — Parity also leaves out every shape's feather band (outline to feather distance): fill-color-ellipse's 21% pixel sits 0.15 px outside the outline, interior mask matches opacity exactly. Open: Natron's Roto feather reads ~0.34 at 0.15 px outside where a linear 2 px falloff gives ~0.54 — possibly a coarse feather mesh; check at UAT/visual review before calling it a Roto bug.
- 2026-10-27 — PM: darktable rasterises path feathers with integer-snapped lines (path.c _path_falloff_roi: len+1, neighbour writes, int truncation), giving a ~1 px full-strength band biased up/left; geometry otherwise matches exactly. Parity for Bezier-path jobs excludes a band from 2 px inside to feather + 1 px outside the path outline (darktable feather lines are int(len)+1 long with neighbour writes); tolerances unchanged; documented beside the feather-falloff deviation.
- 2026-10-27 — PM: kept darktable's behaviour for shapes above Scales+1 (no effect), cropped (not mirrored) out-of-image sources, Roto's feather falloff (differs from darktable's quadratic — documented), Max Heal Iterations visible, a Residual output; darktable's unused 'none' algorithm dropped. Source drag made undoable (one step per drag). References: 16 whole 64x48 synthetic half-float images, ~0.3 MB plain git.

## Phase 211.5: UAT round 1 fixes

- [x] M211.P5.T1 — Make Z smoothing produce expected Roto tangents
  - files: Roto shortcut dispatch, Bezier smoothing code, Wavelet Retouch GUI test
  - approach: Route Z through the same smooth-point operation used by the Roto editor and cover cusp, corner and multi-point selections without changing unrelated shortcuts.
  - verify: Selected retouch-path points become visibly smooth with symmetric tangent handles and undo in one step.
  - size: M

- [x] M211.P5.T2 — Expose paintbrush tools alongside Roto tools
  - files: Wavelet Retouch node registration and viewer toolbar integration, GUI test
  - approach: Advertise the retouch node as supporting the relevant RotoPaint brush tools as well as shape tools, with tool changes targeting its hosted context.
  - verify: The node panel and viewer can create and edit brush strokes and Roto shapes on the same node.
  - size: M

- [x] M211.P5.T3 — Match darktable's default source selection
  - files: `Engine/Nodes/Color/WaveletRetouch*`, darktable parity notes, native tests
  - approach: Trace darktable's source-placement logic and port it when deterministic; if darktable uses an arbitrary offset, document and test Natron's chosen equivalent instead of guessing.
  - verify: Reference cases demonstrate the same initial source placement as darktable or record the justified deterministic Natron policy.
  - size: M

**Verification gate:** original parity and GUI checks pass; Z smoothing, brush tools and default source behaviour are accepted in a replacement AppImage.
- 2026-11-01 — P5.T1/T3 at 513102043: Z smoothing fixed in the shared BezierCP smoothing (aligned, equal handles tracing a circle; also affects Roto/RotoPaint). darktable has no source search: the default offset is a fixed fraction of the image per shape kind (circle/ellipse 2% right and up, path 2% right and down, brush 1%), reused for later shapes; ported. P5.T2: the user's "needs paintbrush" overrides the Scope's "curves only" — porting darktable's brush shape plus Shift/Ctrl+Shift-click source placement (PM).
- 2026-11-01 — P5.T2 at 173c44d05: brush shape ported (tool N, pressure per point with darktable's five modes, per-shape size/hardness/density, GPU render, save/load). Open for UAT: Brush Size is a canonical-pixel diameter (darktable: image-relative); darktable's brush-smoothing preference not ported (implementer).

## Phase 211.6: UAT round 2 fixes

- [x] M211.P6.T1 — The brush overlay stays while drawing
  - files: `Gui/RetouchInteract.*` / the retouch brush overlay drawing, `Tests/gui/m211_retouch.py`
  - approach: User, 2026-11-02: "the brush overlay disappears while drawing". Keep the brush footprint (and the in-progress stroke's outline) drawn throughout the stroke, as RotoPaint's brush cursor stays.
  - verify: The GUI check grabs the viewer mid-stroke and finds the brush outline.
  - size: M

- [x] M211.P6.T2 — Heal shapes set their source like paint strokes
  - files: the RotoPaint clone/reveal source-offset logic (grep the Shift/Ctrl+click source placement and the clone offset knobs in `Engine/RotoPaint*`/`Gui/RotoGui*`), Retouch's heal/clone shapes, tests
  - approach: User, 2026-11-02: "the roto heal should have the same click-to-set-offset and other offset logic as the paint strokes". Retouch's heal (and clone) shapes reuse RotoPaint's source-offset behaviour exactly: the same click to set the offset, the same relative offset carried to the next shape, the same knobs and defaults. Share the code rather than duplicating it; keep darktable's default offset only where RotoPaint has no equivalent.
  - verify: The GUI check sets an offset by the same gesture as on a RotoPaint clone stroke and checks the heal shape's source lands at the clicked point, and the next shape keeps the offset.
  - size: M

- [x] M211.P6.T3 — Brushes use paint-stroke lifetime
  - files: Retouch brush items, the RotoPaint stroke lifetime knobs and behaviour (`Engine/RotoPaint*`, `Engine/RotoStrokeItem*`, lifetime knobs), tests
  - approach: User, 2026-11-02: "brushes should have the same lifetime controls and behaviour as normal paint strokes". Retouch brushes get RotoPaint's stroke lifetime knobs, defaults and behaviour (the same default lifetime as a new RotoPaint stroke, not P5's forced All), through the shared code.
  - verify: A test compares a new Retouch brush's lifetime knobs and defaults with a new RotoPaint stroke's and checks visibility across frames for each lifetime mode.
  - size: M

**Verification gate:** Retouch parity and GUI checks pass; a new AppImage is accepted by the user.
- 2026-11-02 — P6 at 1c7e18874. Decisions (user): the carried source offset starts at 0 like RotoPaint; Ctrl at pen down drags the offset as in RotoPaint; RotoPaint's Reset Transform button is relabelled Reset Clone Offset to match its script name.
- 2026-11-02 — P7 at 869042367: Shift-drag brush size via the shared roto interact; click inside a brush stroke selects it, its settings then edit it (stroke points aren't editable, as in RotoPaint; no Transform tab); one Shape Settings tab (old next-shape knobs kept hidden as storage); General Color hidden — a retouch mask uses coverage only (implementer).
- 2026-11-03 — UAT round 4: UI approved, but performance blocks shipping: a single heal stroke takes seconds to draw on the user's modest workstation (target < 100 ms), and adjusting a clone offset interactively isn't smooth even with the preview downscale. Profile and speed up before merge (user).
- 2026-11-03 — Performance decisions (user): multigrid heal solved to convergence (departs from darktable's unconverged output on large strokes); draft heal while painting/dragging, full heal on release.
- 2026-11-03 — P8.T1–T4 committed 9df7cf93f: multigrid-preconditioned CG heal (29.9x vs SOR on a 1000x300 stroke), draft heal while dragging, one render per motion, cropped processing with tiles, bench test. Open: the draft flag is read at render time rather than captured in the hash; the GPU still runs SOR on the whole image; Scales 5 decomposition now dominates (T5).
- 2026-11-03 — P8.T5–T7 committed d67843119. 2K, 5 scales: clone 147→7.6 ms, heal 203→59 ms (shape on scale 0); half-res heal 87→19 ms; brush raster 18.9→7.2 ms; GPU multigrid matches CPU within 1e-5 in emulation (unrun on a device). Open: descriptor-pool size without push descriptors on long heals; GPU input RoI still the full input.
- 2026-11-03 — Gate green (53e7ae1e6 + GUI script fixes through e0a61c050; GUI 32/32 heal-drag). Bench (CI tree, 3 runs, idle host): 2K scale 0.5 all cases < 100 ms (worst median 71 ms, shape on residual); 2K scale 1 heal up to 243 ms, residual 186–281 ms; 4K residual 750–940 ms. Round 5 UAT AppImage + steps built. Also fixed in passing: makeEllipse read back uncommitted points during a render (V-shaped ellipses from Python).
- 2026-11-03 — UAT round 5 approved (user). Main merge in progress, then gate, PR and review. User asked whether the speedup could go upstream: the multigrid-preconditioned CG heal is a drop-in for darktable src/common/heal.c (SOR); the band/area-limited decomposition is portable in concept to retouch.c; the interaction/caching/GPU parts are Natron-specific. Offered as a future stub (not filed; nothing new starts).
- 2026-11-03 — PR #179 conflicts with main after M205 merged; main merge needed before it can land.
