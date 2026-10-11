# M56 icon mapping (P5.T1)

Date: 2026-10-11. Task: M56.P5.T1. Status: **pending user approval**. Once approved, the mapping goes into M56's `## Decisions`, and P5.T2-P5.T4 apply exactly this.

Contact sheet: `m56-icon-mapping.html`, built in the session scratchpad (`/tmp/claude-1000/-home-bosley-git-Natron/3b932fcc-55b5-46af-8a6f-5089cc13eb8c/scratchpad/m56-icon-mapping.html`) for the PM to publish. It shows today's PNG beside each proposed glyph at 18 px and 36 px on `#303030`, in the Normal (`#c8c8c8`), On (selection colour `#f28a00`, `Engine/Settings.cpp:1646-1648`) and Disabled (`#6d6d6d`, text blended 40% into the base) states.

Source: Lucide 1.55.0, commit `70562c1ee1c4fdcf736fe97bc893fb8511927934`. The ids and files come from `pixmapEnumToPath` (`Gui/GuiApplicationManager.cpp:93-801`) on `.worktrees/m56` at `ab21ebfc1`. The path strings come from `Engine/RotoPaint.cpp`, `Engine/TrackerNode.cpp`, `Engine/TrackerContextPrivate.cpp`, `Gui/MultiInstancePanel.cpp` and `Gui/DopeSheetView.cpp`. Every Lucide name below exists at that commit (checked against the git tree).

Scope: icon scope "Everything" (Q5 = c), so the areas are the viewer toolbar, the timeline, properties-panel chrome, roto, tracker, the curve editor and dope sheet, and (added in revision 2) the panes, properties bin, Script Editor, file dialog and link cursors, plus candidates for the Misc and Other toolbox groupings. The native-node and merge-operator icons are out of scope.

## Pair legend

- **Collapses**: the `*_ENABLED`/`*_DISABLED`, `*_ON`/`*_OFF` or `on`/`off` path pair becomes one glyph through `ThemedIcon::icon`. On draws in the selection colour, Off in the text colour, and Disabled is dimmed. One SVG file serves both ids.
- **Two glyphs**: the two states differ in shape (for example play → stop, or lock → lock-open), so they keep separate glyphs.
- **Custom**: no adequate Lucide glyph. A draft SVG is given in the "Custom glyph drafts" section. Final art is drawn in Lucide's style (24-unit grid, `currentColor`, stroke 2 that thins to 1.5 at load, round caps and joins), under GPL-2.0-or-later, in `Gui/Resources/Icons/natron/`.

## Counts

| Area | Rows | Lucide | Custom | Keep PNG | Collapsing pairs | Revised |
|---|---:|---:|---:|---:|---:|---:|
| Viewer toolbar | 14 | 10 | 4 | 0 | 10 | 3 |
| Timeline and transport | 17 | 10 | 7 | 0 | 1 | 4 |
| Properties panel | 17 | 17 | 0 | 0 | 5 | 0 |
| Roto and RotoPaint | 39 | 25 | 14 | 0 | 10 | 6 |
| Tracker | 27 | 18 | 9 | 0 | 5 | 1 |
| Curve editor and dope sheet | 16 | 0 | 16 | 0 | 9 | 9 |
| Panes, script editor and dialogs | 18 | 15 | 3 | 0 | 2 | 18 |
| **All** | 148 | 95 | 53 | 0 | 42 | 41 |

That is 84 distinct Lucide glyphs to vendor and **50 distinct custom glyphs**, each with a draft SVG below. Shared custom glyphs: `play-reverse` x3, `key-prev` x2.

