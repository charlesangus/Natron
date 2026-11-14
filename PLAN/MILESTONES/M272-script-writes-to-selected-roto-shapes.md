# M272 - Script Writes To Selected Roto Shapes

Roto item knobs are slaved to the node's proxy knobs while selected. Selecting or deselecting copies one item's state into the others, so multi-selecting loses data and script writes fan out. This milestone replaces slaving in `RotoCurveContext` only: the proxy mirrors the last-selected item one way, and an edit on the proxy is replayed on each selected item's own knob, with one undo step. Knob-level slaving (`Engine/Knob*`, used by the tracker's `MultiInstancePanel`) is untouched.

Background (from the stub): found in M211 - Wavelet Retouch's GUI check (2026-10-28) as Python writes to a selected shape being lost; `Layer.getChildren()` returns plain `ItemBase` wrappers (M205's GUI check); and in M421 - Stroke Keyframe Indicators' review (2026-11-14) as multi-select data loss. The user asked that the slaving be fixed here, in one place. M421 reads each item's own values and depends on this fix for multi-select markers.

Root cause (consultant, at 7b1b9a9b6):
- Damage happens on select: `selectInternal` → `tieItemKnob` (`Engine/RotoCurveContext.cpp:1291`, `:1411`) copies item B into the node knob before slaving B; `cloneAndUpdateGui` (`Engine/KnobImpl.h:3084`) runs `refreshListenersAfterValueChange`, which copies the node knob into every item already slaved (`Engine/Knob.cpp:6095`, `:6143`).
- And on deselect: `deselectInternal` copies the deselected item into the node knob (`:1508`), pushing it into the items still selected; `untieItemKnob(copyState)` (`:1511`, `:1444`; `KnobImpl.h:1945`) copies back only when one item is left.
- Key edits copy the whole curve: `onMasterKeyFrameSet/Removed/Moved/AnimationRemoved` (`Engine/Knob.cpp:151-205`).
- A script write to a selected shape is no longer lost since M147's `onSelectedKnobValueChanged` (`:1386-1403`), but now goes item → node knob → every selected item, so it leaks across a multi-select.
- Item knobs have no holder (`Engine/RotoContextPrivate.h:960-1037`) and `isRecordedHolder` (`Engine/PythonUndoCommands.cpp:914-916`) skips them, so Python writes to shape params record no undo.
- Workarounds for slaving: `Engine/RotoDrawableItem.cpp:947`, `:1586-1597`; `Engine/Nodes/Filter/Retouch.cpp:198`, `:325`, `:1128`, `:1147`; `Engine/Nodes/Transform/AutoStraighten.cpp:1270`; `Tests/Native/RetouchTestSupport.h:117`; `Tests/RotoContentHash_Test.cpp:200`. The selected-item branch of `editedKnob` (`Gui/DopeSheetEditorUndoRedo.cpp:138`) goes inert.

## Scope
**In:**
- Roto, RotoPaint, Retouch, SplineWarp and AutoStraighten item knobs: select and deselect never write items.
- Edits on the panel or proxy, from the GUI or Python, reach every selected item, one step to undo.
- Python writes to a shape's knob reach that shape only, and are undoable.
- `Layer.getChildren()` returns typed wrappers.

**Out:**
- `KnobI::slaveTo` and the listener refresh in `Engine/Knob.cpp` (shared with the tracker).
- A Python selection API (open question).
- Stroke frame markers (M421 - Stroke Keyframe Indicators).

**Edge cases:**
- An animated item gets a key at t; a static item gets a static value; auto-key applies as for any knob.
- Removing, moving or retiming a key, or changing its interpolation or derivative, applies only to items that have a key at that time.
- An expression set on the proxy is set on each selected item.
- Knobs that are stroke-only, shape-only, clone-only, or untied transform knobs (`tiesTransformKnobs`, a layer selected) are not forwarded to items that disable them.
- With nothing selected, the proxy is just the node's own knob.
- The proxy stays marked dirty when two or more items are selected.

