# M596 - Remove Old User Knob Dialogs

Delete the old modal user-knob dialogs, `ManageUserParamsDialog`, `AddKnobDialog` and `PickKnobDialog`, now that M373 - In-Panel Knob Editor edits node user knobs in place and M247 - Project User Knobs And Root gives Project Settings the same editor. This was M373.P3.T3, moved out because the project's path variables are edited through the old dialog until M247 lands (inbox, 2026-11-25).

## Scope

**In:**
- Delete `Gui/ManageUserParamsDialog.{h,cpp}`, `Gui/AddKnobDialog.{h,cpp}` and `Gui/PickKnobDialog.{h,cpp}`, with every include, action and call site.
- Port `Tests/gui/m70_rfv.py` from the dialogs to the in-panel editor.

**Out:**
- New editor features. Anything a dialog did that the editor lacks moves into M372's engine API, M373's editor or M247's project editor. No dialog is kept for it.

**Edge cases:**
- **Logic still used:** validation, the path-variable rename and PickKnobDialog's node listing must be found in M372, M373 or M247 code before the files go. If any is still only in a dialog, move it rather than keep the dialog.

## Phase 596.1: Removal

- [ ] M596.P1.T1 — Delete the three dialogs and their call sites
  - files: `Gui/ManageUserParamsDialog.{h,cpp}`, `Gui/AddKnobDialog.{h,cpp}`, `Gui/PickKnobDialog.{h,cpp}` (deleted), `Gui/DockablePanel.cpp` (include :75, dialog at ~:1699, `ManageUserParamsDialogPrivate` ~:1923), `Gui/ProjectGui.cpp` (include :69, `editVariables` ~:152), `Gui/CMakeLists.txt` if it lists the files
  - approach: first grep the three dialogs for logic that has no counterpart in `Engine/UserKnobEdit.cpp`, `Gui/KnobEditProperties.cpp`, `Gui/KnobEditMode.cpp` or M247's project editor: name validation, the path-variable rename (`RenamePathVariableCommand`) and the node listing. Move any leftover logic there. Then `git rm` the files, and remove the includes, menu actions and `ProjectGui::editVariables`. A Manage-dialog entry point that is still reachable opens the panel's edit mode instead.
  - verify: `tools/ci/local/syntax-check.py` on the touched TUs is clean; `git grep -n "ManageUserParamsDialog\|AddKnobDialog\|PickKnobDialog" -- Gui Engine` finds nothing.
  - size: M
- [ ] M596.P1.T2 — Port m70_rfv.py to the in-panel editor
  - files: `Tests/gui/m70_rfv.py`, with `Tests/gui/m373_*.py` as the reference for driving the editor
  - approach: replace the `AddKnobDialog`/`ManageUserParamsDialog` waits, clicks and type constants with the editor's palette and property panel, driven the way the m373 scripts do it. Keep every check: the pinned RFV group can't be edited or deleted, variables can be added and deleted, and reserved names (`class`, `render`) are refused, on both a node and the project.
  - verify: `m70_rfv.py` passes on the remote GUI runner; `git grep -n "AddKnobDialog\|ManageUserParamsDialog" Tests` finds nothing.
  - size: M

**Verification gate:** full build and ctest green; `git grep -n "ManageUserParamsDialog\|AddKnobDialog\|PickKnobDialog"` finds nothing; `m70_rfv.py`, `m247_project_user_params.py` and every `m373_*.py` GUI script pass on the remote GUI runner.
