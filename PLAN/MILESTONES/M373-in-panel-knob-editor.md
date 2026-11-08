# M373 - In-Panel Knob Editor

The GUI half of F16 - User Knob Editor. User knobs are managed today through modal dialogs:
`ManageUserParamsDialog` (Add, Pick, Edit, Delete, Up, Down), `AddKnobDialog` (the property
form) and `PickKnobDialog` (copy/link a knob from another node, used to expose inner knobs on
Groups and PyPlugs). This milestone replaces all three with an edit mode on the properties
panel itself: a pencil toggle in the panel header, a type palette strip at the top, a
collapsible property section below the knobs, drag to add and reorder, drag knobs in from
other panels, and copy/paste. Every operation goes through M372 - User Knob Engine Edits' API,
so it is in place, undoable one step at a time, and recorded for live sessions.

Code facts, read at `origin/main` on 2026-11-08; re-check line numbers before starting.
- Entry points: `DockablePanel::onRightClickMenuRequested` adds "Manage user parameters..."
  (`Gui/DockablePanel.cpp` ~1600-1633, disabled unless the effect is editable);
  `Gui/NodeSettingsPanel.cpp` ~191 connects a `manageUserParams` action; the handler
  `DockablePanel::onManageUserParametersActionTriggered` (~1637-1664) confirms before editing
  a PyPlug and calls `setPyPlugEdited(true)`.
- The panel header has `HeaderModeEnum` and an insert-button API (`Gui/DockablePanel.h` ~65-95).
  Knob widgets are laid out by `Gui/KnobGuiContainerHelper.cpp`.
- Knob-to-knob drag already exists for linking (drop handlers in `Gui/KnobGuiBool.cpp`,
  `KnobGuiChoice.cpp`, `KnobGuiString.cpp`, drag start in `KnobGuiColor.cpp` and others).
- `AddKnobDialog` also renames project path variables through `RenamePathVariableCommand`
  (`Gui/PathVariableUndo.h`, used at `AddKnobDialog.cpp` ~1967-1971); that behaviour must move,
  not vanish.

## Scope

**In:**
- Edit mode on any `DockablePanel` whose holder supports user knobs; this milestone enables it
  on node panels (including Groups and PyPlugs). M247 - Project User Knobs And Root enables it
  on Project Settings.
- Pencil toggle in the panel header and a right-click "Edit parameters" entry; both disabled
  for non-editable holders.
- Palette with every type M372 supports, plus page, group, separator and label; click adds
  after the selection, drag drops at a position.
- Property section for the selected knob, generated per type from `UserKnobSpec`, with type
  change (and a confirm when it must reset) and a button-script editor.
- Drag to reorder user knobs, groups and pages among user pages, and user pages among
  built-in pages.
- Drag a knob in from any open panel, including a Group's inner nodes: plain drop makes a
  linked user knob, Ctrl-drop an unlinked copy. A "Link knob..." palette entry chooses a node
  and knob when the panel isn't open.
- Copy and paste of user knobs, groups and pages between panels (Ctrl+C/Ctrl+V and the context
  menu), through the system clipboard.
- Removal of `ManageUserParamsDialog`, `AddKnobDialog` and `PickKnobDialog`.

**Out:**
- Engine behaviour (in-place edits, conversion, undo, button scripts): M372 - User Knob Engine
  Edits.
- Project Settings panel: M247 - Project User Knobs And Root.
- Hiding, relabelling or moving built-in knobs, and dropping user knobs onto built-in pages.
- Saved user-knob presets.

**Edge cases:**
- Entering edit mode on a PyPlug instance asks once, as today, and marks it edited.
- Built-in knobs are shown dimmed and are not selectable or draggable in edit mode.
- Outside edit mode, the existing knob-to-knob link drag behaves exactly as today; the new
  drop targets exist only in edit mode.
- Deleting a knob that others link to or express on asks first, listing the dependents.
- Leaving edit mode, closing the panel or deleting the node with edit mode on commits any
  pending field edit and exits cleanly.
- Layouts fit a 1920x1080 screen at scale 1 with the default panel width.

## Phase 373.1: Mockups