**Revision 2 (2026-10-11, after the user's review).** Rows the user changed say "Revised" in their notes, and the sheet gives them a Revised badge. Changes: clip to project became `square-dashed`; ROI, render scale, previous/next increment, ellipse, rectangle, clone, pressure, build-up and track range became drawn customs; the in/out points lost the bracket top; reveal became `stamp`; the dope-sheet keyframe textures are replaced by nine custom markers; Other is `circle-ellipsis`; and the pane, properties-bin, Script Editor, file-dialog and cursor ids joined the scope. Every custom glyph now has a draft SVG.

## Viewer toolbar

The two rows above the viewport, built in ViewerTab.cpp:223-560 at 18 px (NATRON_MEDIUM_BUTTON_ICON_SIZE).

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_LOCKED / _UNLOCKED | `locked.png`, `unlocked.png` | `lock` + `lock-open` | **Two glyphs.** Sync-viewers toggle. Also the timeline triple-sync button, the roto panel lock column and NodeGraphPrivate. Keeps two shapes: open vs closed shackle reads better than colour alone. |
| NATRON_PIXMAP_MERGE_GROUPING | (already SVG) | `layers-2` | Compositing-operator label. Already an SVG since P4.T5; listed for completeness, no change. |
| NATRON_PIXMAP_VIEWER_GAIN_ENABLED / _DISABLED | `expoON.png`, `expoOFF.png` | `aperture` | **Collapses.** Gain (f-stop) slider toggle. Today's art is an "expo" ± box. Aperture ties gain to f-stops. |
| NATRON_PIXMAP_VIEWER_GAMMA_ENABLED / _DISABLED | `gammaON.png`, `gammaOFF.png` | **custom** `natron/gamma.svg` (draft below): Greek letter gamma (γ) drawn as a single open stroke. | **Collapses.** Gamma slider toggle. Lucide has no gamma; a letterform matches today's "Y"-shaped glyph. |
| NATRON_PIXMAP_VIEWER_AUTOCONTRAST_ENABLED / _DISABLED | `AutoContrastON.png`, `AutoContrast.png` | `contrast` | **Collapses.** Auto-contrast toggle. Today On adds a red ring. |
| NATRON_PIXMAP_VIEWER_CHECKERBOARD_ENABLED / _DISABLED | `checkerboard_on.png`, `checkerboard_off.png` | **custom** `natron/checkerboard.svg` (draft below): Rounded square split into a 2×2 grid, two diagonal cells filled with currentColor. | **Collapses.** Checkerboard toggle. Fallback if the custom is rejected: Lucide `grid-2x2` (outline only, reads as a grid rather than a checkerboard). |
| NATRON_PIXMAP_COLOR_PICKER | `color_picker.png` | `pipette` | Viewer colour picker; also KnobGuiColor's picker. Same glyph as the Keyer toolbox group; the contexts differ. |
| NATRON_PIXMAP_VIEWER_CLIP_TO_PROJECT_ENABLED / _DISABLED | `cliptoprojectEnabled.png`, `cliptoprojectDisabled.png` | `square-dashed` | **Collapses.** Clip to project format. Revised: user picked `square-dashed` (was `crop`). |
| NATRON_PIXMAP_VIEWER_FULL_FRAME_ON / _OFF | `fullFrameOn.png`, `fullFrameOff.png` | `fullscreen` | **Collapses.** Full-frame processing. Today's gear art doesn't say "whole frame". |
| NATRON_PIXMAP_VIEWER_ROI_ENABLED / _DISABLED | `viewer_roiEnabled.png`, `viewer_roiDisabled.png` | **custom** `natron/roi.svg` (draft below): Square with a short tick outside the middle of each side (crop-style region). | **Collapses.** Region of interest. Revised: custom crop-style square with a tick outside the middle of each side (was `square-dashed`, now used for clip to project). RotoPaint's bbox-click-anywhere toggle reuses these PNGs by path; it keeps its own glyph in the roto table. |
| NATRON_PIXMAP_VIEWER_RENDER_SCALE / _CHECKED | `renderScale.png`, `renderScale_checked.png` | **custom** `natron/render-scale.svg` (draft below): Hollow rounded square with a smaller filled square inset in the bottom-left corner. | **Collapses.** Proxy (render scale) toggle. Revised: custom hollow square with a smaller filled square inset bottom-left (was `scaling`). |
| NATRON_PIXMAP_PLAYER_PAUSE_ENABLED / _DISABLED | `pauseEnabled.png`, `pauseDisabled.png` | `pause` | **Collapses.** Pause viewer updates. Also ProgressTaskInfo (render queue) and TrackerNode's stop-tracking button (path `pauseDisabled.png`). |
| NATRON_PIXMAP_VIEWER_REFRESH / _REFRESH_ACTIVE | `refresh.png`, `refreshActive.png` | `refresh-cw` | **Collapses.** Force render. Not a checkable toggle: ViewerTab40.cpp:763 swaps the icon while rendering, so P5.T2 builds iconRefreshOn from a selection-colour renderPixmap. Also KnobGuiFile's reload button and the tracker's update-viewer toggle. |
| NATRON_PIXMAP_VIEWER_CENTER | `centerViewer.png` | `focus` | Fit/centre the viewer. Also the properties-panel "centre node graph on this node" button. |

## Timeline and transport

The player row under the viewer (ViewerTab.cpp:750-932, ViewerTab10.cpp:150-200), drawn at 18 px.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_PLAYER_FIRST_FRAME | `firstFrame.png` | `skip-back` | Go to first frame. |
| NATRON_PIXMAP_PLAYER_PREVIOUS_KEY | `prevKF.png` | **custom** `natron/key-prev.svg` (draft below): Left chevron beside a small outline diamond (step back to the previous keyframe). | Previous keyframe. Also RotoPanel; the tracker's PREV_USER_KEY reuses this glyph. |
| NATRON_PIXMAP_PLAYER_PREVIOUS_INCR | `previousIncr.png` | **custom** `natron/incr-prev.svg` (draft below): Two full-height bars with a left arrow spanning the gap between them: `\|<-\|`. | Back by the frame increment. Revised: a bounded jump `\|<-\|` (was `rewind`). Two full-height bars distinguish it from first frame (one bar plus triangle) and previous keyframe (chevron plus diamond). |
| NATRON_PIXMAP_PLAYER_PREVIOUS | `back1.png` | `step-back` | Previous frame. Also MultiInstancePanel's path `back1.png`. |
| NATRON_PIXMAP_PLAYER_REWIND_DISABLED | `rewind.png` | **custom** `natron/play-reverse.svg` (draft below): Lucide `play` mirrored horizontally (triangle pointing left). | **Two glyphs.** Play backward (Off state). Its On state is STOP_ENABLED, a different shape, so this pair keeps two glyphs. Also MultiInstancePanel's path `rewind.png`. |
| NATRON_PIXMAP_PLAYER_STOP_ENABLED / _STOP_DISABLED | `stopEnabled.png`, `stopDisabled.png` | `square` | Stop: the On state of both play buttons. STOP_DISABLED is used by ProgressTaskInfo. Both ids point at one glyph. |
| NATRON_PIXMAP_PLAYER_PLAY_DISABLED / _PLAY_ENABLED | `play.png`, `play_enabled.png` | `play` | **Two glyphs.** Play forward (Off state; On is `square`). PLAY_ENABLED is used only by SequenceFileDialog; both ids point at one glyph. Also ProgressTaskInfo and MultiInstancePanel's `play.png`. |
| NATRON_PIXMAP_PLAYER_NEXT | `forward1.png` | `step-forward` | Next frame. Also MultiInstancePanel's `forward1.png`. |
| NATRON_PIXMAP_PLAYER_NEXT_INCR | `nextIncr.png` | **custom** `natron/incr-next.svg` (draft below): Two full-height bars with a right arrow spanning the gap between them: `\|->\|`. | Forward by the frame increment. Revised: `\|->\|` (was `fast-forward`). |
| NATRON_PIXMAP_PLAYER_NEXT_KEY | `nextKF.png` | **custom** `natron/key-next.svg` (draft below): Small outline diamond beside a right chevron (step on to the next keyframe). | Next keyframe. Also RotoPanel; the tracker's NEXT_USER_KEY reuses this glyph. |
| NATRON_PIXMAP_PLAYER_LAST_FRAME | `lastFrame.png` | `skip-forward` | Go to last frame. |
| NATRON_PIXMAP_PLAYER_LOOP_MODE | `loopmode.png` | `repeat` | Playback mode: loop (the button cycles three ids). |
| NATRON_PIXMAP_PLAYER_BOUNCE | `bounce.png` | `arrow-left-right` | Playback mode: bounce. |
| NATRON_PIXMAP_PLAYER_PLAY_ONCE | `playOnce.png` | `arrow-right-to-line` | Playback mode: play once and stop. |
| NATRON_PIXMAP_PLAYER_TIMELINE_IN | `timelineIn.png` | **custom** `natron/in-point.svg` (draft below): Vertical stroke with a foot to the right and no top, and a short arrow pointing into it from the right. | Set the playback in point. Revised: the bracket has no top, only a vertical stroke and a foot. |
| NATRON_PIXMAP_PLAYER_TIMELINE_OUT | `timelineOut.png` | **custom** `natron/out-point.svg` (draft below): Mirror of in-point: vertical stroke with a foot to the left, arrow pointing in from the left. | Set the playback out point. Revised: mirror of the in point. |
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
| RotoPaint.cpp:667 `selectPoints.png` | `selectPoints.png` | **custom** `natron/select-points.svg` (draft below): `mouse-pointer-2` arrow with a small solid square control point at its tip. | Select points. |
| RotoPaint.cpp:680 `selectCurves.png` | `selectCurves.png` | **custom** `natron/select-curves.svg` (draft below): `mouse-pointer-2` arrow beside a short Bézier arc. | Select shapes. |
| RotoPaint.cpp:693 `selectFeather.png` | `selectFeather.png` | **custom** `natron/select-feather.svg` (draft below): `mouse-pointer-2` arrow with a small dashed-outline square point (feather point). | Select feather points. |
| RotoPaint.cpp:706 `addPoints.png` | `addPoints.png` | **custom** `natron/add-points.svg` (draft below): `pen-tool` nib with a small plus at the top right. | Add points. |
| RotoPaint.cpp:719 `removePoints.png` | `removePoints.png` | **custom** `natron/remove-points.svg` (draft below): `pen-tool` nib with a small minus at the top right. | Remove points. |
| RotoPaint.cpp:732 `cuspPoints.png` | `cuspPoints.png` | **custom** `natron/cusp-point.svg` (draft below): Two straight segments meeting at a sharp V around a solid square point. | Cusp points. |
| RotoPaint.cpp:745 `smoothPoints.png` | `smoothPoints.png` | `tangent` | Smooth points: Lucide's tangent (a line touching a circle) is the smooth-handle idea. |
| RotoPaint.cpp:758 `openCloseCurve.png` | `openCloseCurve.png` | **custom** `natron/open-close-curve.svg` (draft below): Bézier arc whose two ends nearly meet, with a small gap marker between them. | Open/close curve. |
| RotoPaint.cpp:772 `removeFeather.png` | `removeFeather.png` | **custom** `natron/remove-feather.svg` (draft below): `pen-tool` nib with a small dashed square crossed out (drop the feather). | Remove feather. |
| RotoPaint.cpp:786 `bezier32.png` (Bezier) | `bezier32.png` | `pen-tool` | Draw Bézier. |
| RotoPaint.cpp:800 `ellipse.png` | `ellipse.png` | **custom** `natron/ellipse-handles.svg` (draft below): Circle with four small filled square handles at the cardinal points. | Draw ellipse. Revised: custom circle with four square handles (was `circle`). |
| RotoPaint.cpp:814 `rectangle.png` | `rectangle.png` | **custom** `natron/rectangle-handles.svg` (draft below): Rectangle with small filled square handles on its corners. | Draw rectangle. Revised: custom rectangle with corner handles (was `rectangle-horizontal`). |
| RotoPaint.cpp:827 `rotopaint_solid.png` + NATRON_PIXMAP_ROTOPAINT_SOLID | `rotopaint_solid.png` | `paintbrush` | Brush tool and its tree icon. `brush` is already the Paint group and the RotoPaint plugin icon, so the tool takes `paintbrush`. |
| RotoPaint.cpp:840 `rotoToolPencil.png` (Pencil) | `rotoToolPencil.png` | `pencil` | Open-Bézier pencil tool. |
| RotoPaint.cpp:854 `rotopaint_eraser.png` + ROTOPAINT_ERASER | `rotopaint_eraser.png` | `eraser` | Eraser. |
| RotoPaint.cpp:868 `rotopaint_clone.png` + ROTOPAINT_CLONE | `rotopaint_clone.png` | **custom** `natron/clone-stamp.svg` (draft below): Two overlapping stamps; the rear one at `stroke-opacity=".4"`. | Clone. Revised: custom double stamp, the rear one at `stroke-opacity=".4"` (was `stamp`). Drawn as a smaller stamp so two fit on the grid without scaling the stroke. |
| RotoPaint.cpp:881 `rotopaint_reveal.png` + ROTOPAINT_REVEAL | `rotopaint_reveal.png` | `stamp` | Reveal. Revised: a single Lucide `stamp` (was `wand-sparkles`), pairing with the clone double stamp. |
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
| RotoPaint.cpp:374/400/426 `rotopaint_pressure_on/off.png` | `rotopaint_pressure_on.png`, `rotopaint_pressure_off.png` | **custom** `natron/pressure.svg` (draft below): Pen nib whose tip ends a filled stroke tapering from thin to thick. | **Collapses.** Pressure alters opacity / size / hardness (three buttons, one pair of files). Revised: custom pen nib drawing a tapered stroke (was `gauge`). |
| RotoPaint.cpp:440 `rotopaint_buildup_on/off.png` | `rotopaint_buildup_on.png`, `rotopaint_buildup_off.png` | **custom** `natron/buildup.svg` (draft below): Pyramid of six small circles: three, two, one. | **Collapses.** Build-up. Revised: custom 3-2-1 pyramid of circles (was `layers-plus`). |
| RotoPaint.cpp:453 `visible/unvisible.png` + NATRON_PIXMAP_VISIBLE / _UNVISIBLE | `visible.png`, `unvisible.png` | `eye` + `eye-off` | **Two glyphs.** Auto-connect viewer toggle, and the RotoPanel visibility column. The tree shows each state as a separate image, so keep two shapes. |
| RotoPaint.cpp:286/296 `addKF/removeKF.png` + NATRON_PIXMAP_ADD_KEYFRAME / _REMOVE_KEYFRAME | `addKF.png`, `removeKF.png` | `diamond-plus` + `diamond-minus` | Add / remove keyframe (toolbar and RotoPanel). The tracker's add/remove user key use the same pair. |
| NATRON_PIXMAP_LAYER | `layer.png` | `layers` | RotoPanel tree: layer item. |
| NATRON_PIXMAP_BEZIER | `bezier.png` | `spline` | RotoPanel tree: Bézier item. |
| NATRON_PIXMAP_INVERTED / _UNINVERTED | `inverted.png`, `uninverted.png` | **custom** `natron/inverted.svg` (draft below): Rounded square with a circle cut out; the area outside the circle filled at `fill-opacity=".45"` (inverted mask). | **Collapses.** RotoPanel inverted column. Tree cells, not a checkable button, so the cell uses renderPixmap in the selection colour for Inverted and the text colour for Uninverted. |
| NATRON_PIXMAP_ROTO_MERGE | `roto_merge.png` | `layers-2` | RotoPanel compositing-operator column header; matches the viewer's operator label. |
| NATRON_PIXMAP_CLEAR_ALL_ANIMATION | `clearAnimation.png` | `trash` | Clear all animation (RotoPanel, TrackerPanel, TrackerNode path). ScriptEditor also uses this id for "clear history". |

## Tracker

Path strings in Engine/TrackerNode.cpp:142-356 and TrackerContextPrivate.cpp:269-284, plus TrackerPanel.cpp ids and MultiInstancePanel.cpp's legacy path strings.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| TrackerNode.cpp:142 `addTrack.png` | `addTrack.png` | `crosshair` | Add a track. The id NATRON_PIXMAP_ADD_TRACK has no caller. |
| TrackerNode.cpp:155 `trackBackwardOn/Off.png` | `trackBackwardOn.png`, `trackBackwardOff.png` | **custom** `natron/play-reverse.svg` (draft below): Lucide `play` mirrored horizontally (triangle pointing left). | **Collapses.** Track backward. Shares the viewer's custom play-reverse glyph. |
| TrackerNode.cpp:166 `trackPrev.png` | `trackPrev.png` | `step-back` | Track one frame back. |
| TrackerNode.cpp:176 `pauseDisabled.png` (stop tracking) | `pauseDisabled.png` | `pause` | Stop tracking. Shares the viewer's `pause`. |
| TrackerNode.cpp:186 `trackNext.png` | `trackNext.png` | `step-forward` | Track one frame forward. |
| TrackerNode.cpp:198 `trackForwardOn/Off.png` | `trackForwardOn.png`, `trackForwardOff.png` | `play` | **Collapses.** Track forward. |
| TrackerNode.cpp:209 `trackRange.png` | `trackRange.png` | **custom** `natron/track-range.svg` (draft below): Play triangle followed by a closing square bracket `>]`. | Track a range. Revised: custom play triangle with a closing bracket `>]`, showing the direction (was `brackets`). TrackerNode has no backward-range button, so there is no mirrored variant. The "R" fallback isn't used: the bracket reads at 18 px. |
| TrackerNode.cpp:219 `trackAllKeyframes.png` | `trackAllKeyframes.png` | **custom** `natron/track-all-keys.svg` (draft below): Two outline diamonds joined by a double-headed horizontal arrow. | Track between all user keyframes. |
| TrackerNode.cpp:230 `trackCurrentKeyframe.png` | `trackCurrentKeyframe.png` | **custom** `natron/track-current-key.svg` (draft below): One outline diamond with short arrows leaving it to the left and right. | Track around the current keyframe. |
| TrackerNode.cpp:241/251 `addUserKey/removeUserKey.png` + NATRON_PIXMAP_ADD_USER_KEY / _REMOVE_USER_KEY | `addUserKey.png`, `removeUserKey.png` | `diamond-plus` + `diamond-minus` | Add / remove user keyframe; same pair as roto's add/remove keyframe. |
| NATRON_PIXMAP_PREV_USER_KEY / _NEXT_USER_KEY | `prevUserKey.png`, `nextUserKey.png` | **custom** `natron/key-prev.svg` (draft below): Left chevron beside a small outline diamond (step back to the previous keyframe). | TrackerPanel previous / next user key. Reuse the timeline's custom key-prev and key-next. |
| TrackerNode.cpp:261 `clearAnimation.png` | `clearAnimation.png` | `trash` | Clear all animation. Shares CLEAR_ALL_ANIMATION's glyph. |
| TrackerNode.cpp:271 `clearAnimationBw.png` | `clearAnimationBw.png` | **custom** `natron/clear-anim-bw.svg` (draft below): Row of three small diamonds, the left two crossed out, with a left arrow under them. | Clear animation before the current frame. The id CLEAR_BACKWARD_ANIMATION has no caller. |
| TrackerNode.cpp:281 `clearAnimationFw.png` | `clearAnimationFw.png` | **custom** `natron/clear-anim-fw.svg` (draft below): Row of three small diamonds, the right two crossed out, with a right arrow under them. | Clear animation after the current frame. |
| TrackerNode.cpp:293 `refreshActive/refresh.png` (update viewer) | `refreshActive.png`, `refresh.png` | `refresh-cw` | **Collapses.** Update the viewer while tracking. Shares the viewer's refresh glyph. |
| TrackerNode.cpp:306 `centerOnTrack.png` | `centerOnTrack.png` | `locate-fixed` | Centre the viewer on the track. |
| TrackerNode.cpp:318 `createKeyOnMoveOn/Off.png` | `createKeyOnMoveOn.png`, `createKeyOnMoveOff.png` | **custom** `natron/key-on-move.svg` (draft below): Outline diamond at the centre of `move`'s four arrows. | **Collapses.** Create a keyframe when a marker moves. |
| TrackerNode.cpp:332 `showTrackError/hideTrackError.png` | `showTrackError.png`, `hideTrackError.png` | `triangle-alert` | **Collapses.** Show the track error. |
| TrackerNode.cpp:345 `resetTrackOffset.png` | `resetTrackOffset.png` | `undo-dot` | Reset the track offset. |
| TrackerNode.cpp:356 `restoreDefaultEnabled.png` (reset track) | `restoreDefaultEnabled.png` | `rotate-ccw` | Reset the track; shares the panel's restore-defaults glyph. |
| TrackerContextPrivate.cpp:269 `patternSize.png` | `patternSize.png` | `scan` | Default pattern window size. |
| TrackerContextPrivate.cpp:284 `searchSize.png` | `searchSize.png` | `scan-search` | Default search window size. |
| NATRON_PIXMAP_SELECT_ALL | `cursor.png` | `list-checks` | TrackerPanel and MultiInstancePanel "select all tracks" buttons. Today this id shares roto's cursor.png; it gets its own glyph, since it selects rows, not a viewer tool. |
| MultiInstancePanel.cpp:1889 `back1.png` | `back1.png` | `step-back` | Legacy multi-instance tracker: previous frame. Same glyph as the timeline. |
| MultiInstancePanel.cpp:1891 `forward1.png` | `forward1.png` | `step-forward` | Legacy multi-instance tracker: next frame. |
| MultiInstancePanel.cpp:1893 `rewind.png` | `rewind.png` | **custom** `natron/play-reverse.svg` (draft below): Lucide `play` mirrored horizontally (triangle pointing left). | Legacy multi-instance tracker: track backward. |
| MultiInstancePanel.cpp:1895 `play.png` | `play.png` | `play` | Legacy multi-instance tracker: track forward. |

## Curve editor and dope sheet

The interpolation menu (DopeSheetView.cpp:2433-2513, 18 px) and the keyframe marker textures (DopeSheetView.cpp:781-798, 14 px GL textures). The curve editor itself has no icons; its interpolation menu is text-only.

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_INTERP_CONSTANT | `interp_constant.png` | **custom** `natron/interp-constant.svg` (draft below): Step line (flat, vertical jump, flat) through a small diamond key. | Constant. |
| NATRON_PIXMAP_INTERP_LINEAR | `interp_linear.png` | **custom** `natron/interp-linear.svg` (draft below): Two straight segments meeting at a small diamond key. | Linear. |
| NATRON_PIXMAP_INTERP_CURVE_Z | `interp_curve_z.png` | **custom** `natron/interp-smooth.svg` (draft below): Gentle S-curve through a small diamond key (smooth, "Z"). | Smooth. |
| NATRON_PIXMAP_INTERP_CURVE_R | `interp_curve_r.png` | **custom** `natron/interp-catmull.svg` (draft below): Curve through a small diamond key with a slight overshoot either side (Catmull-Rom, "R"). | Catmull-Rom. |
| NATRON_PIXMAP_INTERP_CURVE_C | `interp_curve_c.png` | **custom** `natron/interp-cubic.svg` (draft below): Cubic curve through a small diamond key with tangent handles drawn ("C"). | Cubic. |
| NATRON_PIXMAP_INTERP_CURVE_H | `interp_curve_h.png` | **custom** `natron/interp-horizontal.svg` (draft below): Curve flattening into a horizontal tangent handle at the key ("H"). | Horizontal. |
| NATRON_PIXMAP_INTERP_BREAK | `interp_break.png` | **custom** `natron/interp-break.svg` (draft below): Two curves meeting at the key at a sharp angle, the handles split apart. | Break. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_constant.png` / `interp_constant_selected.png` | `interp_constant.png`, `interp_constant_selected.png` | **custom** `natron/marker-constant.svg` (draft below): Square key marker, body at `fill-opacity=".35"`. | **Collapses.** Constant: filled square. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_linear.png` / `interp_linear_selected.png` | `interp_linear.png`, `interp_linear_selected.png` | **custom** `natron/marker-linear.svg` (draft below): Diamond key marker, body at `fill-opacity=".35"`. | **Collapses.** Linear: filled diamond. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_curve.png` / `interp_curve_selected.png` | `interp_curve.png`, `interp_curve_selected.png` | **custom** `natron/marker-curve.svg` (draft below): Round key marker, body at `fill-opacity=".35"`. | **Collapses.** Smooth curve (also the only use of NATRON_PIXMAP_INTERP_CURVE's file): filled circle. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_break.png` / `interp_break_selected.png` | `interp_break.png`, `interp_break_selected.png` | **custom** `natron/marker-break.svg` (draft below): Four-point star key marker, body at `fill-opacity=".35"`. | **Collapses.** Break: four-point star. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_curve_c.png` / `interp_curve_c_selected.png` | `interp_curve_c.png`, `interp_curve_c_selected.png` | **custom** `natron/marker-c.svg` (draft below): Ring with a C inside. | **Collapses.** Cubic: ringed C. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_curve_h.png` / `interp_curve_h_selected.png` | `interp_curve_h.png`, `interp_curve_h_selected.png` | **custom** `natron/marker-h.svg` (draft below): Ring with an H inside. | **Collapses.** Horizontal: ringed H. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_curve_r.png` / `interp_curve_r_selected.png` | `interp_curve_r.png`, `interp_curve_r_selected.png` | **custom** `natron/marker-r.svg` (draft below): Ring with an R inside. | **Collapses.** Catmull-Rom: ringed R. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `interp_curve_z.png` / `interp_curve_z_selected.png` | `interp_curve_z.png`, `interp_curve_z_selected.png` | **custom** `natron/marker-z.svg` (draft below): Ring with a Z inside. | **Collapses.** Smooth (Z): ringed Z. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |
| DopeSheetView.cpp:781-798 keyframe texture `keyframe_node_root.png` / `keyframe_node_root_selected.png` | `keyframe_node_root.png`, `keyframe_node_root_selected.png` | **custom** `natron/marker-node-root.svg` (draft below): U-shaped cup (node-root summary key), body at `fill-opacity=".35"`. | **Collapses.** Node-root summary key: U-shaped cup. Revised: replaces the PNG texture (was keep PNG). The selected texture is the same glyph in the On colour; the GL upload calls renderPixmap at 14 px in the text and selection colours. The 35% body fill uses explicit `fill-opacity`. |

## Panes, script editor and dialogs

Added in this revision at the user's request: the pane (TabWidget) buttons, the properties-bin buttons (GuiPrivate.cpp:330-360), the Script Editor (ScriptEditor.cpp:121-205), SequenceFileDialog's filter drop-down and the link cursors (GuiApplicationManagerPrivate.cpp:210-226).

| Id / path string | Today's file | Proposed glyph | Notes |
|---|---|---|---|
| NATRON_PIXMAP_TAB_WIDGET_LAYOUT_BUTTON | `layout.png` | `layout-grid` | Revised (new scope). The pane's corner menu button (TabWidget.cpp:274, :1786). |
| NATRON_PIXMAP_TAB_WIDGET_LAYOUT_BUTTON_ANCHOR | `layout_anchor.png` | **custom** `natron/layout-anchor.svg` (draft below): Lucide `layout-grid` with the top-left cell filled (this pane is the anchor). | Revised (new scope). Same button when this pane is the anchor (TabWidget.cpp:1784) and in the pane menu (:394). The code swaps the pixmap, so the anchor gets its own shape (a filled cell) rather than relying on colour. |
| NATRON_PIXMAP_TAB_WIDGET_SPLIT_HORIZONTALLY | `splitHorizontally.png` | `columns-2` | Revised (new scope). "Split horizontal" menu item; today's art shows a vertical divider (side-by-side panes), which `columns-2` keeps. The menu's Float and Close items use MAXIMIZE_WIDGET / CLOSE_WIDGET, see the panel table. |
| NATRON_PIXMAP_TAB_WIDGET_SPLIT_VERTICALLY | `splitVertically.png` | `rows-2` | Revised (new scope). "Split vertical" (stacked panes). |
| GuiPrivate.cpp:349-350 MINIMIZE_WIDGET / MAXIMIZE_WIDGET (minimise/maximise all panels) | `minimize.png`, `maximize.png` | `chevrons-down-up` | **Collapses.** Revised (new scope). The properties bin pairs MINIMIZE (On) with MAXIMIZE (Off) to mean "collapse / expand all panels". The panel table maps MAXIMIZE_WIDGET to the float glyph, which would read wrong here. Proposal: GuiPrivate builds this one button with `ThemedIcon::icon` from the `chevrons-down-up` path directly, so no new id is needed. |
| NATRON_PIXMAP_CLOSE_PANEL | `closePanel.png` | `list-x` | Revised (new scope). Properties bin "clear all panels" (GuiPrivate.cpp:336). ScriptEditor.cpp:124 also loads it, but :125 overwrites the same pixmap with CLEAR_ALL_ANIMATION, so that call is dead. |
| NATRON_PIXMAP_SCRIPT_PREVIOUS_SCRIPT | `previousScript.png` | `arrow-left` | Revised (new scope). Previous script in the history. |
| NATRON_PIXMAP_SCRIPT_NEXT_SCRIPT | `nextScript.png` | `arrow-right` | Revised (new scope). Next script in the history. |
| NATRON_PIXMAP_CLEAR_ALL_ANIMATION (Script Editor clear history) | `clearAnimation.png` | `trash` | Revised (new scope). Clear history (ScriptEditor.cpp:125) borrows the clear-animation id; it keeps the shared `trash` glyph. |
| NATRON_PIXMAP_SCRIPT_LOAD_EXEC_SCRIPT | `loadAndExecScript.png` | `file-play` | Revised (new scope). Open and execute a script. |
| NATRON_PIXMAP_SCRIPT_LOAD_SCRIPT | `loadScript.png` | `file-code` | Revised (new scope). Open a script without executing it. |
| NATRON_PIXMAP_SCRIPT_SAVE_SCRIPT | `saveScript.png` | `save` | Revised (new scope). Save the script. |
| NATRON_PIXMAP_SCRIPT_EXEC_SCRIPT | `execScript.png` | `circle-play` | Revised (new scope). Execute. A ringed play keeps it distinct from transport `play`. |
| NATRON_PIXMAP_SCRIPT_OUTPUT_PANE_ACTIVATED / _DEACTIVATED | `outputPanelActivated.png`, `outputPanelDeactivated.png` | `panel-top` | **Collapses.** Revised (new scope). Show/hide the output pane (ScriptEditor.cpp:191-192 builds an On/Off QIcon). |
| NATRON_PIXMAP_SCRIPT_CLEAR_OUTPUT | `clearOutput.png` | `brush-cleaning` | Revised (new scope). Clear the output pane. |
| NATRON_PIXMAP_COMBOBOX / _COMBOBOX_PRESSED | `combobox.png`, `pressed_combobox.png` | `chevron-down` | Revised (new scope). SequenceFileDialog's filter drop-down (SequenceFileDialog.cpp:450). COMBOBOX_PRESSED has no caller; both ids point at one glyph. The preview toggle in the same dialog uses PLAY_ENABLED/DISABLED, see the timeline table. |
| NATRON_PIXMAP_LINK_CURSOR | `linkCursor.png` | **custom** `natron/link-cursor.svg` (draft below): Lucide `link` over a 4.5-wide black halo at 70% (literal `#000000`, so it survives recolouring) to stay visible on any image. | Revised (new scope). Cursor while linking a parameter. Cursors sit over arbitrary images, so the draft carries a literal black halo that recolouring leaves alone. QCursor(p) uses the centre hotspot, as today. |
| NATRON_PIXMAP_LINK_MULT_CURSOR | `linkMultCursor.png` | **custom** `natron/link-mult-cursor.svg` (draft below): Link cursor with a small x in the free bottom-right corner, same black halo. | Revised (new scope). Cursor while linking several parameters. |

## Misc and Other toolbox candidates

Both use Lucide `puzzle` today (`GuiApplicationManager.cpp`, the `MISC_GROUPING` and `OTHER_PLUGINS` cases; `Gui20.cpp` `PLUGIN_GROUP_DEFAULT_ICON_PATH`). Pick one for each. They draw at 24 px.

| Pick | Group | Lucide glyph | Notes |
|---|---|---|---|
| Misc A | Misc | `shapes` | Assorted shapes: reads as "miscellaneous" without implying plugins. Recommended. |
| Misc B | Misc | `ellipsis` | Three dots: the literal "more". Very quiet in a 24 px toolbox column. |
| Misc C | Misc | `blocks` | Building blocks: utility nodes (Dot, NoOp, Backdrop-style helpers). |
| Misc D | Misc | `toy-brick` | A single brick: close to `puzzle`, so less distinct from Other. |
| Other A | Other | `puzzle` | Today's glyph. The plug-in metaphor fits third-party plugins with no known group. Not chosen. |
| Other B | Other | `plug` | Literal plug-in. Reads as power/connection rather than an effect. |
| Other C | Other | `package` | Installed package or bundle. |
| Other D | Other | `circle-ellipsis` | "Everything else": a ringed ellipsis, distinct from Misc's options. Chosen by the user (revised). |

Decided: **Other → `circle-ellipsis` (Pick Other D, user)**. Misc is still open; recommendation `shapes` (Pick Misc A).

## Custom glyph drafts

Each draft is on Lucide's 24-unit grid with `stroke="currentColor"`, `stroke-width="2"` (ThemedIcon thins it to 1.5) and round caps and joins, under GPL-2.0-or-later, for `Gui/Resources/Icons/natron/<slug>.svg`. Any fading uses explicit `stroke-opacity`/`fill-opacity`, since ThemedIcon recolours to one flat colour. The link cursors' halo uses literal `#000000`, which recolouring leaves alone. These are drafts for approval; P5.T2-P5.T4 vendor the approved versions.

### `gamma`

Greek letter gamma (γ) drawn as a single open stroke. Used in: Viewer toolbar.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M5 6c3.5 0 5.5 4.5 7 10"/>
  <path d="M19 6c-2.5 2-5 6-7 10"/>
  <path d="M12 16c-1 2-1.4 4.5 0 4.5s1-2.5 0-4.5"/>
</svg>
```

### `checkerboard`

Rounded square split into a 2×2 grid, two diagonal cells filled with currentColor. Used in: Viewer toolbar.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <rect x="4" y="4" width="8" height="8" fill="currentColor" stroke="none"/>
  <rect x="12" y="12" width="8" height="8" fill="currentColor" stroke="none"/>
  <rect x="3" y="3" width="18" height="18" rx="2"/>
  <path d="M12 3v18"/>
  <path d="M3 12h18"/>
</svg>
```

### `roi`

Square with a short tick outside the middle of each side (crop-style region). Used in: Viewer toolbar.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <rect x="6" y="6" width="12" height="12" rx="1"/>
  <path d="M12 2v2.5"/>
  <path d="M12 19.5V22"/>
  <path d="M2 12h2.5"/>
  <path d="M19.5 12H22"/>
</svg>
```

### `render-scale`

Hollow rounded square with a smaller filled square inset in the bottom-left corner. Used in: Viewer toolbar.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <rect x="3" y="3" width="18" height="18" rx="2"/>
  <rect x="6" y="12" width="6" height="6" rx="1" fill="currentColor"/>
</svg>
```

### `key-prev`

Left chevron beside a small outline diamond (step back to the previous keyframe). Used in: Timeline and transport, Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="m8.5 7-5 5 5 5"/>
  <path d="M16 6.5l5.5 5.5-5.5 5.5-5.5-5.5z"/>
</svg>
```

### `incr-prev`

Two full-height bars with a left arrow spanning the gap between them: `|<-|`. Used in: Timeline and transport.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 6v12"/>
  <path d="M21 6v12"/>
  <path d="M17 12H7"/>
  <path d="M10.5 8.5 7 12l3.5 3.5"/>
</svg>
```

### `play-reverse`

Lucide `play` mirrored horizontally (triangle pointing left). Used in: Timeline and transport, Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <g transform="matrix(-1 0 0 1 24 0)">
  <path d="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z"/>
  </g>
</svg>
```

### `incr-next`

Two full-height bars with a right arrow spanning the gap between them: `|->|`. Used in: Timeline and transport.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 6v12"/>
  <path d="M21 6v12"/>
  <path d="M7 12h10"/>
  <path d="M13.5 8.5 17 12l-3.5 3.5"/>
</svg>
```

### `key-next`

Small outline diamond beside a right chevron (step on to the next keyframe). Used in: Timeline and transport.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="m15.5 7 5 5-5 5"/>
  <path d="M8 6.5l5.5 5.5-5.5 5.5L2.5 12z"/>
</svg>
```

### `in-point`

Vertical stroke with a foot to the right and no top, and a short arrow pointing into it from the right. Used in: Timeline and transport.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M7 4v16h4"/>
  <path d="M21 12h-9"/>
  <path d="m15 9-3 3 3 3"/>
</svg>
```

### `out-point`

Mirror of in-point: vertical stroke with a foot to the left, arrow pointing in from the left. Used in: Timeline and transport.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M17 4v16h-4"/>
  <path d="M3 12h9"/>
  <path d="m9 9 3 3-3 3"/>
</svg>
```

### `select-points`

`mouse-pointer-2` arrow with a small solid square control point at its tip. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 3l5.5 13.5 2.2-5.8 5.8-2.2z"/>
  <rect x="16.5" y="16.5" width="4.5" height="4.5" rx=".5" fill="currentColor"/>
</svg>
```

### `select-curves`

`mouse-pointer-2` arrow beside a short Bézier arc. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 3l5.5 13.5 2.2-5.8 5.8-2.2z"/>
  <path d="M12 21c.5-5 4-8.5 9-9"/>
</svg>
```

### `select-feather`

`mouse-pointer-2` arrow with a small dashed-outline square point (feather point). Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 3l5.5 13.5 2.2-5.8 5.8-2.2z"/>
  <rect x="16" y="16" width="5" height="5" rx=".5" stroke-dasharray="1.6 1.6"/>
</svg>
```

### `add-points`

`pen-tool` nib with a small plus at the top right. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M4 20l2.5-8.5L12 9l3 3-3 5.5z"/>
  <path d="M4 20l5-5"/>
  <circle cx="10.5" cy="13.5" r="1.3"/>
  <path d="M18 3v6"/>
  <path d="M15 6h6"/>
</svg>
```

### `remove-points`

`pen-tool` nib with a small minus at the top right. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M4 20l2.5-8.5L12 9l3 3-3 5.5z"/>
  <path d="M4 20l5-5"/>
  <circle cx="10.5" cy="13.5" r="1.3"/>
  <path d="M15 6h6"/>
</svg>
```

### `cusp-point`

Two straight segments meeting at a sharp V around a solid square point. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3.5 19 12 7l8.5 12"/>
  <rect x="10" y="4.5" width="4" height="4" rx=".5" fill="currentColor"/>
</svg>
```

### `open-close-curve`

Bézier arc whose two ends nearly meet, with a small gap marker between them. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M8.5 20.2A8.5 8.5 0 1 1 15.5 20.2"/>
  <circle cx="8.5" cy="20.2" r="1" fill="currentColor"/>
  <circle cx="15.5" cy="20.2" r="1" fill="currentColor"/>
</svg>
```

### `remove-feather`

`pen-tool` nib with a small dashed square crossed out (drop the feather). Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M4 20l2.5-8.5L12 9l3 3-3 5.5z"/>
  <path d="M4 20l5-5"/>
  <circle cx="10.5" cy="13.5" r="1.3"/>
  <rect x="15" y="3" width="6" height="6" rx=".5" stroke-dasharray="1.6 1.6"/>
  <path d="M14 10 22 2"/>
</svg>
```

### `ellipse-handles`

Circle with four small filled square handles at the cardinal points. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="12" cy="12" r="8"/>
  <rect x="10.25" y="2.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
  <rect x="18.25" y="10.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
  <rect x="10.25" y="18.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
  <rect x="2.25" y="10.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
</svg>
```

### `rectangle-handles`

Rectangle with small filled square handles on its corners. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <rect x="4" y="6" width="16" height="12" rx=".5"/>
  <rect x="2.25" y="4.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
  <rect x="18.25" y="4.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
  <rect x="2.25" y="16.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
  <rect x="18.25" y="16.25" width="3.5" height="3.5" rx=".4" fill="currentColor" stroke="none"/>
</svg>
```

### `clone-stamp`

Two overlapping stamps; the rear one at `stroke-opacity=".4"`. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <g transform="translate(-3 -2)" stroke-opacity=".4">
  <path d="M13.5 11V8.6c0-1.1 1-1.5 1-3.1a2.5 2.5 0 0 0-5 0c0 1.6 1 2 1 3.1V11"/>
  <path d="M17 13.5a2 2 0 0 0-2-2H9a2 2 0 0 0-2 2V15a.8.8 0 0 0 .8.8h8.4a.8.8 0 0 0 .8-.8z"/>
  <path d="M7.5 19h9"/>
  </g>
  <g transform="translate(3 2)">
  <path d="M13.5 11V8.6c0-1.1 1-1.5 1-3.1a2.5 2.5 0 0 0-5 0c0 1.6 1 2 1 3.1V11"/>
  <path d="M17 13.5a2 2 0 0 0-2-2H9a2 2 0 0 0-2 2V15a.8.8 0 0 0 .8.8h8.4a.8.8 0 0 0 .8-.8z"/>
  <path d="M7.5 19h9"/>
  </g>
</svg>
```

### `pressure`

Pen nib whose tip ends a filled stroke tapering from thin to thick. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M2 21.5c4 0 7.5-1.5 10.2-4.2l1.4 1.4C10.5 21 6.5 22 2 21.5z" fill="currentColor" stroke="none"/>
  <path d="M13 15l1.6-4.6L19.2 8.8l3 3-1.6 4.6z"/>
  <path d="M13 15l4-4"/>
</svg>
```

### `buildup`

Pyramid of six small circles: three, two, one. Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="6" cy="18.5" r="2.3"/>
  <circle cx="12" cy="18.5" r="2.3"/>
  <circle cx="18" cy="18.5" r="2.3"/>
  <circle cx="9" cy="12.5" r="2.3"/>
  <circle cx="15" cy="12.5" r="2.3"/>
  <circle cx="12" cy="6.5" r="2.3"/>
</svg>
```

### `inverted`

Rounded square with a circle cut out; the area outside the circle filled at `fill-opacity=".45"` (inverted mask). Used in: Roto and RotoPaint.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path fill="currentColor" stroke="none" fill-opacity=".45" fill-rule="evenodd" d="M5 3h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM12 7a5 5 0 1 0 0 10 5 5 0 1 0 0-10z"/>
  <rect x="3" y="3" width="18" height="18" rx="2"/>
  <circle cx="12" cy="12" r="5"/>
</svg>
```

### `track-range`

Play triangle followed by a closing square bracket `>]`. Used in: Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M4 6.2v11.6a1 1 0 0 0 1.5.86l9-5.8a1 1 0 0 0 0-1.72l-9-5.8A1 1 0 0 0 4 6.2z"/>
  <path d="M17.5 4H20v16h-2.5"/>
</svg>
```

