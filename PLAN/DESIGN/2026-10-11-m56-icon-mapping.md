# M56 icon mapping (P5.T1)

Date: 2026-10-11. Task: M56.P5.T1. Status: **pending user approval**. Once approved, the mapping goes into M56's `## Decisions`, and P5.T2-P5.T4 apply exactly this.

Contact sheet: `m56-icon-mapping.html`, built in the session scratchpad (`/tmp/claude-1000/-home-bosley-git-Natron/3b932fcc-55b5-46af-8a6f-5089cc13eb8c/scratchpad/m56-icon-mapping.html`) for the PM to publish. It shows today's PNG beside each proposed glyph at 18 px and 36 px on `#303030`, in the Normal (`#c8c8c8`), On (selection colour `#f28a00`, `Engine/Settings.cpp:1646-1648`) and Disabled (`#6d6d6d`, text blended 40% into the base) states.

Source: Lucide 1.55.0, commit `70562c1ee1c4fdcf736fe97bc893fb8511927934`. The ids and files come from `pixmapEnumToPath` (`Gui/GuiApplicationManager.cpp:93-801`) on `.worktrees/m56` at `ab21ebfc1`. The path strings come from `Engine/RotoPaint.cpp`, `Engine/TrackerNode.cpp`, `Engine/TrackerContextPrivate.cpp`, `Gui/MultiInstancePanel.cpp` and `Gui/DopeSheetView.cpp`. Every Lucide name below exists at that commit (checked against the git tree).

Scope: icon scope "Everything" (Q5 = c), so the areas are the viewer toolbar, the timeline, properties-panel chrome, roto, tracker, and the curve editor and dope sheet, plus candidates for the Misc and Other toolbox groupings. The native-node and merge-operator icons are out of scope.

## Pair legend

- **Collapses**: the `*_ENABLED`/`*_DISABLED`, `*_ON`/`*_OFF` or `on`/`off` path pair becomes one glyph through `ThemedIcon::icon`. On draws in the selection colour, Off in the text colour, and Disabled is dimmed. One SVG file serves both ids.
- **Two glyphs**: the two states differ in shape (for example play → stop, or lock → lock-open), so they keep separate glyphs.
- **Custom needed**: no adequate Lucide glyph. Draw one in Lucide's style (24-unit grid, `currentColor`, stroke 2 that thins to 1.5 at load, round caps and joins), under GPL-2.0-or-later, in `Gui/Resources/Icons/natron/`.

## Counts

| Area | Rows | Lucide | Custom | Keep PNG | Collapsing pairs |
|---|---:|---:|---:|---:|---:|
| Viewer toolbar | 14 | 12 | 2 | 0 | 10 |
| Timeline and transport | 17 | 12 | 5 | 0 | 1 |
| Properties panel | 17 | 17 | 0 | 0 | 5 |
| Roto and RotoPaint | 39 | 30 | 9 | 0 | 10 |
| Tracker | 27 | 19 | 8 | 0 | 5 |
| Curve editor and dope sheet | 8 | 0 | 7 | 1 | 0 |
| **All** | 122 | 90 | 31 | 1 | 31 |

That is 81 distinct Lucide glyphs to vendor and **28 distinct custom glyphs** to draw. Several rows share a custom glyph: play-reverse is used 3 times and key-prev 2 times.

## Viewer toolbar