- [ ] M373.P1.T1 — Mockups of the edit mode for approval
  - files: new build/m373-mockup/ (not committed), Gui/DockablePanel.cpp and Gui/KnobGuiContainerHelper.cpp (read)
  - approach: Produce PNG mockups at 1920x1080 scale 1 of a Blur panel: edit mode off; edit mode on with the pencil, palette strip, a selected user knob and the property section for a float, a choice and a button (script editor); a drag-in drop indicator; the copy/paste context menu. Send each PNG to the user separately and park the milestone as `response` until approved; record the answer and any changes in `## Decisions`.
  - verify: The user's approval is recorded in this file's `## Decisions`.
  - size: M

## Phase 373.2: Editor core

- [ ] M373.P2.T1 — Edit mode toggle and selection
  - files: Gui/DockablePanel.cpp, Gui/DockablePanel.h, Gui/DockablePanelPrivate.cpp, Gui/DockablePanelPrivate.h, Gui/KnobGuiContainerHelper.cpp, Gui/KnobGuiContainerHelper.h, Gui/NodeSettingsPanel.cpp, new Gui/KnobEditMode.h, new Gui/KnobEditMode.cpp, Gui/CMakeLists.txt, new Tests/gui/m373_edit_mode.py
  - approach: Add the pencil toggle to the panel header and replace "Manage user parameters..." with "Edit parameters" in the right-click menu and `NodeSettingsPanel`'s action; both toggle a `KnobEditMode` object owned by the panel. Entering keeps the PyPlug confirm and `setPyPlugEdited(true)`. In edit mode, user knobs, groups and pages are selectable (highlight per the mockup), built-ins dimmed. Leaving, panel close and node deletion exit cleanly.
  - verify: Build. m373_edit_mode.py under Xvfb: toggle on a Blur via the pencil and the menu, select a user knob, check built-ins can't be selected, toggle off; on a non-editable holder both entries are disabled; screenshots saved.
  - size: L