**Assumptions:**
- `UndoHistory` captures records made while a pushed command runs and undoes them newest first (`Gui/UndoHistory.h:114-118`).
- `KnobStateCommand` works on a holderless knob when the node is given in `UndoTouched`.

## Phase 272.1: Selection mirrors, edits forward

- [x] M272.P1.T1 — Replace item-knob slaving with a mirrored proxy that forwards edits
  - files: `Engine/RotoCurveContext.cpp/.h` (tieItemKnob, untieItemKnob, selectInternal, deselectInternal, refreshTransformKnobTies, onSelectedKnob*Changed), new `Tests/RotoSelectionProxy_Test.cpp`, `Tests/CMakeLists.txt`; tests asserting slave semantics: `Tests/Native/NativeRetouch_Test.cpp` (SourceDragOfASelectedShapeMovesTheNodeOffset), `Tests/Native/SplineWarpTransform_Test.cpp`, `Tests/RotoContentHash_Test.cpp`
  - approach: Never call slaveTo/unSlave. On select, and on deselect when items remain, call `proxy->cloneAndUpdateGui(lastSelectedItemKnob)` inside a MirrorScope guard. Connect the proxy's signals once (valueChanged, keyFrameSet/Removed/Moved, animationRemoved, keyFrameInterpolationChanged, derivativeMoved, expressionChanged). Outside a mirror, replay the operation on each selected item's own same-named knob per the Scope edge cases. Item signals re-mirror the proxy only when they come from the last-selected item; drop the fan-out in onSelectedKnobValueChanged. Update the listed tests to the new semantics: own values are kept; the node knob mirrors the last-selected item.
  - verify: gtests. Strokes with lifeTimeFrame 5 and 12 plus Beziers with different opacity keys keep every value and key through select, select-add, deselect and clearSelection in any order. The proxy shows the last-selected item. A proxy setValue or setValueAtTime, or a key removal, reaches every selected item and nothing else. A write to one selected item's knob changes only it and reads back. No item→proxy→item echo (listener refresh count unchanged).
  - size: L
- [x] M272.P1.T2 — Remove the slave-aware branches in roto hosts
  - files: `Engine/RotoDrawableItem.cpp` (:947, :1586-1597), `Engine/Nodes/Filter/Retouch.cpp` (:198, :325, :1128, :1147), `Engine/Nodes/Transform/AutoStraighten.cpp` (:1270), `Tests/Native/RetouchTestSupport.h` (:117)
  - approach: Item knobs are never slaved now: serialize own values, reset the centre without unslaving, and decide keying from the item's own animation. AutoStraighten re-mirrors the proxy through the T1 path. Drop the deselect workaround in makeShape.
  - verify: the existing Roto, Retouch, SplineWarp, AutoStraighten and RotoPaint save/load gtests pass unchanged; rg finds no isSlave/getMaster on roto item knobs.
  - size: M

## Phase 272.2: Undo

- [x] M272.P2.T1 — Make a forwarded edit one undo step
  - files: `Engine/UndoRecorder.h/.cpp`, `Gui/PythonUndoRecorder.*`, `Gui/UndoHistory.*`, `Engine/PythonUndoCommands.cpp/.h`, `Engine/RotoCurveContext.cpp`, `Tests/RotoSelectionProxy_Test.cpp`
  - approach: Add `UndoRecorderI::isReplaying()`, true while the history undoes or redoes an entry. Skip forwarding while it is true. Before forwarding, record a KnobStateCommand per item knob via a new `recordRotoItemKnobEdit(node, knob)`, with the node in UndoTouched. The pushed KnobUndoCommand or Python step absorbs these records. Check every writer of a proxy knob (drags outside a pushed command).
  - verify: gtest with a test recorder (pattern from `Tests/PythonUndo_Test.cpp`): a proxy edit across 2 selected items with different curves is one entry; undo restores each item's own curve and redo reapplies.
  - size: L