The two rows above the viewport, built in ViewerTab.cpp:223-560 at 18 px (NATRON_MEDIUM_BUTTON_ICON_SIZE).

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_LOCKED / _UNLOCKED | `locked.png`, `unlocked.png` | `lock` + `lock-open` | **Two glyphs.** Sync-viewers toggle. Also the timeline triple-sync button, the roto panel lock column and NodeGraphPrivate. Keeps two shapes: open vs closed shackle reads better than colour alone. |
| NATRON_PIXMAP_MERGE_GROUPING | (already SVG) | `layers-2` | Compositing-operator label. Already an SVG since P4.T5; listed for completeness, no change. |
| NATRON_PIXMAP_VIEWER_GAIN_ENABLED / _DISABLED | `expoON.png`, `expoOFF.png` | `aperture` | **Collapses.** Gain (f-stop) slider toggle. Today's art is an "expo" ± box. Aperture ties gain to f-stops. |
| NATRON_PIXMAP_VIEWER_GAMMA_ENABLED / _DISABLED | `gammaON.png`, `gammaOFF.png` | **custom needed**: `gamma`: Greek letter gamma (γ) drawn as a single open stroke. | **Collapses.** Gamma slider toggle. Lucide has no gamma; a letterform matches today's "Y"-shaped glyph. |
| NATRON_PIXMAP_VIEWER_AUTOCONTRAST_ENABLED / _DISABLED | `AutoContrastON.png`, `AutoContrast.png` | `contrast` | **Collapses.** Auto-contrast toggle. Today On adds a red ring. |
| NATRON_PIXMAP_VIEWER_CHECKERBOARD_ENABLED / _DISABLED | `checkerboard_on.png`, `checkerboard_off.png` | **custom needed**: `checkerboard`: Rounded square split into a 2×2 grid, two diagonal cells filled with currentColor. | **Collapses.** Checkerboard toggle. Fallback if the custom is rejected: Lucide `grid-2x2` (outline only, reads as a grid rather than a checkerboard). |
| NATRON_PIXMAP_COLOR_PICKER | `color_picker.png` | `pipette` | Viewer colour picker; also KnobGuiColor's picker. Same glyph as the Keyer toolbox group; the contexts differ. |
| NATRON_PIXMAP_VIEWER_CLIP_TO_PROJECT_ENABLED / _DISABLED | `cliptoprojectEnabled.png`, `cliptoprojectDisabled.png` | `crop` | **Collapses.** Clip to project format. |
| NATRON_PIXMAP_VIEWER_FULL_FRAME_ON / _OFF | `fullFrameOn.png`, `fullFrameOff.png` | `fullscreen` | **Collapses.** Full-frame processing. Today's gear art doesn't say "whole frame". |
| NATRON_PIXMAP_VIEWER_ROI_ENABLED / _DISABLED | `viewer_roiEnabled.png`, `viewer_roiDisabled.png` | `square-dashed` | **Collapses.** Region of interest. RotoPaint's bbox-click-anywhere toggle reuses these PNGs by path; it gets its own glyph in the roto table. |
| NATRON_PIXMAP_VIEWER_RENDER_SCALE / _CHECKED | `renderScale.png`, `renderScale_checked.png` | `scaling` | **Collapses.** Proxy (render scale) toggle. |
| NATRON_PIXMAP_PLAYER_PAUSE_ENABLED / _DISABLED | `pauseEnabled.png`, `pauseDisabled.png` | `pause` | **Collapses.** Pause viewer updates. Also ProgressTaskInfo (render queue) and TrackerNode's stop-tracking button (path `pauseDisabled.png`). |
| NATRON_PIXMAP_VIEWER_REFRESH / _REFRESH_ACTIVE | `refresh.png`, `refreshActive.png` | `refresh-cw` | **Collapses.** Force render. Not a checkable toggle: ViewerTab40.cpp:763 swaps the icon while rendering, so P5.T2 builds iconRefreshOn from a selection-colour renderPixmap. Also KnobGuiFile's reload button and the tracker's update-viewer toggle. |
| NATRON_PIXMAP_VIEWER_CENTER | `centerViewer.png` | `focus` | Fit/centre the viewer. Also the properties-panel "centre node graph on this node" button. |

## Timeline and transport

The player row under the viewer (ViewerTab.cpp:750-932, ViewerTab10.cpp:150-200), drawn at 18 px.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_PLAYER_FIRST_FRAME | `firstFrame.png` | `skip-back` | Go to first frame. |
| NATRON_PIXMAP_PLAYER_PREVIOUS_KEY | `prevKF.png` | **custom needed**: `key-prev`: Left chevron beside a small outline diamond (step back to the previous keyframe). | Previous keyframe. Also RotoPanel; the tracker's PREV_USER_KEY reuses this glyph. |
| NATRON_PIXMAP_PLAYER_PREVIOUS_INCR | `previousIncr.png` | `rewind` | Back by the frame increment. |
| NATRON_PIXMAP_PLAYER_PREVIOUS | `back1.png` | `step-back` | Previous frame. Also MultiInstancePanel's path `back1.png`. |
| NATRON_PIXMAP_PLAYER_REWIND_DISABLED | `rewind.png` | **custom needed**: `play-reverse`: Lucide `play` mirrored horizontally (triangle pointing left). | **Two glyphs.** Play backward (Off state). Its On state is STOP_ENABLED, a different shape, so this pair keeps two glyphs. Also MultiInstancePanel's path `rewind.png`. |
| NATRON_PIXMAP_PLAYER_STOP_ENABLED / _STOP_DISABLED | `stopEnabled.png`, `stopDisabled.png` | `square` | Stop: the On state of both play buttons. STOP_DISABLED is used by ProgressTaskInfo. Both ids point at one glyph. |
| NATRON_PIXMAP_PLAYER_PLAY_DISABLED / _PLAY_ENABLED | `play.png`, `play_enabled.png` | `play` | **Two glyphs.** Play forward (Off state; On is `square`). PLAY_ENABLED is used only by SequenceFileDialog; both ids point at one glyph. Also ProgressTaskInfo and MultiInstancePanel's `play.png`. |
| NATRON_PIXMAP_PLAYER_NEXT | `forward1.png` | `step-forward` | Next frame. Also MultiInstancePanel's `forward1.png`. |
| NATRON_PIXMAP_PLAYER_NEXT_INCR | `nextIncr.png` | `fast-forward` | Forward by the frame increment. |
| NATRON_PIXMAP_PLAYER_NEXT_KEY | `nextKF.png` | **custom needed**: `key-next`: Small outline diamond beside a right chevron (step on to the next keyframe). | Next keyframe. Also RotoPanel; the tracker's NEXT_USER_KEY reuses this glyph. |
| NATRON_PIXMAP_PLAYER_LAST_FRAME | `lastFrame.png` | `skip-forward` | Go to last frame. |
| NATRON_PIXMAP_PLAYER_LOOP_MODE | `loopmode.png` | `repeat` | Playback mode: loop (the button cycles three ids). |
| NATRON_PIXMAP_PLAYER_BOUNCE | `bounce.png` | `arrow-left-right` | Playback mode: bounce. |
| NATRON_PIXMAP_PLAYER_PLAY_ONCE | `playOnce.png` | `arrow-right-to-line` | Playback mode: play once and stop. |
| NATRON_PIXMAP_PLAYER_TIMELINE_IN | `timelineIn.png` | **custom needed**: `in-point`: Left square bracket `[` with a short arrow pointing into it (set the playback in point). | Set the playback in point. |
| NATRON_PIXMAP_PLAYER_TIMELINE_OUT | `timelineOut.png` | **custom needed**: `out-point`: Right square bracket `]` with a short arrow pointing into it (set the playback out point). | Set the playback out point. |
| NATRON_PIXMAP_FREEZE_ENABLED / _DISABLED | `turbo_on.png`, `turbo_off.png` | `zap` | **Collapses.** Turbo mode (redraw only the viewer during playback). |

