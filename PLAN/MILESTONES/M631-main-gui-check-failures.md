# M631 - Main GUI Check Failures

Several GUI check scripts fail on main itself. An A/B run on main's own workflow (main 83241b652, AppImage from run 45653134112) failed the same checks as M599 - Runner Efficiency's runs, so M599 is not the cause. M599's gate4 (on 60af66f86f) added two more. For each failure, reproduce it on main under Xvfb, decide flaky check or real regression, and fix whichever it is.

Failing checks (all in `Tests/gui/`):
- `m52_layer_stack.py`, 4 of 136: "selecting the right variant changes the viewer", "the cube is green ({'r': (21, 603.7)})", "the displayColor primvar changes the viewer", "the right variant shows the blue cube, not the red one". Suspects: F7 - Natron Utility Core's viewer/render changes, or when the viewer refreshes after a variant switch.
- `m600_nodes_selection.py` (~:215), 2 of 51: "[spreadsheet] Tab on the Geometry Spreadsheet opens the tab menu", "[spreadsheet] Tab and GeoTransform from the Geometry Spreadsheet create a GeoTransform". Suspect: focus or shortcut routing on the Geometry Spreadsheet panel.
- `m146_panel_viewer_slot.py` (:4-9), 4 checks: Roto/RotoPaint viewer interface and viewer-slot hand-off. Likely M511 - Viewer Toolbar Fits 1920x1080's combo eliding, unconfirmed.
- `m275_viewer_widget_editing.py` (~:349, ~:426): translate/scale snap back after release, a real value change, not timing.
- `m121_routing_mix.py` (~:290): `[adjustbbox]` Num Pixels 0 leaves the centre empty (`Engine/Nodes/Transform/AdjustBBox.cpp`, GPU kernel `Engine/Gpu/Kernels/adjustbbox.slang`).

## Scope

**In:**
- The five scripts' listed failures, with a product fix where the product is wrong and a script fix where only the check is.

**Out:**
- `m224_shape_drag.py`: its fix (e9347fd229) is on M299 - Roto Render Performance's branch and lands with it.
- Other failing checks found along the way: note them in the PR, file them, don't fix them here.

**Edge cases:**
- **Flaky only under load:** a check that passes 10 of 10 locally but fails remotely gets its wait made condition-based, never a longer sleep.

## Phase 631.1: Reproduce and triage

- [ ] M631.P1.T1 — Reproduce each failure on main under Xvfb and classify it
  - files: read-only: the five scripts above; notes to `build/m631/triage.md`
  - approach: build or download a main AppImage, run each script under Xvfb (recipe in build/deeprepro/run-gui.sh; pre-seed checkForUpdates=false) three times. Per failing check, record pass rate, a screenshot at the failure, and a verdict: product regression (with the suspect commit, bisecting if needed), script bug, or timing. Remote GUI checks (`build.sh --from-run <id> --gui "<scripts>"`) may stand in for the local run.
  - verify: triage.md has a verdict and evidence for every listed check
  - size: L

## Phase 631.2: Fix

- [ ] M631.P2.T1 — Fix m121's AdjustBBox Num Pixels 0 centre
  - files: `Engine/Nodes/Transform/AdjustBBox.cpp`, `Engine/Gpu/Kernels/adjustbbox.slang`, `Tests/` AdjustBBox test
  - approach: per T1's verdict. If the product is wrong, Num Pixels 0 must pass the input through unchanged on CPU and GPU. Add a unit test for 0 on both paths.
  - verify: the unit test passes on CPU and GPU paths; `m121_routing_mix.py` `[adjustbbox]` passes
  - size: M
- [ ] M631.P2.T2 — Fix m275's translate/scale snap-back after release
  - files: per T1 (viewer transform interact in `Gui/` or the Transform node's overlay); `Tests/gui/m275_viewer_widget_editing.py`
  - approach: per T1's verdict and suspect commit: the value set on release must persist.
  - verify: `m275_viewer_widget_editing.py` passes 3 of 3 under Xvfb
  - size: M
- [ ] M631.P2.T3 — Fix m146's Roto/RotoPaint viewer interface and slot hand-off
  - files: per T1 (`Gui/ViewerTab*.cpp`, the viewer toolbar from M511); `Tests/gui/m146_panel_viewer_slot.py`
  - approach: per T1's verdict. If M511's eliding hides or reorders the tool combo, fix the hand-off without undoing the 1920x1080 fit.
  - verify: `m146_panel_viewer_slot.py` passes 3 of 3; `m511`'s toolbar check still passes at 1920x1080
  - size: M
- [ ] M631.P2.T4 — Fix m600's Tab on the Geometry Spreadsheet
  - files: per T1 (Geometry Spreadsheet panel in `Gui/`, shortcut routing); `Tests/gui/m600_nodes_selection.py`
  - approach: per T1's verdict: Tab with focus on the spreadsheet opens the node tab menu, and a node picked there is created.
  - verify: `m600_nodes_selection.py` passes 3 of 3
  - size: M
- [ ] M631.P2.T5 — Fix m52's variant-switch viewer checks
  - files: per T1 (viewer refresh after a variant edit, or the script's wait); `Tests/gui/m52_layer_stack.py`
  - approach: per T1's verdict. If the viewer doesn't refresh after a variant switch, fix the invalidation. If only the check reads too early, wait on the viewer's render-done signal.
  - verify: `m52_layer_stack.py` passes 3 of 3
  - size: M

**Verification gate:** full build and ctest green; all five scripts pass on a remote GUI run of this branch.
