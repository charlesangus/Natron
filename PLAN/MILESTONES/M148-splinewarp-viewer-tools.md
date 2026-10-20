# M148 - SplineWarp Viewer Tools

Make SplineWarp usable in the viewer, then make it visible in the menus. This milestone delivers:
- the curve tools from M146 - Roto Hosting For Any Node's `RotoCurveInteract`, plus a point tool for single-point curves;
- warp tools: duplicate and link, link two existing curves, unlink, swap, and toggle boundary;
- a correspondence tool;
- colour-coded source and destination overlays with link lines;
- Source/Destination/Both and warped/unwarped viewing toggles;
- live coarse-field feedback while dragging;
- panel columns for each curve's role and partner.

Deps: M147 - SplineWarp Node Core (the node, `SplineWarpContext`, auto-alignment, the field). M146 - Roto Hosting For Any Node supplies `RotoCurveInteract`, `createToolbarKnobs`, the panel column hook and the shared viewer slot.

Scouted facts (2026-10-20, `main` at `8fdb5741f`; re-check at promotion against what M146 - Roto Hosting For Any Node and M147 - SplineWarp Node Core actually built):
- Toolbar pages are generic: `Gui/NodeViewerContext.cpp:192-204` picks up any page with `getIsToolBar()`. Tool groups and buttons are `KnobGroup::setAsToolButton` and `KnobButton::setAsToolButtonAction` (`Engine/KnobTypes.h:453,958,1012`).
- The roto tool enum is `RotoToolEnum` (`Engine/RotoPaintInteract.h:467`). M146 - Roto Hosting For Any Node moves it into `RotoCurveInteract`.
- Picking a parameter on a curve uses `Bezier::isPointOnCurve(x,y,acceptance,&t,&feather)` (`Engine/Bezier.h:~164`).
- Draft render: native nodes receive a `draft` flag in their render arguments. Transform's `getTransform(time, scale, draft, ...)` (`Engine/EffectInstance.h:1412`) shows it is plumbed. Confirm how a native Custom node reads it at promotion.
- Auto-keying and ripple edit are on the context (`RotoContext::setAutoKeyingEnabled`, `RotoContext.h:124`), and move to `RotoCurveContext` in M146 - Roto Hosting For Any Node.

## Scope

**In:**
- **`SplineWarpInteract`** on `RotoCurveInteract`. Its toolbar holds:
  - the curve tool groups (select, points, add/remove, smooth/cusp, Bezier/BSpline/Ellipse/Rectangle/open Bezier);
  - a new **point** tool, where a click creates a single-CP open curve;
  - a **warp** group: Duplicate and Link, Link, Unlink, Swap Source/Destination, and Toggle Boundary;
  - the **correspondence** tool.
- **Duplicate and Link:** the copy lands exactly on the original, becomes the destination, and is selected, so the warp starts at zero.
- **Link:** the first selected curve becomes the source and the second the destination. M147 - SplineWarp Node Core's auto-alignment runs, and no correspondences are added.
- **Correspondence tool:**
  - Click a point on a linked curve. A correspondence is created at that parameter, with its partner at the matching parameter on the other curve under the current pairing.
  - Drag either end, which stays on its curve.
  - Delete removes it.
  - It respects auto-keying.
- **Display:**
  - Distinct overlay colours for source, destination, boundary and unlinked curves.
  - Lines joining the ends of each link, at the CPs and at each correspondence.
  - A toolbar **show** choice of Source / Destination / Both.
  - A toolbar **output** toggle of warped / unwarped. It is a non-persistent viewer knob; when unwarped, the node renders as identity.
- **Live feedback:** while a pen drag is active, renders use a coarser field (×4 step) and half the curve resolution. Full quality is restored on pen-up.
- **Panel:** Role and Linked-to columns, plus Link/Unlink/Boundary context actions, through M146 - Roto Hosting For Any Node's panel hook.
- Undo for every action.
- Making the node visible: non-internal registration, the Transform menu, an icon, and `docs/SplineWarp.md`.

**Out:**
- A field or mesh preview overlay → none (user, grill).
- Mesh solver UI beyond the solver knob → M149 - SplineWarp Mesh Solver.
- Track links and pasting curves from Roto → M152 - Curve Track Links And Roto Copy.

**Edge cases:**
- Duplicate and Link on a curve that is already linked or a boundary: refused, with a status message.
- Link with anything other than two unlinked, non-boundary curves selected: refused, with a status message.
- Toggle Boundary on an open curve or a linked curve: refused, and the button is disabled for that selection.
- Correspondence tool clicked on an unlinked curve: no-op.
- Dragging a correspondence end past another correspondence on the same curve: clamp it between its neighbours, so spans never invert.
- Show = Source while the output toggle is warped: allowed. You see the source curves over the warped image, which is how you check what moved.
- The output toggle is never saved, and resets to warped on project load.
- Pen-up after a drag with the render still running: the full-quality render supersedes the draft one.

**Assumptions:**
- The native Custom kind can read the render's draft flag. If it can't, P2.T2 plumbs it.
- A non-persistent toolbar knob can drive render output without dirtying the project. Check how RotoPaint's viewer-UI knobs avoid serialization.

## Phase 148.1: Interact and warp tools