## Properties panel

The node panel header (DockablePanel.cpp:200-370, NodeSettingsPanel.cpp:90) and the icons inside knob rows, at 18 px; group-box arrows at 11 px.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_SETTINGS | `settings.png` | `settings` | Settings and presets (NodeSettingsPanel). |
| NATRON_PIXMAP_VIEWER_CENTER | `centerViewer.png` | `focus` | Centre the node graph on this node. Shared id; see the viewer table. |
| NATRON_PIXMAP_ENTER_GROUP | `enter_group.png` | `square-arrow-right-enter` | Show the group's internal node graph. |
| NATRON_PIXMAP_HELP_WIDGET | `help.png` | `circle-question-mark` | Node help. |
| NATRON_PIXMAP_HIDE_UNMODIFIED / _UNHIDE_UNMODIFIED | `hide_unmodified.png`, `unhide_unmodified.png` | `list-filter` | **Collapses.** Show only modified parameters. On = filtered (HIDE), Off = all shown (UNHIDE). |
| NATRON_PIXMAP_OVERLAY | `colorwheel_overlay.png` | `paint-bucket` | Overlay colour button. Also RotoPanel's default-overlay column. Check during P5.T3 whether OverlayColorButton paints the chosen colour over the icon. |
| NATRON_PIXMAP_UNDO / _UNDO_GRAYSCALE | `undo.png`, `undo_grayscale.png` | `undo-2` | **Collapses.** Undo. The grayscale PNG is the Disabled state, which ThemedIcon::icon's dimmed Disabled replaces. |
| NATRON_PIXMAP_REDO / _REDO_GRAYSCALE | `redo.png`, `redo_grayscale.png` | `redo-2` | **Collapses.** Redo. As for undo. |
| NATRON_PIXMAP_RESTORE_DEFAULTS_ENABLED | `restoreDefaultEnabled.png` | `rotate-ccw` | Restore defaults. _DISABLED has no caller. Also TrackerNode's reset-track button (path). |
| NATRON_PIXMAP_MINIMIZE_WIDGET | `minimize.png` | `minus` | Minimise the panel. Also GuiPrivate. |
| NATRON_PIXMAP_MAXIMIZE_WIDGET | `maximize.png` | `square-arrow-out-up-right` | Float the panel. Also TabWidget's float-pane button and GuiPrivate, which mean the same thing. |
| NATRON_PIXMAP_CLOSE_WIDGET | `close.png` | `x` | Close the panel. Also TabWidget. |
| NATRON_PIXMAP_OPEN_FILE | `open-file.png` | `folder-open` | File knob browse button. Also CurveWidgetDialogs and ExportGroupTemplateDialog. |
| NATRON_PIXMAP_COLORWHEEL | `colorwheel.png` | `swatch-book` | Colour knob's colour-dialog button; also RotoPanel. Avoids `palette`, which is the Color toolbox group. |
| NATRON_PIXMAP_GROUPBOX_FOLDED / _UNFOLDED | `groupbox_folded.png`, `groupbox_unfolded.png` | `chevron-right` + `chevron-down` | **Two glyphs.** Group-box arrows (GroupBoxLabel, 11 px). mainstyle.qss:73-78 and :185-190 also load these PNGs, so the PNG files stay for the stylesheet. |
| NATRON_PIXMAP_BOLD_CHECKED / _UNCHECKED | `bold_checked.png`, `bold_unchecked.png` | `bold` | **Collapses.** Rich-text string knob. |
| NATRON_PIXMAP_ITALIC_CHECKED / _UNCHECKED | `italic_checked.png`, `italic_unchecked.png` | `italic` | **Collapses.** Rich-text string knob. |

## Roto and RotoPaint

