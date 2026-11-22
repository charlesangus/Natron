# M508 - Incremental Roto Dope Sheet Rows

Adding, removing or renaming one Roto item rebuilds the Dope Sheet rows of every item on that node. `DopeSheet::onCurveContextItemsChanged` (`Gui/DopeSheet.cpp:1338`, connected to `itemInserted`, `itemRemoved` and `itemLabelChanged` at :1275-1280) calls `DSNode::rebuildItemRows` (:2309). That function deletes every item row and creates them all again from `getCurvesByRenderOrder`, so building N shapes costs O(N²) rows and 200 shapes take minutes. Each shape key change also scans every row of every node: `findShapeDSKnob` (:676) and `findDSKnob` (:660), called per key from `onShapeKeyframesChanged` (:1361) and `onItemKnobKeyframesChanged` (:1420). Make the row changes incremental and the lookups indexed.

## Scope

**In:**
- Row insert, remove, reorder and relabel for the item that changed, in `DSNode` (`Gui/DopeSheet.cpp`, `Gui/DopeSheet.h`).
- Indexed lookups for shape rows and item-knob rows.
- Keeping `HierarchyView::onItemRowsRebuilt` (`Gui/DopeSheetHierarchyView.cpp:1047`) from re-walking every row of the node per change.
- A GUI script under Xvfb that checks the rows match and that the cost per shape stays flat.

**Out:**
- Curve editor row building, and the Roto engine's own per-insert cost (`Engine/RotoCurveContext.cpp`). If P1.T1 shows the time is spent there, file an E6 stub.
- Any change to how the rows look.

**Edge cases:**
- `itemInserted(int index, int reason)` carries no item. Sync by comparing `getCurvesByRenderOrder` with the per-item row map. Comparing 200 entries per insert is cheap; rebuilding rows is not.
- Painted Frame rows: a removed stroke unregisters only its own row (`unregisterPaintedFrameRows`). A row is never reused for a new item at a freed address; check this the way `onPaintedFrameChanged` already does.
- Reordering (raise, lower, render order) moves existing rows with `takeChild`/`insertChild`. It never recreates them.
- A label change sets the row text only.

**Assumptions:**
- Rebuilding rows is the dominant cost. P1.T1's baseline confirms it.

## Phase 508.1: Reproduce

- [ ] M508.P1.T1 — GUI script that builds 200 keyed shapes and times each add
  - files: `Tests/gui/m508_roto_dope_sheet_rows.py` (new; model on `Tests/gui/m261_roto_keys_editors.py` and `Tests/gui/guitest.py`)
  - approach: open the dope sheet with a Roto node. Add 200 Beziers from Python, each keyed at frames 1 and 10, and record the wall time of each add with the event loop processed. Check the dope sheet has one item row per shape, with a Shape child, in render order and with the right labels. Then delete 50 shapes, rename one and raise one, checking the rows after each step. Print the mean add time of the first and last 20 shapes. Assert that the last-20 mean is at most 2x the first-20 mean, which tests scaling rather than host speed. Save one screenshot of the dope sheet. The assertion is expected to fail before the fix.
  - verify: PM runs it under Xvfb (`Tests/gui/run-gui-test.sh`) on the phase build. The row checks pass, the scaling assertion fails, and the baseline numbers are recorded as a milestone decision.
  - size: M

## Phase 508.2: Incremental rows

- [ ] M508.P2.T1 — Index shape rows and item-knob rows
  - files: `Gui/DopeSheet.cpp`, `Gui/DopeSheet.h`
  - approach: add maps to `DopeSheetPrivate`, `const Bezier*` → `DSKnobWPtr` and (`const KnobI*`, dimension) → `DSKnobWPtr`. Register and unregister entries where `DSNode` inserts and erases `itemKnobMap` entries, mirroring `registerPaintedFrameRow`/`unregisterPaintedFrameRows`. `findShapeDSKnob` and `findDSKnob` look up the map first. On a hit, check the row still points at the same object, to guard against address reuse. Keep the scan only as the path for knobs of non-Roto nodes if they are not indexed.
  - verify: phase build. `Tests/gui/m261_roto_keys_editors.py`, `m124_dope_sheet_time.py` and `m421_stroke_markers.py` pass under Xvfb.
  - size: M
- [ ] M508.P2.T2 — Sync item rows instead of rebuilding them
  - files: `Gui/DopeSheet.cpp`, `Gui/DopeSheet.h`
  - approach: move the per-item body of `rebuildItemRows` (:2335-2389) into `createItemRows(item)`. Keep a map from `RotoDrawableItem*` to the item's `QTreeWidgetItem*`. Replace the rebuild with `syncItemRows()`, which removes the rows of items that are gone (their `itemKnobMap` entries, index entries and painted-frame rows), creates rows for new items, and moves rows whose order changed. The constructor's first build is a sync from empty. Give `itemLabelChanged` its own slot that only sets the text. Call `pruneExpiredKeys` only when rows were removed. Signal connections stay `Qt::UniqueConnection`.
  - verify: phase build. `Tests/gui/m508_roto_dope_sheet_rows.py` row checks pass, and so do the P2.T1 scripts.
  - size: L
- [ ] M508.P2.T3 — Re-check visibility only for rows that changed
  - files: `Gui/DopeSheet.h` (`itemRowsRebuilt` signal), `Gui/DopeSheetHierarchyView.cpp`, `Gui/DopeSheetHierarchyView.h`
  - approach: `itemRowsRebuilt` carries the item rows added or moved. `HierarchyView::onItemRowsRebuilt` runs the visibility check for those rows and their parent node only, instead of `checkKnobsVisibleState(dsNode)` over every row.
  - verify: phase build. `m508_roto_dope_sheet_rows.py` passes in full, including the scaling assertion, and the P2.T1 scripts pass.
  - size: M

## Phase 508.3: Gate (PM runs)

- [ ] M508.P3.T1 — Timing and behaviour under Xvfb
  - files: none
  - approach: run `m508_roto_dope_sheet_rows.py`, `m261_roto_keys_editors.py`, `m124_dope_sheet_time.py`, `m125_dope_sheet_append_clip.py` and `m421_stroke_markers.py` under Xvfb on the `fast` build. Record the before and after add times as a decision.
  - verify: all scripts pass, and the 200-shape build time is recorded next to the P1.T1 baseline
  - size: S

**Verification gate:** P3.T1 green under Xvfb (ctest alone does not prove a GUI refresh fix); the dope sheet screenshot is shared with the user; full debug PR CI green.
