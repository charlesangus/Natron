# M518 - Save Prompt Key Shortcuts

Let the "Save changes to …?" prompt, and every other question dialog, be answered from the keyboard with one letter: S saves, D discards, C or Escape cancels, Y and N answer yes/no prompts. Today `Gui::saveWarning()` (`Gui/Gui20.cpp:1142`) goes through `Dialogs::questionDialog` to `Gui::onDoDialog` (`Gui/Gui30.cpp:~398-420`, the `type == 3` branch), which builds a stock `QMessageBox` with standard buttons and no mnemonics. The "Do Not Show This Again" overload uses Natron's own `MessageBox` class (`Gui/MessageBox.cpp`) instead. A member of F36 - UI Polish Batch II.

## Scope

**In:**
- One-letter shortcuts on every standard button Natron's question dialogs show (Save, Discard, Cancel, Yes, No, Yes to All, No to All, Ok, Abort, Retry, Ignore…), underlined in the button text.
- A plain key press, without Alt, triggers the button.
- Both dialog paths: the stock `QMessageBox` in `Gui::onDoDialog` and Natron's `MessageBox`.

**Out:**
- Rewording any dialog text: none.
- New buttons or a "Save All" choice: none.

**Edge cases:**
- Two buttons whose names start with the same letter (Yes / Yes to All): the second gets its next unused letter (Yes to &All).
- Focus in a text field inside a dialog (none of the question dialogs have one today): letters type into the field, as Qt does.
- Escape keeps cancelling, Enter keeps choosing the default button.

## Phase 518.1: Shortcuts

- [x] M518.P1.T1 — Mnemonics on stock question dialogs (PR #350)
  - files: `Gui/Gui30.cpp`, `Gui/QtEnumConvert.{h,cpp}` (or a small helper beside it), `Tests/gui/save_prompt_keys.py`
  - approach: after building the `QMessageBox` in `Gui::onDoDialog`'s question branch, set each standard button's text through one helper mapping `StandardButtonEnum` to a translated label with an `&` mnemonic (`&Save`, `&Discard`, `&Cancel`, `&Yes`, `&No`, `Yes to &All`, `N&o to All`, `&Ok`…), resolving clashes as in Edge cases. `QMessageBox::keyPressEvent` already fires a button whose shortcut matches a plain key with no modifier; confirm that in the Qt 6.8 source and, if not, handle the key in a small event filter on the box.
  - verify: an Xvfb script modifies a project, triggers close, sends `d`, and asserts the project closed without writing the file; repeats with `s` (file written) and `c` and Escape (project still open, still modified).
  - size: M

- [x] M518.P1.T2 — Mnemonics on Natron's MessageBox (PR #350)
  - files: `Gui/MessageBox.{h,cpp}`, `Tests/gui/save_prompt_keys.py`
  - approach: use the same helper for the buttons `MessageBox` creates, and make a plain letter press trigger the matching button (`keyPressEvent` or the buttons' `setShortcut`), keeping Escape and Enter as they are.
  - verify: the Xvfb script opens a dialog that uses the "Do Not Show This Again" overload (e.g. the auto-save prompt, `Gui/GuiAppInstance.cpp:562`), presses `n`, and asserts the "No" result.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `Tests/gui/save_prompt_keys.py` passes under Xvfb at 1920x1080; a screenshot of the save prompt with underlined letters goes to the user (collected at F36 - UI Polish Batch II's UAT).