Viewer toolbar buttons from path strings in Engine/RotoPaint.cpp (loaded by KnobGuiButton.cpp:99), plus the ids RotoPanel.cpp:543-645 uses in the layers tree.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| RotoPaint.cpp:654 `cursor.png` (Select all) | `cursor.png` | `mouse-pointer-2` | Select tool. The id NATRON_PIXMAP_SELECT_ALL shares this file today; see the tracker table. |
| RotoPaint.cpp:667 `selectPoints.png` | `selectPoints.png` | **custom needed**: `select-points`: `mouse-pointer-2` arrow with a small solid square control point at its tip. | Select points. |
| RotoPaint.cpp:680 `selectCurves.png` | `selectCurves.png` | **custom needed**: `select-curves`: `mouse-pointer-2` arrow beside a short Bézier arc. | Select shapes. |
| RotoPaint.cpp:693 `selectFeather.png` | `selectFeather.png` | **custom needed**: `select-feather`: `mouse-pointer-2` arrow with a small dashed-outline square point (feather point). | Select feather points. |
| RotoPaint.cpp:706 `addPoints.png` | `addPoints.png` | **custom needed**: `add-points`: `pen-tool` nib with a small plus at the top right. | Add points. |
| RotoPaint.cpp:719 `removePoints.png` | `removePoints.png` | **custom needed**: `remove-points`: `pen-tool` nib with a small minus at the top right. | Remove points. |
| RotoPaint.cpp:732 `cuspPoints.png` | `cuspPoints.png` | **custom needed**: `cusp-point`: Two straight segments meeting at a sharp V around a solid square point. | Cusp points. |
| RotoPaint.cpp:745 `smoothPoints.png` | `smoothPoints.png` | `tangent` | Smooth points: Lucide's tangent (a line touching a circle) is the smooth-handle idea. |
| RotoPaint.cpp:758 `openCloseCurve.png` | `openCloseCurve.png` | **custom needed**: `open-close-curve`: Bézier arc whose two ends nearly meet, with a small gap marker between them. | Open/close curve. |
| RotoPaint.cpp:772 `removeFeather.png` | `removeFeather.png` | **custom needed**: `remove-feather`: `pen-tool` nib with a small dashed square crossed out (drop the feather). | Remove feather. |
| RotoPaint.cpp:786 `bezier32.png` (Bezier) | `bezier32.png` | `pen-tool` | Draw Bézier. |
| RotoPaint.cpp:800 `ellipse.png` | `ellipse.png` | `circle` | Draw ellipse. |
| RotoPaint.cpp:814 `rectangle.png` | `rectangle.png` | `rectangle-horizontal` | Draw rectangle. |
| RotoPaint.cpp:827 `rotopaint_solid.png` + NATRON_PIXMAP_ROTOPAINT_SOLID | `rotopaint_solid.png` | `paintbrush` | Brush tool and its tree icon. `brush` is already the Paint group and the RotoPaint plugin icon, so the tool takes `paintbrush`. |
| RotoPaint.cpp:840 `rotoToolPencil.png` (Pencil) | `rotoToolPencil.png` | `pencil` | Open-Bézier pencil tool. |
| RotoPaint.cpp:854 `rotopaint_eraser.png` + ROTOPAINT_ERASER | `rotopaint_eraser.png` | `eraser` | Eraser. |
| RotoPaint.cpp:868 `rotopaint_clone.png` + ROTOPAINT_CLONE | `rotopaint_clone.png` | `stamp` | Clone (the stamp convention). |
| RotoPaint.cpp:881 `rotopaint_reveal.png` + ROTOPAINT_REVEAL | `rotopaint_reveal.png` | `wand-sparkles` | Reveal. Keeps today's magic-hat idea; `image-up` is the literal alternative. |
| RotoPaint.cpp:896 `rotopaint_blur.png` + ROTOPAINT_BLUR | `rotopaint_blur.png` | `droplet` | Blur (the drop convention, like today's art). |
| RotoPaint.cpp:909 `rotopaint_smear.png` + ROTOPAINT_SMEAR | `rotopaint_smear.png` | `pointer` | Smear (finger, the smudge convention). |
| NATRON_PIXMAP_ROTOPAINT_SHARPEN | `rotopaint_sharpen.png` | `triangle` | Sharpen, RotoPanel tree only (RotoPaint.cpp has no sharpen tool button). The triangle convention. |
| RotoPaint.cpp:922 `rotopaint_dodge.png` + ROTOPAINT_DODGE | `rotopaint_dodge.png` | `sun` | Dodge (lighten). |
| RotoPaint.cpp:935 `rotopaint_burn.png` + ROTOPAINT_BURN | `rotopaint_burn.png` | `flame` | Burn (darken). |
| RotoPaint.cpp:209 `autoKeyingEnabled/Disabled.png` | `autoKeyingEnabled.png`, `autoKeyingDisabled.png` | `timer` | **Collapses.** Auto-keying (After Effects' stopwatch convention). Avoids a key glyph, per the Keyer decision. |
| RotoPaint.cpp:222 `featherLinkEnabled/Disabled.png` | `featherLinkEnabled.png`, `featherLinkDisabled.png` | `link` | **Collapses.** Feather link. |
| RotoPaint.cpp:236 `featherEnabled/Disabled.png` | `featherEnabled.png`, `featherDisabled.png` | `feather` | **Collapses.** Display feather. |
| RotoPaint.cpp:249 `stickySelectionEnabled/Disabled.png` | `stickySelectionEnabled.png`, `stickySelectionDisabled.png` | `pin` | **Collapses.** Sticky selection. |
| RotoPaint.cpp:262 `viewer_roiEnabled/Disabled.png` (bbox click anywhere) | `viewer_roiEnabled.png`, `viewer_roiDisabled.png` | `square-mouse-pointer` | **Collapses.** Click anywhere in the bounding box to select. Today it borrows the viewer RoI art; its own glyph separates the two. |
| RotoPaint.cpp:275 `rippleEditEnabled/Disabled.png` | `rippleEditEnabled.png`, `rippleEditDisabled.png` | `waves-horizontal` | **Collapses.** Ripple edit. |
| RotoPaint.cpp:310 `rotoShow/HideTransformOverlay.png` | `rotoShowTransformOverlay.png`, `rotoHideTransformOverlay.png` | `move` | **Collapses.** Show the transform overlay. `move` is also the Transform toolbox group; the context differs. |
| RotoPaint.cpp:374/400/426 `rotopaint_pressure_on/off.png` | `rotopaint_pressure_on.png`, `rotopaint_pressure_off.png` | `gauge` | **Collapses.** Pressure alters opacity / size / hardness (three buttons, one pair of files). The weakest Lucide match here; a custom stylus could replace it. |
| RotoPaint.cpp:440 `rotopaint_buildup_on/off.png` | `rotopaint_buildup_on.png`, `rotopaint_buildup_off.png` | `layers-plus` | **Collapses.** Build-up. |
| RotoPaint.cpp:453 `visible/unvisible.png` + NATRON_PIXMAP_VISIBLE / _UNVISIBLE | `visible.png`, `unvisible.png` | `eye` + `eye-off` | **Two glyphs.** Auto-connect viewer toggle, and the RotoPanel visibility column. The tree shows each state as a separate image, so keep two shapes. |
| RotoPaint.cpp:286/296 `addKF/removeKF.png` + NATRON_PIXMAP_ADD_KEYFRAME / _REMOVE_KEYFRAME | `addKF.png`, `removeKF.png` | `diamond-plus` + `diamond-minus` | Add / remove keyframe (toolbar and RotoPanel). The tracker's add/remove user key use the same pair. |
| NATRON_PIXMAP_LAYER | `layer.png` | `layers` | RotoPanel tree: layer item. |
| NATRON_PIXMAP_BEZIER | `bezier.png` | `spline` | RotoPanel tree: Bézier item. |
| NATRON_PIXMAP_INVERTED / _UNINVERTED | `inverted.png`, `uninverted.png` | **custom needed**: `inverted`: Rounded square with a circle cut out, the area outside the circle hatched (inverted mask). | **Collapses.** RotoPanel inverted column. Tree cells, not a checkable button, so the cell uses renderPixmap in the selection colour for Inverted and the text colour for Uninverted. |
| NATRON_PIXMAP_ROTO_MERGE | `roto_merge.png` | `layers-2` | RotoPanel compositing-operator column header; matches the viewer's operator label. |
| NATRON_PIXMAP_CLEAR_ALL_ANIMATION | `clearAnimation.png` | `trash` | Clear all animation (RotoPanel, TrackerPanel, TrackerNode path). ScriptEditor also uses this id for "clear history". |

## Tracker

Path strings in Engine/TrackerNode.cpp:142-356 and TrackerContextPrivate.cpp:269-284, plus TrackerPanel.cpp ids and MultiInstancePanel.cpp's legacy path strings.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| TrackerNode.cpp:142 `addTrack.png` | `addTrack.png` | `crosshair` | Add a track. The id NATRON_PIXMAP_ADD_TRACK has no caller. |
| TrackerNode.cpp:155 `trackBackwardOn/Off.png` | `trackBackwardOn.png`, `trackBackwardOff.png` | **custom needed**: `play-reverse`: Lucide `play` mirrored horizontally (triangle pointing left). | **Collapses.** Track backward. Shares the viewer's custom play-reverse glyph. |
| TrackerNode.cpp:166 `trackPrev.png` | `trackPrev.png` | `step-back` | Track one frame back. |
| TrackerNode.cpp:176 `pauseDisabled.png` (stop tracking) | `pauseDisabled.png` | `pause` | Stop tracking. Shares the viewer's `pause`. |
| TrackerNode.cpp:186 `trackNext.png` | `trackNext.png` | `step-forward` | Track one frame forward. |
| TrackerNode.cpp:198 `trackForwardOn/Off.png` | `trackForwardOn.png`, `trackForwardOff.png` | `play` | **Collapses.** Track forward. |
| TrackerNode.cpp:209 `trackRange.png` | `trackRange.png` | `brackets` | Track a range. Keeps today's [ ] motif. |
| TrackerNode.cpp:219 `trackAllKeyframes.png` | `trackAllKeyframes.png` | **custom needed**: `track-all-keys`: Two outline diamonds joined by a double-headed horizontal arrow. | Track between all user keyframes. |
| TrackerNode.cpp:230 `trackCurrentKeyframe.png` | `trackCurrentKeyframe.png` | **custom needed**: `track-current-key`: One outline diamond with short arrows leaving it to the left and right. | Track around the current keyframe. |
| TrackerNode.cpp:241/251 `addUserKey/removeUserKey.png` + NATRON_PIXMAP_ADD_USER_KEY / _REMOVE_USER_KEY | `addUserKey.png`, `removeUserKey.png` | `diamond-plus` + `diamond-minus` | Add / remove user keyframe; same pair as roto's add/remove keyframe. |
| NATRON_PIXMAP_PREV_USER_KEY / _NEXT_USER_KEY | `prevUserKey.png`, `nextUserKey.png` | **custom needed**: `key-prev`: Left chevron beside a small outline diamond (step back to the previous keyframe). | TrackerPanel previous / next user key. Reuse the timeline's custom key-prev and key-next. |
| TrackerNode.cpp:261 `clearAnimation.png` | `clearAnimation.png` | `trash` | Clear all animation. Shares CLEAR_ALL_ANIMATION's glyph. |
| TrackerNode.cpp:271 `clearAnimationBw.png` | `clearAnimationBw.png` | **custom needed**: `clear-anim-bw`: Row of three small diamonds, the left two crossed out, with a left arrow under them. | Clear animation before the current frame. The id CLEAR_BACKWARD_ANIMATION has no caller. |
| TrackerNode.cpp:281 `clearAnimationFw.png` | `clearAnimationFw.png` | **custom needed**: `clear-anim-fw`: Row of three small diamonds, the right two crossed out, with a right arrow under them. | Clear animation after the current frame. |
| TrackerNode.cpp:293 `refreshActive/refresh.png` (update viewer) | `refreshActive.png`, `refresh.png` | `refresh-cw` | **Collapses.** Update the viewer while tracking. Shares the viewer's refresh glyph. |
| TrackerNode.cpp:306 `centerOnTrack.png` | `centerOnTrack.png` | `locate-fixed` | Centre the viewer on the track. |
| TrackerNode.cpp:318 `createKeyOnMoveOn/Off.png` | `createKeyOnMoveOn.png`, `createKeyOnMoveOff.png` | **custom needed**: `key-on-move`: Outline diamond at the centre of `move`'s four arrows. | **Collapses.** Create a keyframe when a marker moves. |
| TrackerNode.cpp:332 `showTrackError/hideTrackError.png` | `showTrackError.png`, `hideTrackError.png` | `triangle-alert` | **Collapses.** Show the track error. |
| TrackerNode.cpp:345 `resetTrackOffset.png` | `resetTrackOffset.png` | `undo-dot` | Reset the track offset. |
| TrackerNode.cpp:356 `restoreDefaultEnabled.png` (reset track) | `restoreDefaultEnabled.png` | `rotate-ccw` | Reset the track; shares the panel's restore-defaults glyph. |
| TrackerContextPrivate.cpp:269 `patternSize.png` | `patternSize.png` | `scan` | Default pattern window size. |
| TrackerContextPrivate.cpp:284 `searchSize.png` | `searchSize.png` | `scan-search` | Default search window size. |
| NATRON_PIXMAP_SELECT_ALL | `cursor.png` | `list-checks` | TrackerPanel and MultiInstancePanel "select all tracks" buttons. Today this id shares roto's cursor.png; it gets its own glyph, since it selects rows, not a viewer tool. |
| MultiInstancePanel.cpp:1889 `back1.png` | `back1.png` | `step-back` | Legacy multi-instance tracker: previous frame. Same glyph as the timeline. |
| MultiInstancePanel.cpp:1891 `forward1.png` | `forward1.png` | `step-forward` | Legacy multi-instance tracker: next frame. |
| MultiInstancePanel.cpp:1893 `rewind.png` | `rewind.png` | **custom needed**: `play-reverse`: Lucide `play` mirrored horizontally (triangle pointing left). | Legacy multi-instance tracker: track backward. |
| MultiInstancePanel.cpp:1895 `play.png` | `play.png` | `play` | Legacy multi-instance tracker: track forward. |

## Curve editor and dope sheet

The interpolation menu (DopeSheetView.cpp:2433-2513, 18 px). The curve editor itself has no icons; its interpolation menu is text-only.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_INTERP_CONSTANT | `interp_constant.png` | **custom needed**: `interp-constant`: Step line (flat, vertical jump, flat) through a small diamond key. | Constant. |
| NATRON_PIXMAP_INTERP_LINEAR | `interp_linear.png` | **custom needed**: `interp-linear`: Two straight segments meeting at a small diamond key. | Linear. |
| NATRON_PIXMAP_INTERP_CURVE_Z | `interp_curve_z.png` | **custom needed**: `interp-smooth`: Gentle S-curve through a small diamond key (smooth, "Z"). | Smooth. |
| NATRON_PIXMAP_INTERP_CURVE_R | `interp_curve_r.png` | **custom needed**: `interp-catmull`: Curve through a small diamond key with a slight overshoot either side (Catmull-Rom, "R"). | Catmull-Rom. |
| NATRON_PIXMAP_INTERP_CURVE_C | `interp_curve_c.png` | **custom needed**: `interp-cubic`: Cubic curve through a small diamond key with tangent handles drawn ("C"). | Cubic. |
| NATRON_PIXMAP_INTERP_CURVE_H | `interp_curve_h.png` | **custom needed**: `interp-horizontal`: Curve flattening into a horizontal tangent handle at the key ("H"). | Horizontal. |
| NATRON_PIXMAP_INTERP_BREAK | `interp_break.png` | **custom needed**: `interp-break`: Two curves meeting at the key at a sharp angle, the handles split apart. | Break. |
| DopeSheetView.cpp:781-798 keyframe textures (`interp_*.png`, `*_selected.png`, `keyframe_node_root*.png`) | `interp_curve.png`, `interp_constant_selected.png`, `keyframe_node_root.png`, `keyframe_node_root_selected.png` | keep PNG | KEEP PNG. These are 14 px GL textures for the keyframe markers in the dope-sheet track area, coloured and shaped to show the interpolation. They are data marks, not chrome. Proposal: leave them as PNG in M56. NATRON_PIXMAP_INTERP_CURVE is used only here. |

## Misc and Other toolbox candidates

Both use Lucide `puzzle` today (`GuiApplicationManager.cpp`, the `MISC_GROUPING` and `OTHER_PLUGINS` cases; `Gui20.cpp` `PLUGIN_GROUP_DEFAULT_ICON_PATH`). Pick one for each. They draw at 24 px.

| Pick | Group | Lucide glyph | Notes |
|---|---|---|---|
| Misc A | Misc | `shapes` | Assorted shapes: reads as "miscellaneous" without implying plugins. Recommended. |
| Misc B | Misc | `ellipsis` | Three dots: the literal "more". Very quiet in a 24 px toolbox column. |
| Misc C | Misc | `blocks` | Building blocks: utility nodes (Dot, NoOp, Backdrop-style helpers). |
| Misc D | Misc | `toy-brick` | A single brick: close to `puzzle`, so less distinct from Other. |
| Other A | Other | `puzzle` | Today's glyph. The plug-in metaphor fits third-party plugins with no known group. Recommended, once Misc moves off it. |
| Other B | Other | `plug` | Literal plug-in. Reads as power/connection rather than an effect. |
| Other C | Other | `package` | Installed package or bundle. |
| Other D | Other | `circle-ellipsis` | "Everything else": a ringed ellipsis, distinct from Misc's options. |

Recommendation: Misc → `shapes`, Other → keep `puzzle`, which then means only "third-party plugin".

## Custom glyphs to draw

| Slug | Brief | Used in |
|---|---|---|
| `gamma` | Greek letter gamma (γ) drawn as a single open stroke. | Viewer toolbar |
| `checkerboard` | Rounded square split into a 2×2 grid, two diagonal cells filled with currentColor. | Viewer toolbar |
| `key-prev` | Left chevron beside a small outline diamond (step back to the previous keyframe). | Timeline and transport, Tracker |
| `play-reverse` | Lucide `play` mirrored horizontally (triangle pointing left). | Timeline and transport, Tracker |
| `key-next` | Small outline diamond beside a right chevron (step on to the next keyframe). | Timeline and transport |
| `in-point` | Left square bracket `[` with a short arrow pointing into it (set the playback in point). | Timeline and transport |
| `out-point` | Right square bracket `]` with a short arrow pointing into it (set the playback out point). | Timeline and transport |
| `select-points` | `mouse-pointer-2` arrow with a small solid square control point at its tip. | Roto and RotoPaint |
| `select-curves` | `mouse-pointer-2` arrow beside a short Bézier arc. | Roto and RotoPaint |
| `select-feather` | `mouse-pointer-2` arrow with a small dashed-outline square point (feather point). | Roto and RotoPaint |
| `add-points` | `pen-tool` nib with a small plus at the top right. | Roto and RotoPaint |
| `remove-points` | `pen-tool` nib with a small minus at the top right. | Roto and RotoPaint |
| `cusp-point` | Two straight segments meeting at a sharp V around a solid square point. | Roto and RotoPaint |
| `open-close-curve` | Bézier arc whose two ends nearly meet, with a small gap marker between them. | Roto and RotoPaint |
| `remove-feather` | `pen-tool` nib with a small dashed square crossed out (drop the feather). | Roto and RotoPaint |
| `inverted` | Rounded square with a circle cut out, the area outside the circle hatched (inverted mask). | Roto and RotoPaint |
| `track-all-keys` | Two outline diamonds joined by a double-headed horizontal arrow. | Tracker |
| `track-current-key` | One outline diamond with short arrows leaving it to the left and right. | Tracker |
| `clear-anim-bw` | Row of three small diamonds, the left two crossed out, with a left arrow under them. | Tracker |
| `clear-anim-fw` | Row of three small diamonds, the right two crossed out, with a right arrow under them. | Tracker |
| `key-on-move` | Outline diamond at the centre of `move`'s four arrows. | Tracker |
| `interp-constant` | Step line (flat, vertical jump, flat) through a small diamond key. | Curve editor and dope sheet |
| `interp-linear` | Two straight segments meeting at a small diamond key. | Curve editor and dope sheet |
| `interp-smooth` | Gentle S-curve through a small diamond key (smooth, "Z"). | Curve editor and dope sheet |
| `interp-catmull` | Curve through a small diamond key with a slight overshoot either side (Catmull-Rom, "R"). | Curve editor and dope sheet |
| `interp-cubic` | Cubic curve through a small diamond key with tangent handles drawn ("C"). | Curve editor and dope sheet |
| `interp-horizontal` | Curve flattening into a horizontal tangent handle at the key ("H"). | Curve editor and dope sheet |
| `interp-break` | Two curves meeting at the key at a sharp angle, the handles split apart. | Curve editor and dope sheet |

`play-reverse` is only Lucide `play` mirrored. The contact sheet shows a draft of it. The others need drawing, and need the user's approval as P5.T4 requires.

## Ids with no caller (for P5.T5)

These ids appear only in `pixmapEnumToPath`. The ones marked † still have their PNG loaded by path elsewhere, so P5.T5's basename grep keeps those files.

`PLAYER_REWIND_ENABLED`, `RESTORE_DEFAULTS_DISABLED`, `COMBOBOX_PRESSED`, `READ_IMAGE`, `WRITE_IMAGE`, `CURVE`, `BEZIER_32`†, `PENCIL`†, `ELLIPSE`†, `RECTANGLE`†, `ADD_POINTS`†, `REMOVE_POINTS`†, `REMOVE_FEATHER`†, `CUSP_POINTS`†, `SMOOTH_POINTS`†, `OPEN_CLOSE_CURVE`†, `SELECT_POINTS`†, `SELECT_FEATHER`†, `SELECT_CURVES`†, `AUTO_KEYING_*`†, `STICKY_SELECTION_*`†, `FEATHER_LINK_*`†, `FEATHER_VISIBLE`/`_UNVISIBLE`†, `RIPPLE_EDIT_*`†, `CLEAR_BACKWARD_ANIMATION`†, `CLEAR_FORWARD_ANIMATION`†, `UPDATE_VIEWER_*`, `VIEWER_ICON`, `VIEWER_ZEBRA_*`, `ADD_TRACK`†, `TRACK_RANGE`†, `TRACK_ALL_KEYS`†, `TRACK_CURRENT_KEY`†, `TRACK_BACKWARD_*`†, `TRACK_FORWARD_*`†, `TRACK_PREVIOUS`†, `TRACK_NEXT`†, `RESET_TRACK_OFFSET`†, `HIDE_TRACK_ERROR`†, `SHOW_TRACK_ERROR`†, `CREATE_USER_KEY_ON_MOVE_*`†, `RESET_USER_KEYS`, `CENTER_VIEWER_ON_TRACK`†, `ROTOPAINT_BUILDUP_*`†, `ROTOPAINT_PRESSURE_*`†, `ROTO_NODE_ICON`, `INTERP_CURVE`† (dope-sheet texture).

P5.T4 can point the † ids at the same SVG as the path string, or leave them alone; nothing calls them.

## Not covered by this sheet

- The TabWidget pane buttons: `TAB_WIDGET_LAYOUT_BUTTON`, `_ANCHOR`, `SPLIT_HORIZONTALLY`/`VERTICALLY`. `MAXIMIZE_WIDGET` and `CLOSE_WIDGET` are shared with the panel and get the panel's glyphs.
- ScriptEditor: nine `SCRIPT_*` ids and `CLOSE_PANEL`. `CLEAR_ALL_ANIMATION` is shared and gets `trash`.
- SequenceFileDialog's `COMBOBOX`, the `LINK_CURSOR`/`LINK_MULT_CURSOR` cursors, `OPEN_EFFECTS_GROUPING`, `APP_ICON`.
- The stylesheet images (`mainstyle.qss`: check boxes, scroll bars, tree branches, group-box arrows, the `checkbox_animation*` knob states).
- Native-node and merge-operator icons (user decision).

## Open questions

1. Dope-sheet keyframe textures (`interp_*.png`, `*_selected.png`, `keyframe_node_root*.png`) are GL marker textures, not chrome. The sheet keeps them as PNG for M56. Should they be redrawn too?
2. Ids outside the five areas stay on PNG unless you want them in: TabWidget (layout, anchor, split horizontally/vertically), ScriptEditor (nine SCRIPT_* ids plus CLOSE_PANEL), SequenceFileDialog's COMBOBOX, and the link cursors. Should they join P5.T3, or go to a follow-up stub?
3. Play/stop and lock/unlock keep two glyphs because the shape itself changes. Is that right, or should a lock toggle collapse to one `lock` glyph with an On colour?
4. Three Lucide picks are weak and could become customs: pressure (`gauge`), reveal (`wand-sparkles`) and full frame (`fullscreen`).
5. The tracker's track-forward and track-backward buttons reuse the transport glyphs (`play`, mirrored play). Natron's PNGs give tracking its own look (a triangle with a target dot). Should tracking get its own customs?
6. Ten of the proposed Lucide files use compact arc flags in their path data (for example `a2 2 0 0112.67 19`): `eye`, `eye-off`, `droplet`, `eraser`, `diamond-plus`, `diamond-minus`, `feather`, `pen-tool`, `pin`, `zap`. ImageMagick's SVG reader drew `feather`, `pen-tool` and `zap` blank. None of the vendored toolbox glyphs use this syntax. At P5.T2's gate, check that Qt6::Svg draws these. If it doesn't, the vendored-unmodified rule needs an exception: re-save them with the flags spaced out.
7. The knob animation, reset and link buttons that P5.T3 names have no pixmap ids. The animation state comes from mainstyle.qss `checkbox_animation*.png`. They are out of this sheet; say if you want them in.