### `track-all-keys`

Two outline diamonds joined by a double-headed horizontal arrow. Used in: Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M4.5 8.5 8 12l-3.5 3.5L1 12z"/>
  <path d="M19.5 8.5 23 12l-3.5 3.5L16 12z"/>
  <path d="M9.5 12h5"/>
  <path d="m11 10-2 2 2 2"/>
  <path d="m13 10 2 2-2 2"/>
</svg>
```

### `track-current-key`

One outline diamond with short arrows leaving it to the left and right. Used in: Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M12 7.5l4.5 4.5-4.5 4.5L7.5 12z"/>
  <path d="M5.5 12H2"/>
  <path d="m4 10-2 2 2 2"/>
  <path d="M18.5 12H22"/>
  <path d="m20 10 2 2-2 2"/>
</svg>
```

### `clear-anim-bw`

Row of three small diamonds, the left two crossed out, with a left arrow under them. Used in: Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <g stroke-opacity=".4">
  <path d="M5 7l3 3-3 3-3-3z"/>
  <path d="M12 7l3 3-3 3-3-3z"/>
  </g>
  <path d="M19 7l3 3-3 3-3-3z"/>
  <path d="M1.5 10h14.5"/>
  <path d="M19 19H5"/>
  <path d="m8 16-3 3 3 3"/>
