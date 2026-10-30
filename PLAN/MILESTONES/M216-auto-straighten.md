# M216 - Auto-Straighten

A native `AutoStraighten` node ported from darktable's rotate-and-perspective module, `src/iop/ashift.c` (about 9k lines with `ashift_lsd.c` and `ashift_nmsimplex.c`, with OpenCL), at `DARKTABLE_REF`. How it works:
- LSD line detection finds the structure in the plate.
- Lines are classified as vertical or horizontal.
- A Nelder–Mead fit solves rotation, vertical and horizontal lens shift, and shear, so the verticals and horizontals straighten.
- The result is applied as a homography through the native resampler.
- An automatic crop removes the empty borders.

The darkroom's rectangle guide mode, a user-placed quad, is a four-point corner interact here. The display and toggling of individual detected lines needs per-segment viewer drawing, which belongs to M165 - Rich Viewer Overlays.

Every user-facing control and darktable default is ported (`port-means-every-control`), except for the line-selection UI that this milestone leaves out.

Deps: none. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`AutoStraighten`** in `Engine/Nodes/Transform/`, plain ID, major 1. Input: Source.
- **Every control** of the module's newest params version:
  - `rotation`, `lensShiftV`, `lensShiftH` and `shear`
  - `autoCrop`: off, largest area, or original format
  - lens model: generic, or specific with `focalLength`, `cropFactor`, `orientation` and `aspectAdjust`
  - the fit buttons: rotation, vertical, horizontal, both, and the combinations darktable offers
  - `structure` detection, with enhance modes none, contrast, edge and detail
  - the rectangle guide mode
  - darktable's own `ashift` presets
- **Fit actions:** they run line detection on the current frame at full resolution and set the knobs. They are undoable as one step and keyed when auto-key is on.
- **Rectangle mode:** four corner position interacts (`Node::addPositionInteract`, `Engine/NodeOverlay.cpp:347`) define the quad that darktable's rectangle mode fits.
- **Homography warp:** through `Resampler` (`Engine/Nodes/Image/Resampler.h`), with the native filter choice, and the region of definition and format from `autoCrop`.
- CPU and GPU paths, darktable reference parity, a params-to-knob audit, a doc, an icon.

**Out:**
- Drawing detected lines in the viewer, toggling them by click or drag, and darktable's drawn-lines guide mode: M165 - Rich Viewer Overlays. They are added to its consumers, and the node exposes the line list for it.
- Concatenation with Transform: none here. M153 - Distortion Concatenation may take it later.

**Edge cases:**
- **No structure found:** the fit leaves the knobs unchanged and shows a viewer message.
- **Fits per frame on a moving plate:** each fit keys its frame, and the doc says to fit on representative frames.
- **Proxy scale:** detection always runs at full resolution, and the parameters are resolution-independent.
- **Pixel aspect ratio ≠ 1:** detection and the homography work in square pixels and map back.

## Phase 216.1: Model and fit

- [x] M216.P1.T1 — Homography model and warp (CPU)
  - files: new `Engine/Nodes/Transform/AutoStraighten.h`, `Engine/Nodes/Transform/AutoStraighten.cpp`, `Engine/Nodes/Transform/AutoStraighten.reg.inc`
  - approach:
    - Port `homography()`, the lens-model and crop math (`do_crop`, `crop_adjust`) and every param knob from `ashift.c`.
    - Warp through `Resampler`, following Transform's native resample path (`Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Image/ResampleExecutor.*`).
    - The region of definition and format follow `autoCrop`.
  - verify: P2.T3's reference jobs for the manual parameters pass.
  - size: L

- [x] M216.P1.T2 — Line detection
  - files: new `Engine/Nodes/Transform/AshiftLsd.{h,cpp}`, new `Engine/Nodes/Transform/AshiftLines.{h,cpp}`
  - approach: port `ashift_lsd.c` (LSD; read its licence header and record it in `tools/license/components/README.md`, stopping to ask if it is not GPL-3-compatible) and `ashift.c`'s `get_structure`, enhance modes, line classification and weighting. Expose a `lines(time)` accessor for M165.
  - verify: `Tests/Native/AshiftLines_Test.cpp` (P2.T3) on a synthetic grid rotated by 3° finds its verticals and horizontals within 0.1°.
  - size: L

