# M432 - Overlays Off The Render Path

Show and edit a node's viewer overlay even when the viewer is not downstream of it. The common case is Roto shapes while viewing the plate that feeds the Roto. The overlay is placed by the graph path between the overlay node and the viewed node. When the viewed node is upstream, the overlay maps back through the inverse of the ops between them. When the two sit on sibling branches, it maps through their nearest shared ancestor. When they are unrelated, it draws in the node's own canonical space. This milestone maps only through what `getTransform` already exposes, which is matrices and time. Non-linear warps come with M435 - Overlays Deform Through Warps.

Scouted 2026-11-14 on `origin/main`:
- `Gui::getNodesEntitledForOverlays` (`Gui/Gui50.cpp:810`) already returns every open-panel node with an overlay, with no upstream test.
- `ViewerTab::drawOverlays` (`Gui/ViewerTab20.cpp:76`) and every pen/key handler in that file then skip a node when `getOverlayTransform` fails and `Settings::viewerOverlaysPath()` is on. That preference is "Only display overlays for the viewer render path" (`Engine/Settings.cpp:1322`, default on, `:1863`), checked at 13 call sites in `ViewerTab20.cpp`.
- `ViewerTabPrivate::getOverlayTransform` (`Gui/ViewerTabPrivate.cpp:163`) walks from the viewer input down to the target. It multiplies `getTransform_public` matrices (draft mode) and prefers non-optional inputs, last to first.
- `getTimeTransform` (`:285`) maps time the same way through `getFramesNeeded`.

## Scope

**In:**
- Remove the `viewerOverlaysPath` preference. Every node `getNodesEntitledForOverlays` returns always draws its overlay and takes pen and key events.
- Path resolution in both directions:
  - viewed node downstream of the overlay node: today's forward matrix walk.
  - viewed node upstream of it: the inverse of the composed matrix.
  - siblings: inverse to the nearest common ancestor, then forward to the viewed node.
  - unrelated: identity, in canonical space.
- A per-viewer toolbar toggle, "Map Sibling Overlays", with a shortcut. On by default. Off draws sibling-branch overlays in their own canonical space.
- Time mapping inverted where it can be: offsets and retimes map the viewer frame to the overlay node's matching frame. Across a FrameHold the overlay shows only while the viewer is on the held frame.
- Off-path overlays look exactly like on-path ones and are fully editable. Pen positions map back to the node's space through the same path.

**Out:**
- Non-linear warps (LensDistortion, SplineWarp, STMap, IDistort…) on the path: they pass through as identity here. M435 - Overlays Deform Through Warps owns them.
- Overlays carried onto 3D geometry: M436 - Overlays On Projected Geometry.

**Edge cases:**
- Several routes between the two nodes, such as a Merge fed the plate on A and B: use the existing preference order. That is non-optional inputs before optional ones, last input to first, the same as `getOverlayTransform` today.
- A singular composed matrix (a scale of 0, a degenerate CornerPin) on an inverse leg: the overlay is hidden for that frame and takes no events.
- A FrameHold on the path while the viewer is on another frame: hidden and takes no events.
- A Group as the target: resolved through its output node, as today.
- Disabled nodes on the path are identity, as today.

**Assumptions:**
- Unconfirmed: the shortcut is an unused key chosen at implementation from the viewer shortcut table, and registered like the other viewer toggles so it can be rebound.
- Removing the preference needs no project or preference migration. The fork makes no backward-compatibility promise, so a stale settings key is ignored.

## Phase 432.1: Path resolution

- [ ] M432.P1.T1 — Bidirectional overlay path resolver
  - files: `Gui/ViewerTabPrivate.{h,cpp}` (replace `getOverlayTransform`/`getTimeTransform` with a resolver), new `Engine/OverlayPath.{h,cpp}` holding the graph search so it is unit-testable without a GUI, CMake source lists, `Tests/OverlayPath_Test.cpp`
  - approach:
    - `resolveOverlayPath(viewedNode, overlayNode, time, view, mapSiblings)` returns a relation (downstream, upstream, sibling, unrelated), a composed 3x3 canonical matrix, the mapped time, and a `visible` flag.
    - Downstream reuses today's forward walk, including its input preference order and PAR handling.
    - Upstream walks from the overlay node to the viewed node and inverts the composed matrix. `visible = false` when it is singular.
    - Sibling finds the nearest common ancestor by a BFS over inputs from both ends, preferring the same input order. It composes inverse(overlay → ancestor) with forward(ancestor → viewed).
    - Time maps through each leg with `getFramesNeeded`, as `transformTimeForNode` does. A frame op whose needed range for every output frame is one constant frame is a hold: that leg is visible only when the viewer time equals the held frame.
    - Groups resolve through `getOutputNodeInput`.
    - Keep it in Engine so the GUI only consumes it.
  - verify: gtest — chains built in a headless app:
    - Read → Transform(translate 10) → Roto, viewing Read: Roto maps by translate(-10).
    - Viewing a downstream Transform: translate(+10), as before.
    - Sibling via a shared Read with a Transform on each branch: composed correctly.
    - mapSiblings = false: identity.
    - Unrelated: identity.
    - Scale 0: not visible.
    - TimeOffset +5: time maps t → t+5 upstream and t → t-5 downstream.
    - FrameHold 10: visible only at viewer time 10.
  - size: L

## Phase 432.2: Viewer wiring

