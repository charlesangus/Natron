# Milestone 45: Port tabtabtab-nuke as the native tab menu

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Port github.com/charlesangus/tabtabtab-nuke as Natron's native tab/create-node
menu (fuzzy search + smart insertion).

Blocked on: not yet prioritized — captured as backlog (user request
2026-09-18); needs a look at tabtabtab-nuke's behavior/license and how it
maps onto Natron's existing node-creation menu before elaboration.

Acceptance sketch:
- Pressing the tab-menu shortcut in the node graph opens a fuzzy-searchable
  create-node menu with tabtabtab-style smart insertion onto the selected
  node/pipe.