- [ ] M148.P1.T1 — `SplineWarpInteract`, the toolbar and the point tool
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.{h,cpp}` (new), `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Engine/RotoCurveInteract.{h,cpp}`
  - approach:
    - Derive from `RotoCurveInteract`, and hook it up as the node's overlay through the base's delegation pattern.
    - Build the toolbar with `createToolbarKnobs` using the curve groups, then append the warp group and the correspondence tool.
    - Add the point tool to `RotoCurveInteract`'s tool set: a click creates a one-CP open Bezier and finishes it. Roto can enable it later.
    - Default to the select tool.
  - verify: `Tests/Native/SplineWarpTools_Test.cpp` (new) checks the toolbar page's children and that the point tool's action creates a one-CP open curve through the context. The Xvfb gate covers the interaction.
  - size: L

- [ ] M148.P1.T2 — Warp actions with undo
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpUndo.{h,cpp}` (new), `Tests/Native/SplineWarpTools_Test.cpp`
  - approach:
    - Duplicate and Link: clone the curve (the copy ctor), insert it above the original, link with `duplicate=true`, and select the copy.
    - Link: take the selection order, then auto-align.
    - Also add Unlink, Swap Source/Destination, and Toggle Boundary, with the refusals from `## Scope`.
    - Each action is one undo command over `SplineWarpContext`.
  - verify: the test covers Duplicate and Link rendering bit-exact identity before any edit, Link auto-aligning a re-started copy, Swap matching invert for a single link, each refusal, and undo/redo restoring the model and the content hash.
  - size: L

- [ ] M148.P1.T3 — Correspondence tool
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpUndo.{h,cpp}`, `Tests/Native/SplineWarpTools_Test.cpp`
  - approach:
    - On pen-down, find the nearest linked curve and parameter with `isPointOnCurve`. Create the correspondence, placing the partner parameter by the current span's arc-length pairing.
    - On drag, project the pen onto that end's curve and clamp between the neighbouring correspondences.
    - Delete or Backspace removes the selected correspondence. Keyframe according to auto-keying.
    - Each gesture is one undo command.
  - verify: the test drives the tool's action functions to create, move (with the clamp) and delete a correspondence, checks the keyframes under auto-key on and off, and checks undo.
  - size: L

## Phase 148.2: Display and feedback

- [ ] M148.P2.T1 — Role colours, link lines, and the show and output toggles
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`
  - approach:
    - Override the base's per-curve overlay colour hook by role.
    - Draw the link lines between paired CPs and correspondence ends at the current time.
    - Add the show choice and the non-persistent output toggle to the toolbar. When unwarped, `isIdentity` returns Source.
  - verify: an `isIdentity` test for the output toggle, plus a project save that leaves it out (`Tests/Native/SplineWarpTools_Test.cpp`). The look is checked in the gate screenshots.
  - size: M

- [ ] M148.P2.T2 — Live coarse-field feedback while dragging
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpField.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`
  - approach:
    - While a drag is active, render in draft. In draft, SplineWarp uses ×4 the field step and half the curve resolution.
    - On pen-up, trigger a full-quality render.
    - Plumb the draft flag into the Custom kind if it isn't there already.
  - verify: a test that a draft render uses the coarse field (by its cache key) and differs from full quality by less than a stated tolerance on a smooth fixture. The gate's Xvfb pass drags a point and captures one frame mid-drag.
  - size: M

## Phase 148.3: Panel, visibility and docs

- [ ] M148.P3.T1 — Panel Role and Linked-to columns and context actions
  - files: `Gui/SplineWarpPanelHook.{h,cpp}` (new), `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Gui/CMakeLists.txt`
  - approach: implement M146 - Roto Hosting For Any Node's panel hook. Role shows Source, Destination, Boundary or a blank; Linked-to shows the partner's label. The context-menu actions call P1.T2's actions.
  - verify: the build passes. The gate's Xvfb panel screenshot shows both columns for a linked pair and a boundary.
  - size: M

- [ ] M148.P3.T2 — Make SplineWarp visible, with its icon and docs
  - files: `Engine/Nodes/Transform/SplineWarp.reg.inc`, `docs/SplineWarp.md` (new), the icon under the native node icon directory used by the M120 - Native Node Port Kit fragments
  - approach: register it non-internal in the Transform group, add an icon, and write the user docs. They cover the curve roles, linking, correspondences, boundaries and feather, amounts, invert, the solver knobs, the viewing toggles, and that unlinked curves are ignored.
  - verify: a test that the plugin is listed and non-internal. The docs build passes if docs CI covers `docs/`.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including `SplineWarpTools_Test`.
- Xvfb GUI check on the `fast` AppImage, with screenshots shared before sign-off:
  - Draw a closed curve and use Duplicate and Link. Drag the destination and see the warp, with a mid-drag draft frame.
  - Draw an open curve and a single point, and link each.
  - Toggle a boundary with a feather.
  - Add and slide a correspondence.
  - Show Source / Destination / Both, and the output toggle.
  - The panel columns.
- Run the GUI bench if the diff touches the bench rule's paths.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-20 — **Duplicate and Link lands on top, as the selected destination (user, grill):** the warp starts at zero, so nothing jumps.
- 2026-10-20 — **Colours, a show toggle and a warped/unwarped toggle (user, grill).**
- 2026-10-20 — **A dedicated correspondence tool, with ends constrained to their curves (user, grill).**
- 2026-10-20 — **Linking existing curves auto-aligns, then the user refines (user, grill).**
- 2026-10-20 — **Live feedback from a coarse field while dragging (user, grill).**
- 2026-10-20 — **No field or mesh preview overlay (user, grill).**