</svg>
```

### `clear-anim-fw`

Row of three small diamonds, the right two crossed out, with a right arrow under them. Used in: Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M5 7l3 3-3 3-3-3z"/>
  <g stroke-opacity=".4">
  <path d="M12 7l3 3-3 3-3-3z"/>
  <path d="M19 7l3 3-3 3-3-3z"/>
  </g>
  <path d="M8 10h14.5"/>
  <path d="M5 19h14"/>
  <path d="m16 16 3 3-3 3"/>
</svg>
```

### `key-on-move`

Outline diamond at the centre of `move`'s four arrows. Used in: Tracker.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M12 2v4.5"/>
  <path d="M12 17.5V22"/>
  <path d="M2 12h4.5"/>
  <path d="M17.5 12H22"/>
  <path d="m9 5 3-3 3 3"/>
  <path d="m9 19 3 3 3-3"/>
  <path d="m5 9-3 3 3 3"/>
  <path d="m19 9 3 3-3 3"/>
  <path d="M12 8.5l3.5 3.5-3.5 3.5L8.5 12z"/>
</svg>
```

### `interp-constant`

Step line (flat, vertical jump, flat) through a small diamond key. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 17h9V7h9"/>
  <path d="M12 14.5l2.5 2.5-2.5 2.5-2.5-2.5z" fill="currentColor"/>
</svg>
```

