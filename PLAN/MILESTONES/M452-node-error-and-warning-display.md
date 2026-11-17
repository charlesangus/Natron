# M452 - Node Error And Warning Display

Requested by the user on 2026-11-16 during the node graph UAT of M397 - Label Resize Keeps Node Top-Centre and M374 - Remove Node Magnify On Ctrl-Scroll: a node's error and warning text is drawn over the node in a very ugly way. Today `NodeGui::onPersistentMessageChanged` (`Gui/NodeGui.cpp`, ~2902-2944) shows a 25 pt "ERROR" (128,0,0) or "WARNING" (180,180,0) `NodeGraphSimpleTextItem` centred over the node body (positioned in `refreshSize()`, ~1582-1603), and the message itself only in the tooltip. The corner state indicator (`refreshStateIndicator()`, ~3060-3090) uses a different warning colour (80,180,0). This milestone replaces the overlay with a compact, readable design and shows the message itself.

## Scope

**In:**
- Remove the big centred "ERROR"/"WARNING" word over the node body.
- A small severity badge (error / warning icon) at the node's top-right corner, and a 2 px border in the severity colour.
- A one-line caption under the node with the first line of the message, small font, in the severity colour, elided to at most twice the node's width; the full message stays in the tooltip on node, badge and caption.
- One error colour and one warning colour (amber) shared by badge, border, caption and the corner state indicator.
- Level of detail: below the zoom level at which node labels stop being readable, the caption hides; the badge and border stay.

**Out:**
- The viewer's persistent-message overlay (`ViewerGL::drawPersistentMessage`) — none.
- Info-level messages (tooltip only today) — unchanged.
- Missing-plugin nodes' dashed red outline — unchanged.
- How errors are raised or cleared — none.

**Edge cases:**
- Multi-line message: only the first line is captioned; the tooltip has all of it.
- Error and warning together: error wins.
- Backdrops: no badge or caption (they carry no persistent messages today; keep it that way).
- The caption must not overlap the label of a node directly below; it sits in the node's own bounding rect so the graph's collision and selection use it.
- A node resized by its label (M397 - Label Resize Keeps Node Top-Centre): badge and caption re-anchor in `refreshSize()`.

**Assumptions:**
- Icons can come from the existing `Gui/Resources` set or be drawn as simple shapes; no new asset pipeline.

## Phase 452.1: New indicators

- [x] M452.P1.T1 — Severity badge and border replace the centred tag
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: in `onPersistentMessageChanged`, stop showing `_persistentMessage` as the centred tag; add a small `QGraphicsItem` badge (icon or drawn shape) anchored top-right in `refreshSize()`, and draw the node outline in the severity colour while a message is set. Replace `persistentErrorColor()` and the two warning colours with one error and one amber warning colour, used by the corner state indicator too. Keep the tooltip on the node and badge.
  - verify: syntax-check the files; a GUI script (P2.T1) sees the badge and no centred tag.
  - size: M

- [x] M452.P1.T2 — Message caption under the node
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: a caption text item below the node body with the message's first line, elided with `QFontMetrics::elidedText` to twice the node width, small font, severity colour; included in the node's bounding rect; repositioned in `refreshSize()`; hidden below the zoom level where the node graph stops drawing labels legibly (reuse the graph's existing LOD check if there is one, otherwise the view's scale threshold). Tooltip shows the full message.
  - verify: syntax-check; P2.T1 covers it.
  - size: M

## Phase 452.2: GUI check

- [x] M452.P2.T1 — GUI script for error and warning display
  - files: `Tests/gui/m452_node_error_display.py` (new)
  - approach: create a Read on a missing file (error) and a node raising a warning (find one that does, or set the message through the Python API if exposed), plus one with a long multi-line message; read back badge visibility, caption text (elided, first line), outline colour and that the centred tag is gone; zoom out past the threshold and check the caption hides; screenshot each state.
  - verify: the script passes in the remote gate and fails on main.
  - size: M

**Verification gate:** build and ctest green; the GUI script passes under the remote gate; GUI bench (`tools/bench/gui/run.sh --xvfb` + `compare.py`) shows no regression; screenshots of an error node, a warning node, a long message and the zoomed-out graph sent to the user (one file each) and approved before `done`.

- 2026-11-16 — All three tasks implemented (76a99286a on milestone/m452-node-error-and-warning-display). Deviations: geometry is set from NodeGui::resize() (refreshSize() doesn't exist); boundingRect() is not widened, since edges attach to it, and node placement uses a new sceneFootprint() including the caption; the badge sits on the top edge beside the expression badge; placeholder (missing-plugin) nodes keep their dark-red outline and get no badge. The engine keeps one message per node, so error-and-warning-together can't arise. Gate run 43982171441; screenshots for sign-off at the Feature UAT.
- 2026-11-17 — Review round 1 (Codex, PR #305) fixed in 1041111e2. The shown caption counts for collision, placement, framing and rubber-band selection, but not for boundingRect(), so edges stay on the body. The badge calls prepareGeometryChange() before changing shape. The caption is the message's first line, hidden if that line is empty. Gate 44140484584 green. Merged into feature/f29-ui-polish-batch. UAT note: rubber-band selection must enclose a long caption.
