# M326 - Legible Viewer Warning Banner

The viewer's persistent-message banner draws warnings as light grey text (`textRenderingColor`, 200,200,200) on a hard-coded yellow strip (`glColor4f(0.65, 0.65, 0)`), which is illegible. Warnings switch to near-black text on the same yellow; errors keep light text on dark red. While in the routine, the banner type is fixed so that several warnings stay yellow: today only a lone warning is yellow and any multi-message set turns red.

## Scope

**In:**
- Near-black text on the yellow warning banner.
- Banner is yellow when every collected message is a warning, red otherwise.

**Out:**
- Theme- or Settings-driven banner colours (user chose fixed colours, 2026-11-04).
- Changing the yellow or the dark red backgrounds, or the error text colour.
- Text shadow/outline support in `TextRenderer`.
- Other viewer text (resolution overlay, "Overlays off") that shares `textRenderingColor`.

**Edge cases:**
- Info (0) and question (3) messages mixed with warnings: not all warnings, so red, as today.

## Phase 326.1: Banner Colours And Type

- [ ] M326.P1.T1 — Draw warning banner text in near-black
  - files: `Gui/ViewerGL.cpp` (`drawPersistentMessage`, ~:1181-1228)
  - approach: In `drawPersistentMessage`, pick the text colour by banner type: for the warning strip pass a near-black `QColor` (e.g. 20,20,20) to `renderText`; for the error strip keep `_imp->textRenderingColor`. Do not change `textRenderingColor` itself — the resolution overlay uses it. Backgrounds unchanged.
  - verify: Builds (syntax-check `Gui/ViewerGL.cpp`); diff shows only the text-colour choice changed and the error path still uses `textRenderingColor`.
  - size: M
- [ ] M326.P1.T2 — Keep the banner yellow when all messages are warnings
  - files: `Gui/ViewerGL.cpp` (`updatePersistentMessageToWidth`, ~:3754-3802)
  - approach: Today the banner type is warning (2) only when exactly one message exists and it is a warning; otherwise error (1). Change it to warning when every collected message is `eMessageTypeWarning` (`Global/Enums.h` ~:94-100), error otherwise. Empty-list behaviour unchanged.
  - verify: Syntax-check passes; reading the loop shows one warning → 2, two warnings → 2, warning + error → 1, warning + info → 1.
  - size: M

## Phase 326.2: GUI Check

- [ ] M326.P2.T1 — Screenshot the warning and error banners under Xvfb
  - files: a new GUI script under `tools/` beside the existing viewer GUI checks (follow the repo's remote GUI-check convention, `tools/ci/remote/build.sh --gui`)
  - approach: Load a small graph into the viewer, raise a warning persistent message on one node (via the Python persistent-message API if exposed; otherwise a node condition that posts a warning, see `Engine/Node.cpp` ~:2667), screenshot; raise two warnings on two upstream nodes, screenshot; add an error, screenshot. Assert banner colour by sampling a banner pixel from the grabbed viewer image (yellow, yellow, red).
  - verify: The script passes under Xvfb (locally or remotely); the three screenshots go to the user one file each, and the user confirms the warning text is legible.
  - size: M

**Verification gate:** fast build passes; the M326.P2.T1 GUI script passes under Xvfb; the user has seen the screenshots and approved legibility (UAT on a fast AppImage with a steps .md).

## Decisions

- 2026-11-04 — Dark text on the existing yellow, not a darker background or theme colours (user, /cat-plan).
- 2026-11-04 — Fix the multi-warning → red routing in this milestone (user, /cat-plan).
