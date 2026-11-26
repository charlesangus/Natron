# M614 - User Knob Editor UAT Round 2

Fixes from the user's UAT of the F16 - User Knob Editor AppImage (2026-11-26), needed before F16 merges. Member of F16 - User Knob Editor.

## Scope

**In:**
- A Delete button in the properties panel's edit mode for the selected user knob (also groups and pages), one undo step.
- Dragging a user knob back onto the palette (the strip of knob-type buttons at the top of edit mode) deletes it, one undo step.
- The selected-knob highlight sits clear of the knob with even padding on every side, top included, and reads as a deliberate selection frame rather than a tight outline.

**Out:**
- Anything not raised in this round.

## Phase 614.1: Fixes

- [x] M614.P1.T1 — Delete button and drag-to-palette delete
  - files: the in-panel editor (Gui/ edit-mode palette, property section, drag handling from M373 - In-Panel Knob Editor), the engine delete operation (M372)
  - approach: a Delete button in the property section (or the palette strip) that deletes the selected user knob/group/page through the existing undoable delete; dropping a dragged user knob on the palette strip deletes it the same way, with drop feedback while hovering the strip
  - verify: GUI script deletes a knob with the button and another by dragging it onto the palette, checks both are gone and that undo brings each back in place
  - size: M
- [x] M614.P1.T2 — Selection highlight with padding
  - files: the selected-knob highlight painting in the edit-mode panel
  - approach: draw the selection frame with even padding outside the knob row (no overlap, clear at the top), rounded corners and a consistent pen, matching the panel's spacing
  - verify: screenshots of a selected knob at the top of a page and mid-page sent to the user
  - size: S

**Verification gate:** f16 gate with the M373 GUI scripts plus the new one; screenshots OK'd by the user.

## Decisions
- 2026-11-26 (PM) — Claimed; lane `.worktrees/m614` off the feature branch; all tasks dispatched to one opus implementer.
- 2026-11-26 (PM) — m614-gate1 green (09fd6f0179, --appimage; the M373 scripts, m70_rfv and m614_delete pass). Screenshots sent; round-2 UAT AppImage staged as Natron-f16-user-knob-editor.AppImage (F16 + M614). Merges into F16 after the user's OK.
- 2026-11-26 — **User approved F16 round 2.** PR #391 merging into F16; then F16 merges main, gates, and PR #384 merges to main.