- [ ] M432.P2.T1 — Draw and route events through the resolver; drop the preference
  - files: `Gui/ViewerTab20.cpp` (all 13 `viewerOverlaysPath()` sites, `drawOverlays`, pen and key handlers), `Gui/ViewerTabPrivate.{h,cpp}`, `Engine/Settings.{h,cpp}` (remove `_viewerOverlaysPath`, its knob and accessor), any Python binding or typesystem entry exposing it (grep `viewerOverlaysPath`)
  - approach:
    - Replace each `getOverlayTransform` + `getTimeTransform` + preference check with one `resolveOverlayPath` call.
    - Skip a node only when `!visible`.
    - Draw with the resolved matrix as the GL modelview (as `:152–170` does) at the resolved time.
    - Map pen positions with the matrix's inverse, as the transformed position is computed today.
    - Remove the preference knob and every reference to it.
  - verify: syntax-check the touched files; then, under the Xvfb recipe, a Read → Transform(translate 50) → Roto scene viewed at the Read shows the Roto shape offset by -50 (screenshot shared with the user).
  - size: M

- [ ] M432.P2.T2 — Map Sibling Overlays toggle and shortcut
  - files: `Gui/ViewerTab.{h,cpp}` / `Gui/ViewerTab10.cpp` (toolbar button beside the other viewer toggles), `Gui/GuiApplicationManager10.cpp` or wherever viewer shortcuts are registered (grep an existing viewer toggle's shortcut ID), `Gui/Resources/Images` icon if the other toggles use one, `Gui/ViewerTabPrivate.h` (state), the call sites from P2.T1 (pass the flag)
  - approach:
    - A checkable toolbar button, "Map Sibling Overlays", on by default.
    - Its tooltip says that off draws overlays from other branches in their own space.
    - It is per-viewer state, saved with the viewer's GUI state the way the other per-viewer toggles are.
    - A rebindable shortcut on an unused key, checked against the shortcut table.
    - Toggling redraws the overlays.
  - verify: syntax-check; under Xvfb, toggling the button and pressing the shortcut both flip a sibling-branch Roto between mapped and canonical placement (screenshots shared with the user).
  - size: M

## Phase 432.3: Tests and docs

- [ ] M432.P3.T1 — GUI test: off-path Roto on the plate
  - files: `Tests/gui/m432_overlays_off_path.py` (on `Tests/gui/guitest.py`), the GUI test registration list
  - approach:
    - Build Read(constant) → Transform(translate 50,0) → Roto, with one Bezier, and open the Roto panel.
    - View the Read. Assert the Roto overlay is drawn: query the overlay draw count, or the point's mapped viewport position, through the test hooks other overlay GUI tests use.
    - Drag one point by the viewer-space offset (+20,0). Assert the Roto point's knob moved by (+20,0) in Roto space.
    - Branch a second Transform off the Read, view it, and assert the sibling mapping.
    - Toggle Map Sibling Overlays off and assert identity placement.
    - Run under the Xvfb recipe.
  - verify: the script passes under `build/m63-gui/run-gl-tests.sh` or the remote `--gui` gate.
  - size: M

- [ ] M432.P3.T2 — Docs
  - files: the viewer docs page that lists overlay behaviour and preferences (grep `docs/` for "overlays for the viewer render path" or the viewer toolbar page)
  - approach: remove the preference, describe off-path overlays, the sibling toggle and its shortcut, and the FrameHold rule. No Nuke mentions.
  - verify: the docs build (`sphinx` job or local `tools/ci` docs check), or render the page.
  - size: S

**Verification gate:** OverlayPath_Test and `m432_overlays_off_path.py` pass, and the full ctest suite is green on a remote release build with `--gui` including the new script. The GUI bench rule doesn't apply, since no `Gui/NodeGraph*`, `NodeGui*` or `Edge*` files change. Build a UAT AppImage with a steps `.md`: Roto on the plate behind a Transform, a sibling branch, the toggle, and a TimeOffset.

## Decisions

- 2026-11-14 — Off-path overlays map by graph path (upstream inverse, sibling via nearest common ancestor, unrelated in canonical space) (user, grill): a Roto behind a Transform has to land on the plate it was drawn for.
- 2026-11-14 — Remove the `viewerOverlaysPath` preference (user, grill): one behaviour, no hidden mode.
- 2026-11-14 — Time ops invert where possible; a FrameHold shows the overlay only on the held frame (user, grill).
- 2026-11-14 — Off-path overlays are fully editable and drawn the same as on-path ones (user, grill): editing on the plate is the point.
- 2026-11-14 — Sibling mapping has a per-viewer toolbar toggle with a shortcut, on by default (user, grill: "as 1, but with a toggle", "and a shortcut").
- 2026-11-17 — P1.T1, P2.T1, P2.T2 and P3.T1 code in the lane (.worktrees/m432); gate running.
  - Each node on the path maps time at its own mapped time; the forward walk remembers visited nodes; "singular" is |det| < 1e-24.
  - Pen-down passes the pen's timestamp, key events use the current view, and a retimed overlay on pen-up or key-up skips that node only.
  - The toggle is viewer knob mapSiblingOverlays ("Map Sibling Overlays", default on), with a "Siblings" text button and Shift+O. The viewerOverlaysPath Preference is removed.
  - PM calls: retimed overlays stay locked (dimmed, no edits, double-click included); the FrameHold rule goes in Engine/Nodes/Time/docs/FrameHold.md (P3.T2), since there is no viewer docs page; the button stays text-labelled until the user sees the screenshots.
  - Seen, not touched: key repeat never fills its node list, so overlays get no key-repeat events (predates this milestone).