### `interp-linear`

Two straight segments meeting at a small diamond key. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 19 12 6l9 11"/>
  <path d="M12 3.5l2.5 2.5-2.5 2.5-2.5-2.5z" fill="currentColor"/>
</svg>
```

### `interp-smooth`

Gentle S-curve through a small diamond key (smooth, "Z"). Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 19c6 0 4-7 9-7s3-7 9-7"/>
  <path d="M12 9.5l2.5 2.5-2.5 2.5-2.5-2.5z" fill="currentColor"/>
</svg>
```

### `interp-catmull`

Curve through a small diamond key with a slight overshoot either side (Catmull-Rom, "R"). Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 15c2 0 3 2.5 5 1.5S10 9 12 9s2.5 6.5 4 7.5 3.5-1.5 5-1.5"/>
  <path d="M12 6.5l2.5 2.5-2.5 2.5-2.5-2.5z" fill="currentColor"/>
</svg>
```

### `interp-cubic`

Cubic curve through a small diamond key with tangent handles drawn ("C"). Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 19c4 0 5-7 9-7s5-7 9-7"/>
  <path d="M6.5 12h11"/>
  <circle cx="6.5" cy="12" r="1" fill="currentColor"/>
  <circle cx="17.5" cy="12" r="1" fill="currentColor"/>
  <path d="M12 9.5l2.5 2.5-2.5 2.5-2.5-2.5z" fill="currentColor"/>
</svg>
```

