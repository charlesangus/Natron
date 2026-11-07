# M306 - Knob Row Layout

Follow-up from the user's 2026-11-02 UAT of M229 - 3D Viewer Guides And Display: in properties panels, single-value number knobs (USDLight's Intensity, Exposure, Width, Height, Uniform Scale) put their field far to the right, lined up with the second column of the three-value knobs, instead of next to their label like Prim Path, Type and Color. It "is happening everywhere across milestone files, maybe it was always like that, but it needs to be fixed" (user). Screenshot: `.local/knob_layout.png`.

M229 (7e55a1ca1) packed number knobs without a display range to the left; knobs with a display range (and so a slider) still spread out. Find the layout rule in `Gui/KnobGuiValue*`/`Gui/KnobGuiContainerHelper*` and make every single-value number knob sit straight after its label, in every panel, at 1920x1080.

Deps: M229.

## Scope

**In:** single- and multi-value number knobs' row layout in all properties panels; a GUI check that measures the field's x position against the label for a sample of nodes.

**Out:** slider redesign.

Stub: to be broken down into tasks when claimed.

Likely no longer reproduces: the screenshot `.local/knob_layout.png` (2026-11-02 00:35) predates PR #163, and reading `Gui/KnobGuiValue.cpp:630,671,723-728`, every single-value case is packed left. So this milestone measures first, and fixes only what the sweep finds.

## Scope

**In:** a GUI sweep measuring every single- and multi-value number knob against the field column, on all panels at 1920x1080. A fix in the layout rule only if the sweep finds a violation.

**Out:** slider redesign; matrix knobs, which use their own grid.

**Edge cases:** knobs sharing a line follow the previous field. A knob with a visible slider fills the row by design, so the check measures the spinbox's left edge.

## Phase 306.1: Measure then fix

- [ ] M306.P1.T1 — Sweep every node's knob rows and report violations
  - files: `Tests/gui/knob_row_layout_sweep.py` (new; reuse the helpers of `Tests/gui/scene_node_knob_layout.py`)
  - approach: create every plugin node, open its panel at 1920x1080, scale 1, and check each main-page row's first spinbox is within 30 px of the field column. List violations without failing the first run. Include a 1-D knob with a display range, and Intensity, Exposure, Width, Height and Uniform Scale on USDLight.
  - verify: the script runs remotely and prints the table.
  - size: M

- [ ] M306.P1.T2 — Fix the rule for each violation the sweep finds
  - files: `Gui/KnobGuiValue.cpp`, `Gui/KnobGuiContainerHelper.cpp` if the grid is the cause
  - approach: fix in the layout rule, never per node. If T1 finds nothing, close this task and record "not reproducible" under Decisions.
  - verify: the sweep reports zero violations; USDLight and one image node shots go to the user.
  - size: M

- [ ] M306.P1.T3 — Make the sweep a hard check
  - files: `Tests/gui/knob_row_layout_sweep.py`
  - approach: a `check()` per row.
  - verify: it passes remotely and fails if the `addStretch(0)` at `Gui/KnobGuiValue.cpp` ~:727 is removed.
  - size: M

**Verification gate:** the remote sweep passes with zero violations; the user has seen the USDLight shot.

## Decisions

- 2026-11-07 — **Cancelled (user):** the user confirmed newer builds no longer show this; 7e55a1ca1 fixed it. The sweep isn't being built.
