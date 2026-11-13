# M421 - Stroke Keyframe Indicators

User request (2026-11-13, from F10 - Viewer Polish UAT): RotoPaint strokes show no keyframe indicators. Strokes have no shape keys. Their frame is the `lifeTimeFrame` item knob (`Engine/RotoContextPrivate.h` ~:1015, read by `RotoDrawableItem::isActivated`, `Engine/RotoDrawableItem.cpp` ~:1091-1117), which painting sets to the current frame (`Engine/RotoPaintInteract.cpp` ~:506, and ~:1094 on a multi-stroke continuation); a frame change breaks the multi-stroke (~:282-289), so each stroke has one painted frame. The timeline's roto markers come only from `Gui/RotoPanel.cpp` following Bezier signals (`trackBezierKeyframes` ~:2496-2511, slots ~:902-944, `onItemRemoved` ~:1377-1395, `onSettingsPanelClosed` ~:2587-2657), pushing incremental counts into `GuiAppInstance::timelineKeyframes`. The dope sheet adds a Shape row only for Beziers (`Gui/DopeSheet.cpp` ~:2177-2188). This milestone gives every current or selected stroke a timeline marker at its painted frame, and each stroke a display-only "Painted Frame" row in the dope sheet. Not a regression: upstream 2.6 never showed stroke frames.

## Scope

**In:**
- A timeline marker at `lifeTimeFrame` for each selected stroke and for the current stroke (the one this RotoPaint last painted), while the node's panel is open. Next and previous keyframe navigation includes these frames.
- RotoPanel's roto markers become a derived set per panel instead of incremental counts, for Beziers and strokes alike.
- Dope sheet: a "Painted Frame" row under each stroke's item row with one key at `lifeTimeFrame`; selectable, not movable, deletable or interpolable.
- A GUI test script.

**Out:**
- Moving the painted frame from the dope sheet or timeline (the Lifetime "Frame" knob does that).
- Markers for item-knob keys beyond what the proxy knobs already show.
- Tracker markers and user markers.

**Edge cases:**
- Multiple selected strokes: one marker per distinct frame; two strokes on one frame keep it until both are deselected or deleted.
- Stroke deleted: its marker goes; undoing the delete brings it back.
- Panel closed: no stroke or Bezier markers; reopening restores the selected strokes' markers.
- Painted frame changed through the Lifetime "Frame" knob or the Lifetime mode, on the item or the context proxy: the marker follows at once.
- Undo of a stroke (`AddStrokeUndoCommand`): the marker goes on undo and returns on redo.
- RotoPaint inside a Group behaves the same.
- Beziers unaffected: shape-key markers and Shape rows unchanged.

**Assumptions:**
- "Painted frame" is `lifeTimeFrame` in every Lifetime mode, including All and Custom where the knob is hidden. Confirm at UAT.
- "Current stroke" is the stroke most recently passed to `setUserIsPainting` for this node, current until another stroke is painted or the panel closes. Confirm at UAT.

## Phase 421.1: Derived roto markers

- [ ] M421.P1.T1 — Roto markers as a derived set per panel
  - files: `Engine/TimeLineKeyFrames.h`, `Gui/GuiAppInstance.{h,cpp}`, `Gui/RotoPanel.{h,cpp}`
  - approach: add `setPanelKeyframeIndicators(const void* owner, std::vector<SequenceTime>)` and `clearPanelKeyframeIndicators(owner)`, merged by `sortedKeysWithExtras`. RotoPanel recomputes the union of selected Beziers' keys on a coalesced queued call and publishes it, clearing it on panel close and in its destructor, replacing the `setItemKey`/`removeItemKey`/`setVisibleItemKeyframes` counting. This also fixes `onSettingsPanelClosed`'s `std::set<std::set<double>>` dedup, which drops one of two identical key sets and leaves a ghost marker.
  - verify: syntax-check; `rg "KeyframeIndicator" Gui/RotoPanel.cpp` shows only the new calls; existing `m261_roto_keys_editors.py` and `m305_keyframe_markers.py` still pass at the gate.
  - size: M

