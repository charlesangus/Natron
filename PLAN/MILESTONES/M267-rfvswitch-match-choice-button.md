# M267 - RFVSwitch Match Choice Button

Follow-up to M72 - RFV Integration And UAT. RFVSwitch gets a "Match Variable" button that configures the switch from the Choice variable named in its Variable knob: one pattern row per option, so the rows and inputs line up with the menu. Today the user types each option's label by hand (M72 decided a Choice variable is matched by its option label, `DECISIONS/2026-10-08-rfvswitch-design.md`). Depends on M72 - RFV Integration And UAT, which owns `Engine/Nodes/RFV/RFVSwitch.*`.

## Scope

**In:**
- A one-shot button on RFVSwitch. It reads the Choice variable declared in the project under the name in **Variable**, writes one pattern row per option, and sets the row count.
- A row for an option whose label has `*`, `?` or `[` is written as an escaped `re:` line, since globs have no escape character.
- Documentation in `Engine/Nodes/RFV/docs/RFVSwitch.md` and the button's tooltip.

**Out:**
- Live sync. The rows stay hand-editable and do not follow later edits to the variable's options (user, 2026-10-27).
- Reconnecting inputs by label. Inputs keep their index (user, 2026-10-27).
- Choice knobs that are not declared RFVs.
- Choice variables declared only by an RFVSet node. RFVSwitch's `choiceLabel` reads the project's declaration alone, so the button uses the same lookup and the patterns it writes are the ones the switch matches.
- macOS/Windows (the fork is Linux x86-64 only).

**Edge cases:**
- Variable empty, not declared in the project, or not a Choice: nothing is changed, and the node posts a message saying why.
- More than 16 options (`kRFVSwitchInputCount`): the first 16 rows are written and the message says how many options were left out.
- Fewer options than the current rows: surplus rows are emptied and hidden, and surplus inputs stay connected but unreachable until a row is added back, as with Remove today.
- An option with an empty label uses its id, as `choiceLabel` does. An option whose label and id are both empty gets an empty row, which never matches and keeps its input's place.
- Two options with the same label: both rows are written; the first wins at match time.
- A label starting `re:` is escaped like any regex line, so it is not read as a prefix.

**Assumptions:**
- Writing the rows leaves connected inputs alone, because input N is row N already.

## Phase 267.1: The button

- [ ] M267.P1.T1 — Pattern line for an option label
  - files: `Engine/Nodes/RFV/RFVPatternMatch.h`, `Engine/Nodes/RFV/RFVPatternMatch.cpp`, `Tests/RFVPatternMatch_Test.cpp`
  - approach: add `std::string patternForLabel(const std::string& label)` in namespace `RFV`. A label with none of `*`, `?`, `[` is returned unchanged, as a glob that matches exactly itself. Any other label returns `kPatternRegexPrefix` followed by the label with every ECMAScript special character (`\ ^ $ . | ? * + ( ) [ ] { }`) backslash-escaped. A label starting `re:` returns the escaped form too.
  - verify: gtest table: for each of plain, `a*b`, `x?`, `[v1]`, `re:x`, `1.5 (a+b)`, `firstMatch(label, {patternForLabel(label)}) == 0` and a near-miss value gives -1.
  - size: M

- [ ] M267.P1.T2 — "Match Variable" button on RFVSwitch
  - files: `Engine/Nodes/RFV/RFVSwitch.h`, `Engine/Nodes/RFV/RFVSwitch.cpp`
  - approach: add a `KnobButton` "Match Variable" (name `matchVariable`, `setEvaluateOnChange(false)`) on the page right after Variable, with a tooltip saying it writes one row per option of the Choice variable and leaves inputs where they are. In `knobChanged` call a new `matchVariableOptions()`. It finds the variable with `RFV::findDeclaredVariable(project, name)` as `choiceLabel` does and casts it to `KnobChoice`. It takes `getEntries_mt_safe()`, writes `RFV::patternForLabel(label.empty() ? id : label)` into rows 0..n-1 (n capped at `kRFVSwitchInputCount`), empties the other rows, and sets `patternCount` to max(n, 1), all inside `beginChanges()`/`endChanges()` as `insertRowAfter` does, then `refreshRows()`, `refreshInputsVisibility()`. It does not reconnect inputs. Failure cases follow Scope's edge cases and use the node's existing posted-message path (see `clearPostedMessage`).
  - verify: compiles clean under `tools/ci/local/syntax-check.py` for both files.
  - size: M

- [ ] M267.P1.T3 — Test the button
  - files: `Tests/RFVSwitch_Test.cpp`
  - approach: with the fixtures `ProjectVariableChoosesTheInput` uses, declare a project Choice variable with three options, one of them `a*b`, press the button, and check: three rows hold the expected patterns, `patternCount` is 3, an input connected to row 1 is still connected to it, and rendering with each option selected passes the matching input. Cases for a variable that is empty, undeclared and not a Choice (rows unchanged, message posted), and 20 options (16 rows, message names 4 left out). Fewer options than before: surplus rows empty.
  - verify: the new cases pass under both schedulers.
  - size: M

- [ ] M267.P1.T4 — Docs
  - files: `Engine/Nodes/RFV/docs/RFVSwitch.md`
  - approach: add the button under Parameters: what it writes, the escaped `re:` form for labels with glob characters, that it does not follow later option edits (press it again, and note that it overwrites the rows), the 16-row cap, and that inputs keep their index.
  - verify: the doc matches the tooltip and the tests' behaviour.
  - size: S

- [ ] M267.P1.T5 — Check the button in the GUI
  - files: none
  - approach: run the GUI under Xvfb per the repo recipe (`build/deeprepro/run-gui.sh`, fixtures under `build/`) at 1920x1080 scale 1. Declare a Choice variable, add an RFVSwitch, press the button, and screenshot the panel before and after. Share the screenshots with the user through SendUserFile before the task is checked off.
  - verify: rows, row count and input visibility refresh on the press; the user OKs the screenshots.
  - size: S

**Prior verification gate:** `RFVPatternMatch` and `RFVSwitch` gtests pass; full ctest passes; the user has OK'd the Xvfb screenshots (M267.P1.T5).

## Phase 267.2: Practical row capacity

- [ ] M267.P2.T1 — Raise RFVSwitch's effective row limit to 1024
  - files: RFVSwitch dynamic input/pattern storage, serialization and GUI tests
  - approach: Replace the 16-row cap with dynamic growth up to 1024 rows, keeping lazy input visibility and avoiding eager creation/layout work for unused rows.
  - verify: A 1024-option Choice matches, saves/reopens and renders the first, middle and final options without truncation or unusable panel latency.
  - size: M

**Verification gate:** RFVSwitch tests pass at 1, 16 and 1024 rows; the large-choice workflow is accepted in the replacement AppImage.