### `interp-horizontal`

Curve flattening into a horizontal tangent handle at the key ("H"). Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 19c3-5 5-7 9-7s6 2 9 7"/>
  <path d="M7 12h10"/>
  <path d="M12 9.5l2.5 2.5-2.5 2.5-2.5-2.5z" fill="currentColor"/>
</svg>
```

### `interp-break`

Two curves meeting at the key at a sharp angle, the handles split apart. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 19c3 0 7-3 9-9"/>
  <path d="M12 10c1 4 5 9 9 9"/>
  <path d="m12 10-3-5.5"/>
  <path d="m12 10 4.5-4"/>
  <path d="M12 7.5l2.5 2.5-2.5 2.5-2.5-2.5z" fill="currentColor"/>
</svg>
```

### `marker-constant`

Square key marker, body at `fill-opacity=".35"`. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <rect x="5" y="5" width="14" height="14" rx="1.5" fill="currentColor" fill-opacity=".35"/>
</svg>
```

### `marker-linear`

Diamond key marker, body at `fill-opacity=".35"`. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M12 3l9 9-9 9-9-9z" fill="currentColor" fill-opacity=".35"/>
</svg>
```

### `marker-curve`

Round key marker, body at `fill-opacity=".35"`. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="12" cy="12" r="8" fill="currentColor" fill-opacity=".35"/>
</svg>
```

### `marker-break`

Four-point star key marker, body at `fill-opacity=".35"`. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M12 2.5l2.6 6.9L21.5 12l-6.9 2.6L12 21.5l-2.6-6.9L2.5 12l6.9-2.6z" fill="currentColor" fill-opacity=".35"/>
</svg>
```