- [ ] M421.P1.T2 — Stroke painted frames in the set
  - files: `Gui/RotoPanel.{h,cpp}`
  - approach: on insert, rebuild and panel open, follow each `RotoStrokeItem`'s `lifeTimeFrame` and `lifeTime` knobs (valueChanged) and the context's proxies (`onLifeTimeKnobValueChanged`, `RotoCurveContext.cpp` ~:726); follow `setUserIsPainting` for the current stroke. The union adds `getLifeTimeFrameKnob()->getValue()` for each selected or current stroke. Removal and undo go through `onItemRemoved`/`onItemInserted`.
  - verify: syntax-check; covered by P2.T2.
  - size: M

- [ ] M421.P1.T3 — Item-knob key edits refresh the markers (check first)
  - files: `Engine/RotoCurveContext.cpp` (only if the check fails)
  - approach: item knobs have a null holder, so `invalidateMarkersOfEffect` (`Engine/Knob.cpp` ~:1189-1199) returns early for them; markers rely on `tieItemKnob`'s echo into the proxy. P2.T2 keys an item opacity from Python with two strokes and a Bezier selected, and with a stroke selected through its layer. If markers go stale, call `getApp()->invalidateNodeKeyframeIndicators()` from `onSelectedKnobCurveChanged`/`onSelectedKnobValueChanged`.
  - verify: P2.T2's item-knob section passes.
  - size: M

## Phase 421.2: Dope sheet and test

- [ ] M421.P2.T1 — Painted Frame row
  - files: `Gui/DopeSheet.{h,cpp}`, `Gui/DopeSheetView.cpp`, `Gui/DopeSheetEditorUndoRedo.cpp`, `Global/Enums.h`
  - approach: add `eDopeSheetItemTypePaintedFrame`, a `DSKnob` backed by a `RotoStrokeItemPtr` whose `getKeyFrames()` is `{lifeTimeFrame}`, refreshed on that knob's valueChanged. The stroke's item row stays visible with no animated knobs. Move, delete, paste and interpolation skip these keys.
  - verify: syntax-check; the row draws at the painted frame and refuses a drag (P2.T2).
  - size: M

- [ ] M421.P2.T2 — GUI test
  - files: `Tests/gui/m421_stroke_markers.py` (conventions of `m305_keyframe_markers.py`; paint with `m224_brush_strokes.py`'s mouse helpers)
  - approach: a RotoPaint with strokes painted on frames 5, 5 and 12, a Bezier keyed at 1 and 20, and a second RotoPaint inside a Group. After each step compare `TimeLineGui::keyframeMarkerTimes()` with the expected set: current stroke; select one, then several; panel close and reopen; Frame knob edit; Lifetime mode change; delete and undo; stroke undo and redo; Beziers unchanged; the P1.T3 item-knob case. Check the dope sheet's Painted Frame rows. Save screenshots of the timeline and dope sheet.
  - verify: passes with explicit `--gui` and fails on main.
  - size: M

**Verification gate:** release gate green; `Tests/gui/m421_stroke_markers.py`, `Tests/gui/m305_keyframe_markers.py` and `Tests/gui/m261_roto_keys_editors.py` pass via explicit `--gui`; screenshots shared; UAT AppImage with a same-basename steps `.md`, approved by the user.

## Decisions

- 2026-11-13 — **Stroke indicators (user):** show "the actual frame that was painted for current/selected strokes": each current or selected stroke gets one indicator at the frame it was painted on, on the timeline and in the dope sheet. Filed as a follow-up and picked up at once (user: not a regression, so not in F10).
- 2026-11-13 — **Dope sheet (PM):** a display-only Painted Frame row. Moving it would mean writing `lifeTimeFrame` with undo across Lifetime modes, so it stays out unless the user asks.
