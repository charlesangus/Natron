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

### 2026-10-08T12:15:00+00:00 — change-request
- refs: M69
- Append a new section to the M69 stub (apply after the M69-creation entry above) and add one line to its intent. Stub stays a stub; do not elaborate without a user go-ahead. Section to append to `PLAN/MILESTONES/M69-minor-cleanup.md`, under the existing groups:

````markdown
Viewer colour picker:
- **Pick scene-referred values.** The picker's tooltip says it reads the display-referred value from the viewer and converts it back to linear. That is wrong: it must return the scene-referred value at the picked pixel, before any viewer transform (display/view, gain, gamma, channel display). Currently `ViewerGL::getColorAt` / `getColorAtRect` (Gui/ViewerGL.cpp) take a `linear` flag from the `getColorPickerLinear` setting; confirm at elaboration whether the values are read from the post-transform texture, and fix so the picker samples the pre-transform image data. The tooltip and the `getColorPickerLinear` setting are then likely obsolete.
- **Pick from a node's input or output.** Add a viewer picker mode that reads the colour on the viewed node's **input** or on its **output**. The default is the node's **input**, the opposite of Nuke (where it is the output). Decide at elaboration where the mode lives (viewer toolbar or the picker's own control) and how it behaves when the viewed node has several inputs (the one feeding the viewer's active input, or the first).
````
