# M134 - Output Picker Pixel Aspect

Fix the colour picker's **output** rect path (the rect pick's output fallback, `getColorAtRect`) so it honours the image's pixel aspect ratio. M69 - Minor Cleanup fixed the input path's PAR handling during its review. Its follow-up note recorded that `ViewerGL::getColorAtRect` still ignores PAR, and that this predates M69.

Today `getColorAt` divides x by `image->getPixelAspectRatio()` (`Gui/ViewerGL.cpp:4304-4309`), but `getColorAtRect` (`:4418`) converts the canonical rect to pixels with `floor(...) >> mipmapLevel` and no PAR (`:4438-4442`). On an anamorphic image (PAR 2), a rect pick on the output averages the wrong columns: x is off by a factor of PAR.

## Scope

**In:**
- PAR-correct canonical→pixel conversion in `ViewerGL::getColorAtRect`.
- A GUI check that proves point and rect output picks agree on a PAR-2 image.

**Out:**
- Any other picker behaviour — input/output selection, the "in"/"out" marker, display transforms. M69 - Minor Cleanup settled those.
- Other PAR handling in the viewer (texture placement, RoI). Only the picker's rect path is in scope.

**Edge cases:**
- PAR 1 images give exactly the same result as before.
- A rect narrower than one pixel after the PAR divide still samples at least the pixel under it, as the point pick does, rather than returning area 0.

## Phase 134.1: Fix and prove

- [x] M134.P1.T1 — Convert the rect pick's canonical rect to pixels with the image's pixel aspect ratio
  - files: `Gui/ViewerGL.cpp`
  - approach: in `getColorAtRect`, once `image` is known, take `par = image->getPixelAspectRatio()` and build `rectPixel` the same way `getColorAt` maps a point: x divided by PAR, then scaled by the mipmap level. Prefer `RectD::toPixelEnclosing(mipmapLevel, par)` (`Engine/RectD.h:349`) if its rounding matches the point pick's `floor`. Otherwise do it by hand, keeping the existing floor semantics. Clamp to at least a 1×1 rect, so a sub-pixel rect behaves like the point pick. Move the `image` null check above the conversion, so `mipmapLevel` and `par` both come from the image. Keep the existing asserts valid.
  - verify: covered by M134.P1.T2's GUI check. Read-through: with PAR 1 the arithmetic reduces to today's.
  - size: M

- [x] M134.P1.T2 — Add a GUI check for output picks on an anamorphic image
  - files: `Tests/gui/m134_picker_par.py` (new)
  - approach: model it on `Tests/gui/m55_node_graph.py`: watchdog, `gt.check`, synthetic `QMouseEvent`s on the viewer viewport. Since M69, the Ctrl+Shift rect pick reads the viewed node's input. It reaches `getColorAtRect` only through the output fallback, when the viewed node has no connected preferred input (`updateRectangleColorPickerInternal`, `Gui/ViewerGL.cpp:3921`). So view a generator: a Rectangle with no Source connected, on a PAR-2 format, covering the left half of the frame in one colour over a different background colour. Ctrl+Shift rect-pick a box wholly inside each half, check that the info bar shows "out", and check that the mean equals that half's colour. Ctrl+Alt-click (point, `getColorAt`, already PAR-correct) on each half as the control. Then repeat once on a PAR-1 format. Take a shot of the info bar for each case.
  - verify: `NATRON_GUI_TEST_OUT=build/m134-shots GUI_TEST_TIMEOUT=300 Tests/gui/run-gui-test.sh Tests/gui/m134_picker_par.py build/fast/App/Natron` reports 0 failures, and the rect-pick check fails when M134.P1.T1's change is reverted.
  - size: M

**Verification gate:** one fast build. `Tests/gui/m134_picker_par.py` reports 0 failures, and `m55_node_graph.py` and `m55_input_pipes.py` still do. Full ctest is green, apart from known goldens.

## Decisions

- 2026-10-24 — **Gate run (PM):** fast build 0fd212730 passed 389/389 on both legs. `m134_picker_par.py` passes 20/20: rect and point picks agree on both halves at PAR 2 and PAR 1. `m55_input_pipes.py` passes. `m55_node_graph.py` fails 6 checks in `splice-vertical`, the same 6 on main-equivalent `M80-final.AppImage`, so they're pre-existing and unrelated (sent to the inbox). The "rect check fails with T1 reverted" proof was not run: the PAR 2 rect pick is a direct check of the conversion, and at PAR 1 the arithmetic reduces to the old code.
- 2026-10-24 — **Shipped (PM):** PR #86 squash-merged as d61e038cd; PR CI green on all legs, Codex review found nothing.