- [x] M216.P1.T3 — Nelder–Mead fit and fit actions
  - files: new `Engine/Nodes/Transform/AshiftFit.{h,cpp}`, `Engine/Nodes/Transform/AutoStraighten.cpp`
  - approach: port `ashift_nmsimplex.c` and `nmsfit`/`model_fitness`. Add the fit buttons, each running detection and fitting on the current frame and setting the knobs as one undo step.
  - verify: P2.T3's fit tests pass.
  - size: M

## Phase 216.2: Interaction, GPU and parity

- [x] M216.P2.T1 — Rectangle guide mode
  - files: `Engine/Nodes/Transform/AutoStraighten.cpp`
  - approach: add four animatable corner position knobs with `addPositionInteract`, shown only in rectangle mode, which feed darktable's quad fit.
  - verify: P2.T4's GUI script passes.
  - size: M

- [x] M216.P2.T2 — GPU warp
  - files: `Engine/Nodes/Transform/AutoStraighten.cpp`, new `Tests/Native/NativeAutoStraightenGpu_Test.cpp`
  - approach: reuse the native resampler's GPU path. Detection and fitting stay on the CPU, since they are actions, not renders.
  - verify: `ctest -R NativeAutoStraightenGpu` passes on lavapipe within 1e-4 relative.
  - size: M

- [x] M216.P2.T3 — Reference parity, fit tests and knob audit
  - files: `tools/raw/make-reference.py`, `Tests/fixtures/raw/reference/` (new), new `Tests/Native/NativeAutoStraighten_Test.cpp`, new `Tests/Native/AshiftLines_Test.cpp`
  - approach:
    - darktable-cli jobs with fixed rotation, lens shift, shear and lens model, crossed with each `autoCrop` mode, compared within the doc's tolerance.
    - Run the fit against darktable's fitted parameters for the same fixture. darktable's fit is interactive, so record its fitted values once with the harness's logging and compare the parameters within 0.05°.
    - Audit `dt_iop_ashift_params_t` against the knobs, listing the M165-owned GUI controls as known exclusions.
  - verify: `ctest -R "NativeAutoStraighten|AshiftLines"` passes.
  - size: L

- [x] M216.P2.T4 — GUI script, doc and icon
  - files: new `Tests/gui/test_auto_straighten.py`, `Engine/Nodes/Transform/docs/AutoStraighten.md`, `Gui/Resources/Images/NativeNodes/`
  - approach: under Xvfb, fit "both" on a tilted architecture fixture, check that the knobs moved, switch to rectangle mode, drag a corner, and take screenshots. The doc covers the fits, the lens model, crop and the M165 follow-up.
  - verify: the script passes under Xvfb, and `ctest -R NativePluginList` passes.
  - size: M

**Verification gate:**
- The full build and ctest pass, including both AutoStraighten suites.
- The Xvfb script passes.
- Before-and-after screenshots of an auto-straightened architecture plate are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.

## Decisions

- 2026-10-28 — Release gate green. Next: GUI checks/screenshots (container held for the M117/M180 benches), then AppImage + UAT.
- 2026-10-30 — **UAT passed (user).** Auto-Straighten is accepted for merge.
- 2026-10-27 — Drawn Lines implemented: a Draw Line tool (V) makes two-point open curves auto-tagged by direction; Line Orientation knob / right-click retag; fit needs ≥2 lines per relevant direction. Curve-hosting hunks copied from M211 (expect overlap when M211 merges).
- 2026-10-27 — **User-drawn lines replace line detection (user):** no LSD (AGPL-3) and no own detector; the user draws vertical/horizontal guide lines with the shared roto groundwork (M146 curve hosting) on the node, tagged as vertical or horizontal; the fit uses those lines (darktable's ashift fit on the user's segments). Rectangle mode stays.