- [ ] M373.P2.T2 — Palette strip with click to add
  - files: Gui/KnobEditMode.cpp, Gui/KnobEditMode.h, new Gui/KnobEditPalette.h, new Gui/KnobEditPalette.cpp, Gui/CMakeLists.txt, new Tests/gui/m373_palette.py
  - approach: A palette strip at the top of the panel in edit mode with one entry per type (M372's full set plus page, group, separator, label). Click creates the knob through `createUserKnob` with a unique default name after the selection (or on the first user page, creating one if none exists), selects it and focuses the property section's name field.
  - verify: m373_palette.py adds one knob of every type to a Blur, checks each exists with the right type, and that one Ctrl+Z removes the last one.
  - size: L

- [ ] M373.P2.T3 — Property section
  - files: Gui/KnobEditMode.cpp, Gui/KnobEditMode.h, new Gui/KnobEditProperties.h, new Gui/KnobEditProperties.cpp, Gui/CMakeLists.txt, new Tests/gui/m373_properties.py
  - approach: A collapsible section below the knobs showing fields for the selected knob built from `specFromKnob` (common fields, then the type's own: ranges, defaults per dimension, menu items with help, text/file/path/layer flags, parametric curves, button script in a monospace multi-line editor). Commit a field on editing-finished through `applySpec`, showing `validateSpec` errors inline instead of committing. The type combo calls `changeUserKnobType` and asks before a reset. Port the project path-variable rename (`RenamePathVariableCommand`) from AddKnobDialog so renaming a path variable still updates its users.
  - verify: m373_properties.py: rename a user float that another node expresses on (expression rewritten), change its range and tooltip, change Int (animated)→Float keeping keys, try a duplicate name (inline error, nothing changes), set a button script and click the button; each edit is one Ctrl+Z.
  - size: L

- [ ] M373.P2.T4 — Drag to reorder and drag from the palette
  - files: Gui/KnobEditMode.cpp, Gui/KnobEditMode.h, Gui/KnobEditPalette.cpp, Gui/KnobGuiContainerHelper.cpp, new Tests/gui/m373_reorder.py
  - approach: In edit mode, drag a user knob, group or page to a new position among user pages (drop indicator per the mockup), and drag user page tabs among built-in pages; drag a palette entry to create at the drop position. Moves go through the M372 API as one undo entry each. Built-in pages and knobs are never drop targets for knobs.
  - verify: m373_reorder.py moves a knob into a group, a group to another user page, a user page before a built-in page, and drags a palette entry to a position; checks order after each and after undo.
  - size: L

## Phase 373.3: Drag-in, copy/paste, dialog removal

- [ ] M373.P3.T1 — Drag knobs in from other panels, and Link knob...
  - files: Gui/KnobEditMode.cpp, Gui/KnobEditMode.h, Gui/KnobGui.cpp, Gui/KnobGuiPrivate.h (or wherever knob drag starts), Gui/KnobEditPalette.cpp, new Tests/gui/m373_drag_in.py
  - approach: While a panel is in edit mode, accept knob drags from any other panel: plain drop creates a linked user knob via `createDuplicateOnHolder` (alias), Ctrl-drop an unlinked copy, inside one undo entry. For a Group or PyPlug, its inner nodes' panels are valid sources. Add a "Link knob..." palette entry that opens an inline node-then-knob chooser (reuse PickKnobDialog's node list logic: the holder's node plus a Group's inner nodes). Outside edit mode the existing knob-to-knob link drag is untouched.
  - verify: m373_drag_in.py drags a Blur's size onto a Group's panel in edit mode (linked: changing one changes the other), Ctrl-drags another (independent), uses Link knob... for an inner node, and checks that a normal link drag between two panels outside edit mode still links as before.
  - size: L

- [ ] M373.P3.T2 — Copy and paste between panels
  - files: Gui/KnobEditMode.cpp, Gui/KnobEditMode.h, new Tests/gui/m373_copy_paste.py
  - approach: In edit mode, Ctrl+C/Ctrl+V and context-menu Copy/Paste on selected user knobs, groups or pages, through `copyUserKnobs`/`pasteUserKnobs` and the system clipboard (a Natron MIME type plus plain-text YAML). Paste goes after the selection on the target panel.
  - verify: m373_copy_paste.py copies a user page with a group and knobs from one Blur, pastes into a second Blur's panel, checks definitions and values, and that one Ctrl+Z removes the paste.
  - size: M

- [ ] M373.P3.T3 — Remove the old dialogs
  - files: Gui/ManageUserParamsDialog.cpp, Gui/ManageUserParamsDialog.h, Gui/AddKnobDialog.cpp, Gui/AddKnobDialog.h, Gui/PickKnobDialog.cpp, Gui/PickKnobDialog.h, Gui/CMakeLists.txt, Gui/DockablePanel.cpp, any other includer found by grep
  - approach: Delete the three dialogs and every reference (includes, actions, CMake). Anything still used — validation, the path-variable rename, PickKnobDialog's node listing — must already live in M372's API or the editor (P2.T3, P3.T1); move any leftover the grep finds rather than keeping a dialog file.
  - verify: Build; `git grep -n 'ManageUserParamsDialog\|AddKnobDialog\|PickKnobDialog'` finds nothing; all m373_*.py GUI scripts still pass.
  - size: M

**Verification gate:** full build and ctest green; every `Tests/gui/m373_*.py` script passes under Xvfb at 1920x1080. Screenshots of the real editor and a fast-preset UAT AppImage with a steps `.md` are collected at F16 - User Knob Editor's UAT (Feature rule).

## Decisions

- 2026-11-08 — In-panel editor replaces the dialogs, which are removed (user, grill).
- 2026-11-08 — The editor works on any panel; M247 - Project User Knobs And Root enables it on Project Settings and depends on this milestone (user, grill).
- 2026-11-08 — Pick is replaced by dragging knobs in (plain drop links, Ctrl-drop copies) plus a Link knob... palette entry (user, grill).
- 2026-11-08 — Edit mode rearranges user pages only; built-in knobs stay read-only and in place (user, grill).
- 2026-11-08 — Palette and property section live inside the panel, with a pencil toggle in the header (user, grill).
- 2026-11-08 — Mockups are approved before the editor is built (user, grill).
