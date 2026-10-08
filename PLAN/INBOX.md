## Pending

### 2026-10-08T04:45:00+00:00 — change-request
- refs: M69 (new), M55, M56, M67
- Add a new milestone M69 "Minor cleanup" as a **stub**, todo, placed at the end of the board (no execution order set; user said "some later" milestone). Create `PLAN/MILESTONES/M69-minor-cleanup.md` with the content below and add the board row `| M69 | Minor cleanup: keyer redesign, app-level hidpi/interactive, node-graph input handling, curve-editor handles | todo | [M69-minor-cleanup.md](PLAN/MILESTONES/M69-minor-cleanup.md) |`. Do not start or elaborate without a user go-ahead.

````markdown
# Milestone 69: Minor cleanup

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A grab-bag of small UX and design fixes from the user (2026-10-08), grouped to ship together. Each item is independent.

Keying and node knobs:
- **Keyer redesign.** The current design is unintuitive. It becomes four values: below A the matte is 0, between B and C it is 1, above D it is 0 (a trapezoid, with A<=B<=C<=D). It only creates an alpha: no premult, no compositing over a background, no despill. The native Keyer from M67.P5.T1 is the node to redo. Decide at elaboration whether ChromaKeyer shares the new matte stage.
- **No `hidpi` knob on nodes.** DPI handling becomes an app-level concern. Remove the knob from every node that carries it (the native generators and Transform-family nodes from M67 included) and handle scaling in the app (preference or automatic).
- **`interactive` is not a node property.** Replace the per-node knob with a global toggle with a keyboard shortcut. Remove the knob from nodes (M67's native nodes included).
- **Move "(un)premult by" below the line** with mask and mix, as should clamp black/clamp white on the nodes that have those knobs.

Node graph interaction:
- **Paste attaches.** Pasting a node while another node is selected connects the pasted node's main input to the selected node.
- **Unconnected inputs stay visible** when the node is deselected (they currently disappear).
- **Inputs are easier to grab.** Unconnected input arrows have a clunky, small hitbox; enlarge it.
- **Shortcuts for connecting node inputs.**
- **Pick-up connection mode.** Clicking an input "picks it up" and it sticks to the cursor; clicking a node then sets that input to the clicked node. Panning and zooming still work while picked up; right-click or Escape puts it down (cancels).
- **Tab menu position.** The tab menu briefly opens at the extreme top-left and then jumps to the cursor; it must open at the right position the first time.

Curve editor:
- **Full handle control.** Dragging a handle lengthens/shortens it; handles can be broken (independent in/out tangents) and re-joined.

Blocked on: a codebase-scouting pass (graph input arrow hit-testing, the tab menu show path, curve-editor tangent model, where `hidpi`/`interactive` are consumed), and a design answer on where the global interactive toggle and the app-level DPI setting live. The keyer and knob items should wait until M67 has landed and been UAT'd, since they edit nodes that milestone writes. Overlaps with M55 (node-graph interaction) and M56 (the unpremult-by label); decide at elaboration whether to fold those items in or leave them.

Acceptance sketch:
- Keyer has four matte values and outputs an alpha only.
- No node has `hidpi` or `interactive` knobs; DPI is app-level and interactive is one global toggle with a shortcut.
- Pasting onto a selected node connects it; unconnected inputs remain visible when deselected and are easy to grab.
- Pick-up mode connects an input with a click, pans/zooms while held, and cancels on right-click/Escape.
- Tab menu opens in the right spot with no jump.
- Curve-editor handles can be dragged, lengthened, shortened and broken.
````
