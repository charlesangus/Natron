# M588 - Light Linker Drag And Drop Panel

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A custom panel for USDLightLink, the node from M567 - USD Light Linking: lights in a tree on the left, model prims on the right, and rule cards in the middle. Dragging a light or a prim onto a card creates a rule or adds an include or exclude, and existing links show as read-only cards. It edits the same rule rows as the plain knob panel. Design reference: Solaris Light Linker's custom UI (`.local/docs/houdini/nodes/lop/lightlinker.md`).

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead); M567 - USD Light Linking landing.

Acceptance sketch:
- Dragging a light into the middle column creates a rule row, and dragging prims onto its card fills Includes or Excludes.
- Edits made in the panel and in the knob panel show up in both.
