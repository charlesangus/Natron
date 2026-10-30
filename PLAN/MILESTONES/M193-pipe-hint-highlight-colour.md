# M193 - Pipe Hint Highlight Colour

The drop-on-pipe hint paints the target pipe and the two ghost pipes green: `Edge::paint` hardcodes `Qt::green` (`Gui/Edge.cpp:861-862`) and the ghost edges use `QColor(0,255,0,100)` (`Gui/NodeGraph.cpp:153-159`). They should use the app's highlight colour, the Preferences "Selection" colour (`Settings::getSelectionColor`, `Engine/Settings.cpp:723-726`), so the new kiss preview and the splice hint share one themeable colour.

## Scope

**In:**
- The highlighted pipe, the ghost input/output pipes and the arrowhead use the Selection colour, reading it live so a Preferences change applies.
- `Tests/gui/m55_node_graph.py`'s green detection (`is_green`, `count_green_in_scene_rect`, `check_green_present`) becomes a check against the configured Selection colour.

**Out:**
- Drop-on-pipe behaviour and geometry. (M176 - Splice-Vertical Regression)
- The kiss preview itself, which reuses this colour. (M194 - Kiss Connections)
- A separate "hint colour" preference. (none)

**Edge cases:**
- A Selection colour near the default pipe or white selected-pipe colour: the hint remains distinguishable by its thickness/alpha; no extra handling.
- The ghost edges keep their translucent alpha (100/255) applied to the Selection colour.

**Assumptions:**
- Unconfirmed: the Selection colour is acceptable on both dark and light pipe backgrounds without a contrast adjustment.

## Decisions

- 2026-10-28: GUI checks pass on Actions (M269 remote runs); AppImage and UAT doc in build/appimages/, contact sheet build/uat-shots/; awaiting user UAT.
- 2026-10-28 — Release gate green. Next: GUI checks/screenshots (container held for the M117/M180 benches), then AppImage + UAT.
- 2026-10-30 — **UAT passed (user).** The highlight-colour change is accepted for merge. Drag-insert behaviour for nodes with an existing input is follow-up work in M277 - Graph Creation And Insert.
- 2026-10-25 — highlight-colour-source: the Preferences Selection colour is the app's highlight colour; no new setting: it is already the shared highlight (ComboBox, checkbox, dope sheet) and `QPalette::Highlight` is only set locally.

## Phase 193.1: Recolour

- [ ] M193.P1.T1 — Recolour the highlighted pipe and ghost edges
  - files: `Gui/Edge.cpp`, `Gui/NodeGraph.cpp`, `Gui/NodeGraph20.cpp`
  - approach: In `Edge::paint` replace `Qt::green` in the `useHighlight` branch with the colour from `appPTR->getCurrentSettings()->getSelectionColor`. Replace the ghost edges' `setDefaultColor(QColor(0,255,0,100))` with the Selection colour at alpha 100, and refresh it where the splice hint is shown (`NodeGraph20.cpp:321-329`) so a Preferences change is picked up.
  - verify: build; under Xvfb the splice hint pixels match the Selection colour (T2).
  - size: M

- [ ] M193.P1.T2 — Update the GUI colour checks
  - files: `Tests/gui/m55_node_graph.py`
  - approach: Replace `is_green` and its callers with a colour-distance check against the Selection colour read from Preferences, and add a check with a non-default Selection colour.
  - verify: `m55_node_graph.py` passes under Xvfb, including `splice-vertical` as far as M176 - Splice-Vertical Regression allows (record any pre-existing failure).
  - size: M

**Verification gate:** the full build and `ctest` pass, and `Tests/gui/m55_node_graph.py` passes under Xvfb with the hint drawn in the Selection colour. A screenshot of the hint with the default and a changed Selection colour is shared with the user before sign-off. The GUI bench rule applies.