- [x] M272.P2.T2 — Record Python writes to shape knobs for undo
  - files: `Engine/PyParameter.cpp`, `Engine/PythonUndoCommands.cpp`, `Engine/PyRoto.cpp`, `Tests/PythonUndo_Test.cpp`
  - approach: Resolve a holderless item knob to its item's node (through the item wrapper or the curve context), then record it with recordRotoItemKnobEdit.
  - verify: gtest: `shape.getParam("opacity").set()` in a step undoes in one step, on a selected and an unselected shape.
  - size: M

## Phase 272.3: Python and GUI check

- [x] M272.P3.T1 — Return typed wrappers from Layer.getChildren
  - files: `Engine/PyRoto.cpp` (:197), `Tests/PyRotoCreate_Test.cpp`
  - approach: Wrap each child as getItemByName does (Layer, BezierCurve, stroke).
  - verify: gtest: the children of a layer holding a layer, a Bezier and a stroke expose their own types' methods.
  - size: M
- [x] M272.P3.T2 — GUI check for multi-select across frames and script writes
  - files: `Tests/gui/m272_roto_multiselect.py`; update the comment at `Tests/gui/m261_roto_keys_editors.py:405`
  - approach: RotoPaint with strokes on frames 5 and 12, and Roto with 2 Beziers keyed differently. Select through the RotoPanel tree (pattern in `m152_track_links_copy.py:81-122`): one, then both, then deselect, then clear. Read every item's values back each time and check the panel shows the last-selected. Edit opacity in the panel, and set the node knob from Python, while both are selected: both items change; one undo restores each. A Python write to one selected shape changes only it. Take screenshots.
  - verify: passes in the remote gate with explicit `--gui`.
  - size: M

**Verification gate:** One build for the milestone. All gtests pass: RotoSelectionProxy, PythonUndo*, PyRotoCreate, RotoContentHash, Roto*, NativeRetouch, SplineWarp*, AutoStraighten. GUI scripts pass remotely with explicit `--gui`: m272_roto_multiselect, m261_roto_keys_editors, m211_retouch, m152_track_links_copy, m205_object_select, roto_gui, plus m421_stroke_markers (its XFAIL multi-select steps must now pass). PR CI (debug asserts) is green. If the diff touches `Engine/Knob*`, the GUI bench runs and its result goes in `## Decisions`.

## Decisions
- 2026-11-14 — Claimed by bosley-native-nodes (user). Elaborated from the stub (consultant).
- 2026-11-14 — Forward operations, not state: replaying the operation per item keeps each item's other keys; copying state is what caused the loss.
- 2026-11-14 — A Python write to one shape's knob reaches that shape only: the panel (or node knob) edit stays the only way to change several items at once (user, in the stub).
- 2026-11-14 — PM defaults pending the user's answer (Open questions): no Python selection API; an expression set on the node knob with shapes selected is set on each selected shape.
- 2026-11-14 — Phase 1 implemented (b161fadda, main merged in first). Deviation: key set, key removal and animation removal are found by diffing the proxy against a per-knob snapshot on value change rather than replayed from their own signals, because `cloneCurve` itself fires animation-removed and replaying it would wipe items' animation. Known limits: a proxy linked to another knob doesn't forward; a child selected through its layer and then directly stays non-forwarded; `makeShape` leaves the new shape selected alone. Building with the roto GUI scripts; Phases 2–3 started.
- 2026-11-14 — Phases 2–3 implemented (9712c7943). AutoStraighten no longer writes selected lines itself (forwarding reaches them, so undo can restore); SplineWarp records an untied curve's own transform knobs before writing. The Dope Sheet's selected-item branch stays (tracker knobs are still slaved). Set Center / Reset Transform still edit outside an undo step, as before. Superseded Phase 1 build cancelled; full gate running (m421_stroke_markers runs at M421's re-gate on top of this).
