# M597 - Curve Editor Expanded Knob Rows

`Tests/gui/m69_cleanup.py`'s `[expanded-curve]` section fails on main. It was checked on a 2026-11-20 main AppImage and on M521. After expanding Blur's `size` and bringing the Curve Editor tab to the front, the size row is found with 8 component rows (expected 2) and the curve widget is not found visible. The script last passed at 3b71764db (2026-11-12). The suspects since then are 5031f097c (M421 - Stroke Keyframe Indicators: tree `blockSignals` around context removal in `Gui/CurveEditor.cpp`) and 125e085eb (M399 - Node Creation Keys And Speed: the `KnobGuiContainerHelper` rewrite). Find out whether the curve editor really duplicates component rows (a product bug) or the check reads the tree wrongly, fix whichever it is, and keep the script passing.

**Possibly already covered (PM, 2026-11-27, from F36 - UI Polish Batch II):** the `[expanded-curve]` failure looks like the script counting the curve editor's hidden loop-driver rows (`Gui/CurveEditor.cpp` adds 6 hidden Start/End rows under each int/double knob), not a tree bug. F36 changes the script to count only shown rows (`feature/f36-ui-polish-batch-ii`). Once F36 lands, re-run the check on main first, then cancel this milestone or narrow it to what still fails.

## Scope

**In:**
- The root cause and a fix: in `Gui/` if the product is wrong, in `m69_cleanup.py` if only the check is.

**Out:**
- Other failing `m69_cleanup.py` sections, if any. Note them in the PR; they are not fixed here.

**Edge cases:**
- **Both wrong:** if the tree really does duplicate rows and the check also misreads visibility, fix both, in separate tasks.

## Phase 597.1: Diagnose and fix

- [ ] M597.P1.T1 — Find the cause of the duplicated rows and the hidden curve widget
  - files: `Gui/CurveEditor.cpp`, `Gui/KnobGuiContainerHelper.cpp`, `Tests/gui/m69_cleanup.py` (`[expanded-curve]` ~:1927, `size_rows` ~:1967); read-only
  - approach: run `m69_cleanup.py` under Xvfb (recipe in build/deeprepro/run-gui.sh) on a main build, dump the curve-editor tree's items under the Blur node (labels, parents, hidden flags) at the failing point, and bisect between 3b71764db and main, starting with 5031f097c and 125e085eb. Report which commit introduced it, and whether the tree really holds 8 size rows or the script counts rows from several contexts or nodes.
  - verify: a written diagnosis naming the commit and the faulty code or check, with the tree dump as evidence.
  - size: M
- [ ] M597.P1.T2 — Fix the cause
  - files: per P1.T1: `Gui/CurveEditor.cpp` and/or `Gui/KnobGuiContainerHelper.cpp`, or `Tests/gui/m69_cleanup.py`
  - approach: a product bug gets fixed so each knob has one row per dimension per context after removal and re-add, and the curve widget shows when its tab is front; add a GUI assertion that would have caught it. A check bug gets the script reading only the expanded node's rows and the widget's real visibility. Keep M421's signal blocking, since it prevents a crash on context removal.
  - verify: `m69_cleanup.py` passes in full on the remote GUI runner (`build.sh --from-run <id> --gui "m69_cleanup.py"`); `m96_weighted_tangents.py`, which shares its helpers, still passes.
  - size: M

**Verification gate:** full build and ctest green; `m69_cleanup.py` and `m96_weighted_tangents.py` pass on the remote GUI runner.