### `marker-c`

Ring with a C inside. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="12" cy="12" r="9.5"/>
  <path d="M15.5 9.2a4 4 0 1 0 0 5.6"/>
</svg>
```

### `marker-h`

Ring with an H inside. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="12" cy="12" r="9.5"/>
  <path d="M9 7.5v9"/>
  <path d="M15 7.5v9"/>
  <path d="M9 12h6"/>
</svg>
```

### `marker-r`

Ring with an R inside. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="12" cy="12" r="9.5"/>
  <path d="M9.5 16.5v-9h3a2.25 2.25 0 0 1 0 4.5h-3"/>
  <path d="m12.5 12 2.5 4.5"/>
</svg>
```

### `marker-z`

Ring with a Z inside. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <circle cx="12" cy="12" r="9.5"/>
  <path d="M9 7.5h6l-6 9h6"/>
</svg>
```

### `marker-node-root`

U-shaped cup (node-root summary key), body at `fill-opacity=".35"`. Used in: Curve editor and dope sheet.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M5.5 3.5h4v9a2.5 2.5 0 0 0 5 0v-9h4v9a6.5 6.5 0 0 1-13 0z" fill="currentColor" fill-opacity=".35"/>
</svg>
```

### `layout-anchor`

Lucide `layout-grid` with the top-left cell filled (this pane is the anchor). Used in: Panes, script editor and dialogs.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <rect x="3" y="3" width="7" height="7" rx="1" fill="currentColor"/>
  <rect x="14" y="3" width="7" height="7" rx="1"/>
  <rect x="14" y="14" width="7" height="7" rx="1"/>
  <rect x="3" y="14" width="7" height="7" rx="1"/>
</svg>
```

### `link-cursor`

Lucide `link` over a 4.5-wide black halo at 70% (literal `#000000`, so it survives recolouring) to stay visible on any image. Used in: Panes, script editor and dialogs.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <g stroke="#000000" stroke-opacity=".7" stroke-width="4.5">
  <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
  <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
  </g>
  <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
  <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
</svg>
```

### `link-mult-cursor`

Link cursor with a small x in the free bottom-right corner, same black halo. Used in: Panes, script editor and dialogs.

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <g stroke="#000000" stroke-opacity=".7" stroke-width="4.5">
  <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
  <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
  <path d="m17 17 4.5 4.5"/>
  <path d="m21.5 17-4.5 4.5"/>
  </g>
  <path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/>
  <path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>
  <path d="m17 17 4.5 4.5"/>
  <path d="m21.5 17-4.5 4.5"/>
</svg>
```

## Ids with no caller (for P5.T5)

These ids appear only in `pixmapEnumToPath`. The ones marked † still have their PNG loaded by path elsewhere, so P5.T5's basename grep keeps those files.

`PLAYER_REWIND_ENABLED`, `RESTORE_DEFAULTS_DISABLED`, `COMBOBOX_PRESSED`, `READ_IMAGE`, `WRITE_IMAGE`, `CURVE`, `BEZIER_32`†, `PENCIL`†, `ELLIPSE`†, `RECTANGLE`†, `ADD_POINTS`†, `REMOVE_POINTS`†, `REMOVE_FEATHER`†, `CUSP_POINTS`†, `SMOOTH_POINTS`†, `OPEN_CLOSE_CURVE`†, `SELECT_POINTS`†, `SELECT_FEATHER`†, `SELECT_CURVES`†, `AUTO_KEYING_*`†, `STICKY_SELECTION_*`†, `FEATHER_LINK_*`†, `FEATHER_VISIBLE`/`_UNVISIBLE`†, `RIPPLE_EDIT_*`†, `CLEAR_BACKWARD_ANIMATION`†, `CLEAR_FORWARD_ANIMATION`†, `UPDATE_VIEWER_*`, `VIEWER_ICON`, `VIEWER_ZEBRA_*`, `ADD_TRACK`†, `TRACK_RANGE`†, `TRACK_ALL_KEYS`†, `TRACK_CURRENT_KEY`†, `TRACK_BACKWARD_*`†, `TRACK_FORWARD_*`†, `TRACK_PREVIOUS`†, `TRACK_NEXT`†, `RESET_TRACK_OFFSET`†, `HIDE_TRACK_ERROR`†, `SHOW_TRACK_ERROR`†, `CREATE_USER_KEY_ON_MOVE_*`†, `RESET_USER_KEYS`, `CENTER_VIEWER_ON_TRACK`†, `ROTOPAINT_BUILDUP_*`†, `ROTOPAINT_PRESSURE_*`†, `ROTO_NODE_ICON`, `INTERP_CURVE`† (dope-sheet texture).

P5.T4 can point the † ids at the same SVG as the path string, or leave them alone; nothing calls them.

## Not covered by this sheet

- `OPEN_EFFECTS_GROUPING` and `APP_ICON`: no caller outside `pixmapEnumToPath` (the toolbox doesn't use the OpenEffects logo, and the application icon is the Natron logo, not chrome).
- The stylesheet images (`mainstyle.qss`: check boxes, scroll bars, tree branches, group-box arrows, the `checkbox_animation*` knob states). They aren't pixmap ids.
- Native-node and merge-operator icons (user decision).

## Open questions

1. Custom drafts are first drafts. Approve them as drawn, or mark the ones to rework. The 14 px dope-sheet markers (ringed C/H/R/Z) are the tightest; check them at real size in the gate build.
2. The properties bin's minimise/maximise-all button pairs MINIMIZE_WIDGET with MAXIMIZE_WIDGET, which this sheet maps to the float glyph. The proposal builds that one button from `chevrons-down-up` through `ThemedIcon::icon` by path (no new id). Is that acceptable, or should MAXIMIZE_WIDGET stay a generic maximise glyph?
3. The link cursors carry a literal black halo (`#000000` at 70%) so they stay visible over any image; ThemedIcon recolours only `currentColor`, so the halo survives. Is a two-tone cursor fine, given every other icon is flat?
4. Remaining weak Lucide picks: full frame (`fullscreen`), reset track offset (`undo-dot`), Script Editor previous/next (`arrow-left`/`arrow-right`). Say if any should become a custom.
5. The tracker's track-forward and track-backward buttons reuse the transport glyphs (`play`, mirrored play), while track range gets its own `>]`. Should forward and backward also get tracker-specific customs?
6. The knob animation, reset and link buttons that P5.T3 names have no pixmap ids. The animation state comes from mainstyle.qss `checkbox_animation*.png`. They are out of this sheet; say if you want them in.
7. Ten of the proposed Lucide files use compact arc flags in their path data (for example `a2 2 0 0112.67 19`): `eye`, `eye-off`, `droplet`, `eraser`, `diamond-plus`, `diamond-minus`, `feather`, `pen-tool`, `pin`, `zap`. ImageMagick's SVG reader drew `feather`, `pen-tool` and `zap` blank. None of the vendored toolbox glyphs use this syntax, and the custom drafts don't either. At P5.T2's gate, check that Qt6::Svg draws these. If it doesn't, the vendored-unmodified rule needs an exception: re-save them with the flags spaced out.
