# M56 - Visual And Menu Polish

Full title: Polish — visual and menus

A consolidated milestone for independent visual-polish work:
- Node text layout: grow horizontally to a max width, then wrap vertically
  instead of overflowing.
- Icon replacement pass: many icons are hard to read; replace/redesign for
  legibility.
- Stylesheet/look-and-feel overhaul: revisit spacing, density and visual
  hierarchy — panels feel cramped and cluttered.
- Premult/Unpremult's unpremult-by channel picker has no label (the plugin
  never labelled it either); give it one. (User, 2026-09-30, from M65 T5f.)
- On IDistort and STMap the "V Channel" picker is indented relative to
  "U Channel"; align it. (User, 2026-09-30, from M65 T5f.)

Acceptance sketch:
- Long node names wrap instead of overflowing the node silhouette.
- A defined set of hard-to-read icons is replaced with clearer versions.
- Key panels (node graph, properties, viewer) read as less cramped, judged
  against before/after screenshots.
- The unpremult-by picker is labelled, and IDistort/STMap's U/V/A channel
  pickers line up.

Scoping draft (2026-10-10): `PLAN/DESIGN/2026-10-10-m44-m55-m56-scoping.md` holds a draft phase/task breakdown and the user questions to settle before elaborating. Deps: M24 (confirmed by the user).

Design inputs: `PLAN/DESIGN/2026-10-10-m44-m55-m56-scoping.md` (M56 section), `PLAN/DESIGN/2026-10-10-icon-direction-options.md` (icon audit, loaders, cache bug), and the icon-options page (claude.ai artifact `9B52JMKTyWw5Ufep99afxr`, "Chosen" section, which holds the toolbox glyph SVGs). The `## Phase 56.8: Shot review round 1 fixes

- [x] M56.P8.T1 — Apply the user's shot-review fixes (artifact `6y7CdSBwvuiThopTMEmBYJ` comments + chat, 2026-10-12)
  - files: `Gui/NodeGui.cpp` (label alignment), `Gui/LayerChannelRow.*` / `Gui/KnobGuiLayerSelect.*` / `Gui/KnobGuiChannelSelect.*` (channel picker), `Gui/KnobGuiContainerHelper.cpp` (button column), `Engine/OfxEffectInstance.cpp` (IDistort UV offset row), `Gui/GuiApplicationManager.cpp` or the toolbox builder (stray coloured icon), Merge panel knob/label code, `Tests/gui/m56_visual.py`
  - approach:
    - Left-align node labels by default; wrapped continuation lines get a small hanging indent.
    - Align the Premult channel picker's dropdowns.
    - Dim the very saturated R/G/B/A channel buttons in the channel picker.
    - Left-align the picker's "+" button under the layer dropdown, not offset to the left.
    - Left-align buttons with the first knob column, not with the labels.
    - Merge panel: fix the dark label text and the misaligned Output row.
    - IDistort: fix the odd alignment of the UV Offset knobs.
    - Toolbox: make the one remaining coloured icon monochrome.
  - verify: an incremental build; `m56_visual.py` passes at 1× and 2× with new checks for each item; the revised shots are approved by the user.
  - size: L

## Decisions` below override all three. Line numbers are for `main` at `a50bef552` and will move once M44 and M55 merge. One build at the end (user rule): implementers do not compile. Only P1.T1 runs against a standing binary.

## Phase 56.1: Before evidence

- [x] M56.P1.T1 — Write the M56 screenshot script and take the "before" shots against the existing binary
  - files: `Tests/gui/m56_visual.py` (new)
  - approach:
    - Model it on `Tests/gui/m55_input_pipes.py` / `build/m24-shots/m24_border.py` (watchdog, `gt.section`/`gt.check`/`gt.shot`, NodeGraph crop). Create nodes with `CreateNodeArgsPropSettingsOpened=True`.
    - Sections, each written so the same script later checks the "after" state:
      - **wrap:** Blur nodes labelled 8, 30 and 80 characters (one 80-character name with no spaces); a Grade with a multi-line user label; a node with a plugin icon; a node with a preview on; a Backdrop and a Dot as controls. Record `Effect.getSize()` and the name item's scene rect, found through `gt.widgets_of("NodeGraph")` → `scene().items()` (a `QGraphicsTextItem` parented to the node). Checks: width ≤ the P2 maximum and the label rect inside the node rect. Both are expected to **fail** before the fix for the long names.
      - **labels:** properties panels of `net.sf.openfx.Premult`, `net.sf.openfx.Unpremult`, `net.sf.openfx.IDistort` and `net.sf.openfx.STMap`. Shot each panel. Check that each picker has a non-empty label `QLabel`, and that the U, V and Alpha field widgets share the same left x within 2 px.
      - **icons:** the left toolbox, the viewer toolbar plus timeline, and a Grade properties panel header. Take each at the default scale and again under `QT_SCALE_FACTOR=2` (a second run, driven by the environment).
      - **density:** the full main window with a Grade and a Merge panel open, the Preferences panel, and a right-click menu in the node graph.
    - Write `results.txt` with the binary's `git describe`.
  - verify: `NATRON_GUI_TEST_OUT=build/m56-shots/before Tests/gui/run-gui-test.sh Tests/gui/m56_visual.py build/fast/App/Natron` completes, plus the `QT_SCALE_FACTOR=2` run. Record which checks fail before the fix in M56's `## Decisions`; the expected failures are the wrap and labels checks.
  - size: M

## Phase 56.2: Node text layout

- [x] M56.P2.T1 — Grow a node horizontally up to a maximum width, then wrap its label
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, `Gui/GuiDefines.h`, `Gui/BackdropGui.h`
  - approach:
    - Today `resize()` never widens a node for its text. `adjustSizeToContent` (`NodeGui.cpp:1001`) ignores `w`, and `setNameItemHtml` (`:3334-3446`) only calls `_nameItem->adjustSize()`, so a long label overflows the silhouette sideways.
    - Add `NODE_MAX_WIDTH` to `GuiDefines.h` next to `NODE_WIDTH`/`NODE_HEIGHT` (`:45-46`), using the Q1 answer. If the answer is a preference, this task also touches `Engine/Settings.*`; re-scope it then.
    - Add `virtual bool wantsLabelWrap() const { return true; }` to `NodeGui`. `BackdropGui` overrides it to `false`, because it frames its name and the user sizes it. `DotGui` already can't resize.
    - In `adjustSizeToContent`, measure the natural label width (`setTextWidth(-1)`, then `document()->idealWidth()`). The content width is `min(natural + padding, TO_DPIX(NODE_MAX_WIDTH) - iconWidth)`. Widen `*w` to `max(*w, contentWidth + getPluginIconWidth())`. When `natural` is larger than the content width, call `_nameItem->setTextWidth(contentWidth)` so the centred `<div>` wraps; otherwise leave the text width at -1. The height then follows the wrapped `labelBbox.height() * 1.2` as it does today.
    - Never shrink a node below `getInitialSize`. Keep a saved or Python-set width that is larger than the content needs. Re-measure on every `setNameItemHtml` and on preview toggle (`getSizeWithPreview`).
    - `refreshPreviewAndLabelPosition` (`:1031-1082`) already centres using `labelBbox.width()`. Check that it still does once the width is set, including the preview and `mustFrameName` paths.
    - M55 edits `refreshEdges`/`connectEdge` in the same file. This task only touches the size and label functions; edges re-anchor through the existing `resize` path.
  - verify: at the gate build, P1.T1's wrap checks pass: width ≤ max, height grows, and the label rect sits inside the node rect for 30 and 80 characters (also with an icon and with a preview). Short names keep exactly today's size. The Backdrop and the Dot are unchanged. A project saved before the change loads with long-name nodes widened and wrapped, and nothing else moves.
  - size: M

## Phase 56.3: Knob label and alignment fixes

- [x] M56.P3.T1 — Stop hidden take-over choices from swallowing the row break, and label the unpremult-by twin
  - files: `Engine/OfxEffectInstance.cpp`
  - approach:
    - **Alignment:** in `takeOverMultiplaneChoices` (`:826`), each twin is inserted at the plugin choice's position, and the choice is then made secret. openfx-misc `Distortion.cpp:3642-3661` gives `channelU` a no-new-line layout hint (`eLayoutHintNoNewLine`). `KnobGuiContainerHelper::initializeKnobVector` (`Gui/KnobGuiContainerHelper.cpp:418-420`) puts the next knob on the previous sibling's row whenever that sibling has `!isNewLineActivated()`, even when the sibling is secret. So the V twin joins the hidden U choice's row, which is the indent. Fix it in the engine, not the GUI: when the choice is hidden, copy its `isNewLineActivated()` onto the twin, then `choice->setAddNewLine(true)`. Whether the twin keeps "no new line" is the Q2 call. Default: the U/V/A twins each get their own row, so `setAddNewLine(true)` on the twin as well for IDistort and STMap, which matches Nuke.
    - Apply the same rule to the plane twins (`kPlaneChoiceTakeOvers`, `:724-727`): Premult's `inputPlane` also has no-new-line (`Premult.cpp:929`).
    - **Label:** the channel twin copies `choice->getLabel()` (`~:931`). Premult's `unPremultByChannel` label is `""` (openfx-misc `SupportExt/ofxsMaskMix.h:39`). Add a small `{pluginID, paramName, label}` override table beside `kPlaneChoiceTakeOvers`. Use it when the plugin label is empty, with the Q2 text. Default: `net.sf.openfx.Premult` → "Premult by", `net.sf.openfx.Unpremult` → "Unpremult by".
    - Leave `Gui/KnobGuiContainerHelper.cpp` alone (M44 touches it).
  - verify: at the gate build, P1.T1's labels checks pass: the picker has a label, and the U, V and Alpha fields line up within 2 px on IDistort and STMap. Old projects still load the twins' values (only layout flags change, and the twins' persistence is untouched). ctest stays green, including the existing multiplane and OfxMultiplaneChoice cases.
  - size: M

## Phase 56.4: Themed SVG icon pipeline

- [x] M56.P4.T1 — Link Qt6::Svg and confirm the container and the bundle carry it
  - files: `CMakeLists.txt` (`QT_COMPONENTS`, `:69-71`), `Gui/CMakeLists.txt` (`target_link_libraries(NatronGui …)`, `:58`), `tools/release/stage-bundle.sh` (only if the check below finds a gap)
  - approach:
    - Add `Svg` to `QT_COMPONENTS` and `Qt::Svg` to NatronGui.
    - In the natron-dev container, confirm that `Qt6SvgConfig.cmake` and `libQt6Svg.so` exist. If they don't, stop and report: the Q7 fallback is pre-rasterised PNGs.
    - `stage-bundle.sh:172-179` already copies `imageformats` plugins and their dependencies. Check that `libQt6Svg.so.6` reaches the stage through the binary's own dependency copy. Only edit the script if it doesn't.
  - verify: at the gate build, configure finds Qt6::Svg, `ldd build/fast/App/Natron | grep Svg` resolves, and the P7.T2 AppImage passes `tools/release/check-relocatable.sh`.
  - size: M

- [x] M56.P4.T2 — Vendor the Lucide glyphs and the custom glyphs, and add the third-party art licence record
  - files: `Gui/Resources/Icons/lucide/*.svg` (new), `Gui/Resources/Icons/natron/*.svg` (new), `Gui/Resources/Icons/SOURCES.txt` (new), `tools/license/components/LICENSE-Lucide.txt` (new), `tools/license/components/README.md`, `Gui/GuiResources.qrc`
  - approach:
    - Take the upstream files unmodified from one pinned Lucide release (record the version and commit). Keep `stroke="currentColor"` and `stroke-width="2"`; the loader thins the stroke at load time, so the vendored files stay byte-identical to upstream.
    - The first batch is the toolbox set from the "Chosen" table: `image`, `brush`, `clock`, `menu`, `palette`, `pipette`, `layers-2`, `move`, `glasses`, `box`, `puzzle`. Add the Q10 picks for Toolsets/Misc, and M44's `chevrons-down` and `expand`. Later batches (P5) add their own glyphs to the same directory and manifest.
    - Under `Icons/natron/`, add `filter.svg` ("Filter v2" droplet with fins) and `deep.svg` ("Deep v3" D in a rounded square), taken from the page's "Chosen" markup (also in `PLAN/DESIGN/icons/`). Re-save them on Lucide's 24-unit grid with `currentColor`, round caps and joins, and `stroke-width="2"`, so they thin the same way. These are GPL-2.0-or-later.
    - `SOURCES.txt` has one line per glyph: file, origin (Lucide `<version>` / Feather-derived / Natron custom), and licence (ISC / MIT / GPL-2.0-or-later). `chevrons-down` is Feather-derived (MIT); `expand` is ISC.
    - `LICENSE-Lucide.txt` holds Lucide's ISC text and the Feather MIT notice. Add an alias for it in the qrc's root block next to the other `LICENSE-*.txt` (`GuiResources.qrc:4-…`), so About → Third-Party components lists it (`AboutWindow.cpp:492-529` lists every root `LICENSE-*`). Add a "Lucide icons (ISC; Feather-derived glyphs MIT)" line to `README.md`.
  - verify: `xmllint --noout` passes on every new SVG. Each qrc path exists (a `grep -o 'Resources/Icons/[^<]*' Gui/GuiResources.qrc | xargs ls` check). At the gate build, About → Third-Party lists `LICENSE-Lucide.txt`.
  - size: M

- [x] M56.P4.T3 — Render SVG icons recoloured from the theme in `getIcon`, and fix the scaled-cache bug
  - files: `Gui/GuiApplicationManager.cpp`, `Gui/GuiApplicationManager.h`, `Gui/ThemedIcon.h` (new), `Gui/ThemedIcon.cpp` (new)
  - approach:
    - `ThemedIcon::renderPixmap(path, logicalSize, QColor, dpr)`:
      - Read the bytes, replace `currentColor` with the colour's hex value, and replace `stroke-width="2"` with the stroke for this size. 1.5 at 24 and 18 px, per the Decision; make it a single constant.
      - Render with `QSvgRenderer` into a `QPixmap(size*dpr)` that starts transparent, then call `setDevicePixelRatio(dpr)`. Take the device pixel ratio from `qApp->devicePixelRatio()`.
    - In `GuiApplicationManager::getIcon(e, pix)` (`:86-…`), when `path` ends in `.svg`, render at the source's natural 24 px using the Settings text colour (`getTextColor`).
    - In `getIcon(e, size, pix)` (`:816-823`), when the path is an SVG, render directly at `size` instead of scaling a pixmap.
    - Fix the bug: the scaled result is stored under the unsized key `QString::number(e)`, so the `e@size` key is never filled. Insert under `e@size@colour@dpr`.
    - PNG ids keep today's path unchanged.
    - A theme colour change applies after a restart, the same contract the `useBwIcons` tooltip states today.
  - verify: covered by `Tests/ThemedIcon_Test.cpp` (P4.T4) and P7.T1's icon shots at 1× and 2×.
  - size: M

- [x] M56.P4.T4 — Build state icons from one SVG, and unit-test the themed renderer
  - files: `Gui/ThemedIcon.h`, `Gui/ThemedIcon.cpp`, `Tests/ThemedIcon_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Add `QIcon ThemedIcon::icon(path, logicalSize)`. It fills Normal/Off with the text colour, Normal/On with the selection colour (`getSelectionColor`), Disabled with the text colour at about 40% alpha over the base colour, and Active with the text colour (`addPixmap`, no custom `QIconEngine` unless `addPixmap` can't cover a case).
    - Pairs whose shape changes (play/pause, lock/unlock) keep two glyphs.
    - Test cases:
      - `currentColor` is substituted;
      - the stroke-width rewrite is applied;
      - a pixmap at dpr 2 has twice the device size and `devicePixelRatio()==2`;
      - the On and Off pixmaps differ;
      - the cache key includes the size, so the 24 px and 18 px requests differ.
  - verify: at the gate build, `ctest -R ThemedIcon` passes.
  - size: M

- [x] M56.P4.T5 — Point the toolbox grouping icons at the single SVG set
  - files: `Gui/GuiApplicationManager.cpp` (`*_GROUPING` cases `:278-321`, `iconSet` `:89-90`), `Gui/Gui20.cpp` (`PLUGIN_GROUP_DEFAULT_ICON_PATH` `:80`, toolbox icon build `:840-857`), `Global/Macros.h` (`NATRON_ICON_SET_NUMBER` `:266`), `Engine/AppManager.cpp` (RotoPaint registration `:1655`, the Set2 paint icon)
  - approach:
    - Map each grouping case to `Icons/lucide/<glyph>.svg` or `Icons/natron/{filter,deep}.svg`, with no set index: Image → `image`, Paint → `brush`, Time → `clock`, Channel → `menu`, Color → `palette`, Filter → custom, Keyer → `pipette`, Merge → `layers-2`, Transform → `move`, Multiview → `glasses`, Deep → custom, 3D → `box`, Other/Misc/Toolsets → per Q10.
    - Replace the default-group path and RotoPaint's icon path with the matching SVG paths. Delete `NATRON_ICON_SET_NUMBER` once nothing uses it.
    - In `Gui20.cpp:840-857`, build the toolbox and menu icons through `ThemedIcon::icon` when the path is an SVG (OFX plugin PNG icons are unchanged).
    - Toolbox tint per Q3. The default is monochrome. If the answer is to tint, colour each group with `Settings::getNodeCategoryColor(Node::categoryFromGroupingAndLabel(...))` instead of the text colour.
    - Keep `Global/Enums.h` untouched (no new ids).
  - verify: at the gate build, P1.T1's toolbox shot at 1× and 2× shows the new set, sharp, with no missing-image asserts in `natron.log`.
  - size: M

- [x] M56.P4.T6 — Remove the "Use black & white toolbutton icons" preference
  - files: `Engine/Settings.cpp`, `Engine/Settings.h`, `Gui/GuiApplicationManager.cpp`
  - approach: Per Q4 (default: remove), following M55.P1.T1's removal pattern:
    - delete `_useBWIcons` (`Settings.h:574`, `Settings.cpp:759-763`), its default (`:1613`), and `getIconsBlackAndWhite` (`Settings.h:237`, `Settings.cpp:3244-3247`);
    - drop the last use in `getIcon`.
    - `restoreKnobsFromSettings` only looks up keys for knobs that exist, so an old `useBwIcons` key is silently ignored.
    - M55 deletes `_hideOptionalInputsAutomatically` in adjacent lines (`:1616`, `:577`), so expect a trivial textual conflict and resolve it on top of M55.
  - verify: at the gate build, Preferences → Appearance → Main Window no longer shows the knob. A settings file with `useBwIcons=true` loads with no warning.
  - size: M

- [x] M56.P4.T7 — Move M44's colour-row icons onto the SVG path, and delete the grouping PNG sets
  - files: `Gui/GuiApplicationManager.cpp` (`NATRON_PIXMAP_COLOR_ADJUST_DROPDOWN` / `_GLOBAL_TRACKBALL` cases), `Gui/GuiResources.qrc`, `Gui/Resources/Images/colorAdjustDropdown.png`, `Gui/Resources/Images/colorGlobalTrackball.png`, `Gui/Resources/Images/GroupingIcons/**`
  - approach:
    - Point M44's two ids at `Icons/lucide/chevrons-down.svg` and `expand.svg`. M44's checked-state `mainstyle.qss` rule stays.
    - `git rm` the two PNGs and all of `GroupingIcons/Set1`–`Set3`, including the `.svg` and `_512.png` files in Set3. Remove their `<file>` lines (Set1 at `qrc:68-77` is already unused).
    - Before deleting anything, grep for `GroupingIcons` and both PNG names across `Engine Gui App Tests` to confirm no other references remain.
  - verify: that grep is empty. At the gate build, P1.T1's Grade panel shot shows the colour-row dropdown and global buttons recoloured, crisp at 2×, with the checked state still distinct.
  - size: M

- [x] M56.P4.T8 — Render SVG plugin icons themed on the node graph
  - files: `Gui/NodeGui.cpp`
  - approach: the optional on-canvas plugin icon (`isPluginIconActivatedOnNodeGraph`, around `NodeGui.cpp:700-760`) loads `Plugin::getIconFilePath()` with plain `QPixmap::load()`. RotoPaint's icon is now an SVG with a literal `currentColor` stroke, so it would draw untinted or wrong. When the path is an SVG (`ThemedIcon::isSvgPath`), draw it with `ThemedIcon::renderPixmap` at the icon size in the theme text colour; PNG plugin icons are unchanged.
  - verify: at the gate build, with "plugin icons in the node graph" on, a RotoPaint node shows a crisp themed brush glyph (P1.T1 shot).
  - size: M

## Phase 56.5: Icon replacement batches (scope per Q5)

- [x] M56.P5.T1 — Pitch the per-area glyph mapping for user approval
  - files: `PLAN/DESIGN/<date>-m56-icon-mapping.md` (new), a claude.ai artifact contact sheet (PM publishes it)
  - approach:
    - For each area in Q5's scope, list every `NATRON_PIXMAP_*` id with its current file (from `getIcon`'s switch, `GuiApplicationManager.cpp:86-809`) and its proposed Lucide glyph, or "custom needed".
    - Default scope (Q5 b): viewer toolbar and timeline (`ViewerTab.cpp:223-431` uses) and properties-panel chrome (`DockablePanel.cpp`: settings, help, minimise/float/close, lock, hide-unmodified, undo/redo, centre).
    - Render today's icon next to the proposed glyph at 18 px and 2× on `#303030`, in the Normal, On and Disabled states.
    - Mark paired `*Enabled`/`*Disabled` and `*_on`/`*_off` ids that collapse to one glyph through `ThemedIcon::icon`.
  - verify: the user approves the sheet. Record the approved mapping in M56's `## Decisions`; P5.T2 and P5.T3 apply exactly that.
  - size: M

- [x] M56.P5.T2 — Apply the approved viewer-toolbar and timeline glyphs
  - files: `Gui/GuiApplicationManager.cpp`, `Gui/ViewerTab.cpp` (only where a pair collapses into one `ThemedIcon::icon`), `Gui/Resources/Icons/lucide/*.svg`, `Gui/Resources/Icons/SOURCES.txt`, `Gui/GuiResources.qrc`
  - approach: Vendor the approved glyphs (and record them in the manifest), point each id at its SVG, and replace `addPixmap(on)/addPixmap(off)` pairs with the themed QIcon where the mapping says so. No new ids.
  - verify: at the gate build, P1.T1's viewer shots at 1× and 2× show the approved glyphs, and the RoI, clip-to-project, auto-contrast and pause-render toggles each show distinct On and Off states.
  - size: M

- [x] M56.P5.T3 — Apply the approved properties-panel glyphs
  - files: `Gui/GuiApplicationManager.cpp`, `Gui/DockablePanel.cpp`, `Gui/Resources/Icons/lucide/*.svg`, `Gui/Resources/Icons/SOURCES.txt`, `Gui/GuiResources.qrc`
  - approach: Same as P5.T2, for the panel header and the knob animation, reset and link buttons.
  - verify: at the gate build, P1.T1's Grade panel shots at 1× and 2× show the approved glyphs.
  - size: M

- [x] M56.P5.T4 — Send the path-based roto, tracker and dope-sheet icons through the themed loader
  - files: `Gui/KnobGuiButton.cpp` (`:99` icon load), `Gui/DopeSheetView.cpp`, `Engine/RotoPaint.cpp`, `Engine/TrackerNode.cpp`, `Engine/TrackerContextPrivate.cpp` (path strings only)
  - approach: `KnobGuiButton`/`DopeSheetView` call `ThemedIcon` when the path is an SVG. Engine keeps passing `:/Resources/...` strings, and only the file names change. The custom glyphs (roto point tools, tracker controls, interpolation) need their own approval sheet first, like P5.T1.
  - verify: at the gate build, shots of the roto and tracker toolbars and the curve-editor interpolation menu.
  - size: L

- [x] M56.P5.T6 — Vendor every approved glyph before the area tasks apply them
  - files: `Gui/Resources/Icons/lucide/*.svg`, `Gui/Resources/Icons/natron/*.svg`, `Gui/Resources/Icons/SOURCES.txt`, `Gui/GuiResources.qrc`
  - approach: from `PLAN/DESIGN/2026-10-11-m56-icon-mapping.md` (approved), fetch the 84 Lucide glyphs byte-identical from tag 1.55.0 (commit `70562c1ee1c4`) and write the 51 custom glyphs verbatim from the doc's SVG-source section (convention A for interpolation; dope-sheet markers stay small shapes). Record each in `SOURCES.txt` (Feather-derived glyphs are MIT; check Lucide's LICENSE list) and add qrc entries. Run before P5.T2, so the area tasks only re-point ids.
  - verify: `xmllint --noout` on every new SVG; every qrc path exists; at the gate build, P1.T1's shots show no blank icon (the compact-arc-flag glyphs `eye`, `eye-off`, `droplet`, `eraser`, `diamond-plus`, `diamond-minus`, `feather`, `pen-tool`, `pin`, `zap` included; re-save any that Qt6::Svg can't draw, and note it in SOURCES.txt).
  - size: M

- [x] M56.P5.T7 — Apply the approved glyphs to panes, the properties bin, the Script Editor, the file dialog and the link cursors
  - files: `Gui/GuiApplicationManager.cpp`, `Gui/TabWidget.cpp`, `Gui/PropertiesBinWrapper.cpp` (or wherever the bin's buttons are built), `Gui/ScriptEditor.cpp`, `Gui/SequenceFileDialog.cpp`, and the cursor construction site (grep `NATRON_PIXMAP_LINK_CURSOR`)
  - approach: point each id at its SVG per the mapping's "Panes, script editor and dialogs" section. The bin's minimise/maximise-all button loads Lucide `chevrons-down-up` through `ThemedIcon::icon` by path. The link cursors keep their black halo and use the hotspot the mapping gives; build them from a rendered pixmap with `QCursor(pixmap, hotX, hotY)`. Fix the Script Editor's close icon being overwritten by the clear-history icon only if the mapping says the close button should show it.
  - verify: at the gate build, P1.T1 shots of a split pane header, the properties bin buttons, the Script Editor toolbar and the file dialog; the link cursor's hotspot lands on the clicked knob in the GUI script.
  - size: M

- [x] M56.P5.T5 — Delete orphaned PNGs and their qrc entries
  - files: `Gui/GuiResources.qrc`, `Gui/Resources/Images/*.png` (only the unreferenced ones)
  - approach:
    - For each `<file>` under `Resources/Images/`, grep its basename across `Engine Gui App Global Tests` and `Gui/Resources/Stylesheets/mainstyle.qss`.
    - `git rm` only the files with zero hits, and drop their lines.
    - Leave `natronIcon*`, `splashscreen*`, `NativeNodes/`, `Other/` and every file the stylesheet references alone.
    - Put the deleted list in the commit message.
  - verify: the grep re-run shows each kept file referenced and each deleted file unreferenced. At the gate build, no `Missing image` assert appears in `natron.log` across the P1.T1 run.
  - size: M

## Phase 56.6: Stylesheet density (direction per Q8)

- [x] M56.P6.T1 — Pitch a spacing and density spec as before/after mockups
  - files: `PLAN/DESIGN/<date>-m56-density-spec.md` (new), a claude.ai artifact (PM publishes it)
  - approach:
    - From P1.T1's density shots, catalogue the spacing sources in both places:
      - `mainstyle.qss`: 792 lines and 27 padding/margin rules, covering tab bars `:469-515`, `QMenu` `:548-560`, `QMenuBar` `:735-750`, buttons, line edits and combos;
      - C++ `setSpacing`/`setContentsMargins`: `DockablePanel.cpp` 9 uses, `KnobGuiContainerHelper.cpp` 5, `TabWidget.cpp` 5, `ViewerTab.cpp` 12.
    - Propose a small token set (row gap, group indent, header height, tab padding, menu item padding) with values. Mock the node-graph tab area, a Grade properties panel, the viewer toolbar and a context menu before and after.
    - Colours stay fixed unless Q8 = b.
  - verify: the user approves a spec. Record the token values in M56's `## Decisions`; P6.T2–T4 apply exactly those.
  - size: M

- [x] M56.P6.T2 — Apply the spec in the stylesheet, including the menus
  - files: `Gui/Resources/Stylesheets/mainstyle.qss`, `Gui/Gui20.cpp` (`loadStyleSheet` prelude `:268-290`, only if a token needs a new `%n` substitution)
  - approach:
    - Change padding and margins only. Keep the `%1`–`%11` colour placeholders and their order.
    - Bring the `QMenu`/`QMenuBar` item padding, separator and icon column to the spec.
    - Keep M44's checked-button rule.
  - verify: at the gate build, P1.T1's density shots match the approved mockups. A user stylesheet set in Preferences still loads.
  - size: M

- [x] M56.P6.T3 — Apply the spec to the properties panel layout
  - files: `Gui/DockablePanel.cpp`, `Gui/DockablePanelPrivate.cpp`, `Gui/KnobGuiContainerHelper.cpp`
  - approach: Replace the hard-coded spacing and margins with the spec's values (as `GuiDefines.h`-style constants with `TO_DPIX`, kept local to these files). `KnobGuiContainerHelper.cpp` carries M44 edits; change only the margin and spacing literals.
  - verify: at the gate build, the Grade and Merge panel shots match the mockup, and P1.T1's labels alignment check still passes.
  - size: M

- [x] M56.P6.T4 — Apply the spec to the viewer toolbar, tab widgets and toolbox
  - files: `Gui/ViewerTab.cpp`, `Gui/TabWidget.cpp`, `Gui/Gui20.cpp` (toolbox layout), `Gui/ToolButton.cpp`
  - approach: Same as P6.T3.
  - verify: at the gate build, the viewer, tab and toolbox shots match the mockup at 1× and 2×.
  - size: M

## Phase 56.7: Evidence and UAT

- [x] M56.P7.T1 — End build: run every suite and the M56 script, and take the "after" shots
  - files: `Tests/gui/m56_visual.py` (fixes only), `PLAN/MILESTONES/M56-polish-visual-and-menus.md` (gate results; the executor edits this)
  - approach:
    - Run one `tools/ci/local/build.sh fast`, then ctest.
    - Run `m56_visual.py` at 1× and `QT_SCALE_FACTOR=2`, plus `m55_input_pipes.py`, `m55_node_graph.py` and `m24_border.py` as regression checks.
    - Fix product bugs found here in small follow-up commits, and record them in `## Decisions`.
  - verify: ctest is green, including `ThemedIcon`. `m56_visual.py` reports 0 FAIL in both runs, and the other scripts report 0 FAIL. Show the before and after shots side by side to the user.
  - size: L

- [x] M56.P7.T2 — Package the AppImage for the user checkpoint
  - files: `build/appimages/M56-<sha>.AppImage`, `build/appimages/M56-uat.md`
  - approach: `tools/ci/local/package.sh fast` (M24's `--no-appstream` wrapper note applies). The UAT script walks through:
    - renaming a node to a long name and watching it widen, then wrap;
    - an old project with long names;
    - Premult/Unpremult labels and IDistort/STMap alignment;
    - the toolbox, viewer and panel icons at the user's real scale;
    - the Preferences text colour change, then a restart, recolouring the icons;
    - About → Third-Party → Lucide;
    - general density in the user's own project.
  - verify: the user signs off on the AppImage.
  - size: M

## Phase 56.9: UAT round 3 fixes (2026-10-12)

- [x] M56.P9.T1 — Strip the platform icons from the standard buttons of Natron's message boxes
  - files: `Gui/Gui30.cpp`, `Gui/MessageBox.cpp`
  - approach:
    - The "Save changes to %1?" dialog (`Gui::saveWarning`, `Gui/Gui20.cpp:1334`) reaches `Gui::onDoDialog` (`Gui/Gui30.cpp`, question branch ~line 397). That builds a `QMessageBox` whose main glyph is already themed by `Gui::setQMessageBoxThemedIcon` (`Gui30.cpp:295`).
    - Its standard buttons (Save, Discard, Cancel, Ok, Yes, No…) still get the style's `SP_Dialog*Button` icons.
    - Next to `setQMessageBoxThemedIcon`'s call sites (lines ~369, 376, 384, 403), loop over `widget->buttons()` and call `setIcon(QIcon())`, so no standard button carries an icon. User decision: no icons on any button.
    - Do the same for Natron's own `MessageBox` (`Gui/MessageBox.cpp`), after its `DialogButtonBox` is built (~line 154).
    - Leave the main glyph alone.
  - verify: at the end build, change a project and close it. The Save, Discard and Cancel buttons show text only, and so do an info dialog and a question dialog.
  - size: M
- [x] M56.P9.T2 — Re-derive the curve editor's colours from the theme greys
  - files: `Engine/Settings.cpp`, `Gui/CurveWidgetPrivate.cpp`, `Gui/CurveWidgetPrivate.h`, `Gui/CurveWidget.cpp`, `Gui/CurveGui.cpp`
  - approach:
    - Change the curve-editor defaults (`Settings.cpp:1698-1707`) to the node graph's greys: background the sunken grey (0.12), grid a low-contrast grey (~0.30–0.35), scale text the alt-text grey (0.6).
    - User decision: a clean break, with no migration, no appearance-version bump and no prompt.
    - In the paint code, replace the fixed colours:
      - the selected-curve yellow `_selectedCurveColor` (`CurveWidgetPrivate.cpp:79`) → `Settings::getSelectionColor` (the theme orange, user decision);
      - the fixed `glColor4f` greys at `CurveWidgetPrivate.cpp:309`, `:323` and `:587` → the text and selection colours at suitable alpha;
      - `QColor(240,240,240)` in `CurveGui.cpp:530-544` → the theme text colour (`ThemedIcon::themeTextColor()` or `getTextColor`).
    - Keep the per-curve hue rotation and the white default, so channels stay distinguishable.
    - Read the settings on the main thread only.
    - Leave the dope sheet unchanged.
  - verify: `grep -n "glColor4f(0\.\|QColor(2" Gui/CurveWidget*.cpp Gui/CurveGui.cpp` finds no fixed greys left. At the end build, screenshot the curve editor with a Grade's curves next to the node graph at 1× and 2× for the user.
  - size: M
- [x] M56.P9.T3 — End build, regression scripts, and the UAT AppImage
  - files: `build/appimages/M56-<sha>.AppImage`, `build/appimages/M56-uat.md`
  - approach: rebase onto `main` (M45 merges first). Run one `tools/ci/local/build.sh fast`, ctest, `m56_visual.py` at 1× and 2× and the gate's regression scripts, then `tools/ci/local/package.sh fast`. The UAT script covers the button-free dialogs and the curve-editor screenshot.
  - verify: ctest and the scripts are green, and the user signs off on the AppImage.
  - size: L

**Verification gate:**
- One `fast` build at the end, with CI `format`, `lint-ci` and `build-and-test` green.
- ctest green, including the new `ThemedIcon` case.
- Under `Tests/gui/run-gui-test.sh` against the milestone build, `m56_visual.py` passes every check at 1× and `QT_SCALE_FACTOR=2` (after failing on the pre-fix binary for the cases recorded in P1.T1). `m55_input_pipes.py`, `m55_node_graph.py` and `m24_border.py` report 0 failures.
- The user approves the before/after screenshots for wrap, labels, icons and density.
- About → Third-Party components lists the Lucide licence, and `Gui/Resources/Icons/SOURCES.txt` covers every vendored and custom glyph, including M44's two.
- The user signs off on `build/appimages/M56-<sha>.AppImage` through the UAT script.

## Decisions

- 2026-10-10 — **Icon direction (user):** a monochrome SVG line-icon set under a GPL-compatible licence, recoloured by the theme, with the three GroupingIcons sets reduced to one. The user wants pitched options before any before/after work starts.
- 2026-10-10 — **Icon set picks (user, from the icon-options page):** Lucide is the set, drawn at a 1.5 px stroke (native 2 px is too heavy). Overrides for the toolbox grouping icons: Filter should look like Tabler's or Phosphor's filter glyph, drawn in-house in Lucide's style if Lucide has nothing suitable; Channel is a "hamburger" (three stacked horizontal lines); Merge uses the layers icon the page proposed for Deep; Deep gets a custom "D" glyph in Lucide's style. The rest of the page's questions (toolbox tint, B&W setting, NativeNodes/merge-operator scope, licence route) are still open; Lucide's ISC licence keeps GPL-2.0-or-later.
- 2026-10-10 — **Toolbox glyph selects (user):** Filter is the custom droplet-with-fins glyph ("Filter v2", after Phosphor's `drop-half`); Deep is the custom D inside a rounded square ("Deep v3", Lucide's `square-*` convention); Keyer is Lucide `pipette` rather than `key-round`, since a key reads as keyframes while a keyer picks a colour. Channel is Lucide `menu` and Merge is Lucide `layers-2`. All custom glyphs use Lucide's 24 px grid, round caps and joins, at the 1.5 px stroke. The drafts' SVG markup is in the icon-options page (claude.ai artifact `9B52JMKTyWw5Ufep99afxr`, "Chosen" section).
- 2026-10-11 — **User answers to the elaboration questions** (`PLAN/DESIGN/2026-10-11-m56-questions.md`):
  - **Max node width:** start at 2.5× the default node width (200 logical px before DPI scaling) as a constant; the user expects to tune it after seeing it.
  - **Toolbox tint:** monochrome in the theme text colour.
  - **Icon scope:** everything, so P5.T4 (roto, tracker, curve editor, dope sheet, with custom glyphs) is in. The 33 native-node and 34 merge-operator icons stay out, for a follow-up stub.
  - **Stylesheet:** spacing, padding and grouping only, colours unchanged, Nuke as the reference look; mockups first (P6.T1).
  - **B&W icons preference:** removed; the old key is silently ignored.
  - **Licence:** stay GPL-2.0-or-later with MIT/ISC sets only (Lucide pinned to its latest release at implementation time); custom glyphs GPL-2.0-or-later.
  - **Rendering:** runtime SVG through Qt6::Svg, recoloured from the Preferences text colour.
  - **Menus:** styling only (padding, separators, icon column) inside P6.T2.
  - **Grouping icons:** Toolsets → Lucide `wrench`. Misc and Other: the user wants options; P5.T1's mapping sheet shows candidates for both.
  - **Size:** keep M56 whole.
  - **Twin labels / U-V-A layout:** the user asked for pictures of each option before choosing; pending.
- 2026-10-11 — **Twin labels (user, from mockups at claude.ai artifact `5Fw2Nn1B4F7tmtjqF3WA4n`):** Premult/Unpremult picker on its own row labelled "Premult by" / "Unpremult by" (A1). IDistort/STMap: U, V and Alpha each on their own aligned row (B1), so P3.T1 stands as written. The user briefly floated a single layer row capped at two channels (first enabled = U, second = V, as in Nuke), and a consultant designed it (`KnobLayerSelect::setMaxChannels`, one `hostUVChannels` twin). The user then dropped that path in favour of B1, so it is not planned.
- 2026-10-11 — **Started early (user):** M56 opened in `.worktrees/m56` on `milestone/m56-visual-and-menu-polish`, branched from `main` at `a50bef552`, while M44 - Trackball Colour Editing (PR #51, in CI) and M55 - Node Graph Polish (at its gate) are still open. The user asked for it explicitly. The lane rebases onto `main` after each of those merges. Tasks that build on M44's icons (P4.T7) wait until M44 has merged. P1.T1's "before" shots run later against a `main` build. **Freshness check:** every task's files exist on `main`. P4.T6 and P5.T5 were re-sized S → M under the sizing rule: they are multi-file edits that need grep judgement.
- 2026-10-11 — **P4.T2:** Lucide pinned at 1.55.0 (commit `70562c1ee1c4`). Lucide's LICENSE lists `clock` and `move` as Feather-derived (MIT) as well as `chevrons-down`; SOURCES.txt records all three as MIT. The custom glyphs came from `PLAN/DESIGN/icons/` and were re-saved at stroke-width 2 so the runtime thinning treats them like Lucide.
- 2026-10-11 — **P4.T5:** toolbox groupings map to SVGs (Misc, Other and the default catch-all all `puzzle` for now). A new public `GuiApplicationManager::getIcon(PixmapEnum, int, QIcon*)` overload builds themed state icons. The implementer found that `NodeGui`'s on-canvas plugin icon loads SVG paths with plain `QPixmap::load`, so **M56.P4.T8** was added to theme it.
- 2026-10-11 — **Icon mapping approved (user)** after six comment rounds on claude.ai artifact `3iXFWNHqn6JCTVH3HF22kN`. It has 149 rows: 84 Lucide glyphs and 51 custom glyphs, with the SVG source for the custom glyphs in `PLAN/DESIGN/2026-10-11-m56-icon-mapping.md`. Interpolation uses **convention A** (Maya-style: one key dot and a curve whose shape is the type). The dope-sheet keyframe markers stay small shapes. Other → `circle-ellipsis`. The scope widened to panes, the bin, the Script Editor, the file dialog and the cursors, which became new task **P5.T7**. Vendoring all glyphs up front became new task **P5.T6**. P5.T4 is no longer conditional.
  - **Free interpolation:** add a Free entry to the interpolation menu (user). This goes in P5.T4: the curve editor's and dope sheet's interpolation menus gain "Free" using `NATRON_PIXMAP_INTERP_CURVE`.
  - **Bin minimise/maximise-all:** uses `chevrons-down-up` by path. **Link cursors** keep their black halo.
  - **Order in Phase 56.5:** P5.T6, then T2, T3, T4, T7, then T5.
- 2026-10-11 — **Density spec v2** published (claude.ai artifact `QTNUQASqsHJscpeR5JcvHa`): overlays off by default. Per the user's question, layout spacing goes through one `Gui/GuiDensity.h` header of named, DPI-scaled constants, which replaces P6.T3's "constants kept local". Optional Q_PROPERTY theming comes later. Awaiting the user's approval of the token values.
- 2026-10-11 — **Density spec approved (user)** as v2: the token values in `PLAN/DESIGN/2026-10-11-m56-density-spec.md`, applied through a new `Gui/GuiDensity.h` of DPI-scaled constants. P6.T2–T4 apply exactly those values. P6.T4's toolbox file is `Gui/Gui05.cpp` (not `Gui20.cpp`/`ToolButton.cpp`), per the spec.
- 2026-10-11 — **Misc toolbox icon → Lucide `shapes` (user).** It gets vendored and re-pointed after P5.T2, which is editing `GuiApplicationManager.cpp`. Other stays `circle-ellipsis`, and the default catch-all stays `puzzle`.
- 2026-10-11 — **P5.T4:** check at the gate: the dope-sheet texture orientation was flipped (R, Z and node-root must read upright), and the upload changed from GL_RGBA to GL_BGRA. Free interpolation gets a shortcut ID with no default key. KnobGuiContainerHelper's pattern-size and search-size label icons now render themed.
- 2026-10-11 — **P5.T5:** 178 orphaned PNGs deleted, 128 remain. Found an older bug: `GuiApplicationManager.cpp` asks for `merge_hard_light.png`/`merge_soft_light.png`, but the files are `merge_hardlight.png`/`merge_softlight.png`, so those two Merge-operator icons never load. It is fixed alongside P6.T2.
- 2026-10-11 — **P1.T1 "before" run** against the M55 build, which has none of M56. 28 checks fail at both 1× and 2×, every one of them expected: wrap 5, labels 6, icons 11, density 6. The shots are in `build/m56-shots/before{,-2x}/`. For the after run, pass `M56_BASELINE_DIR` so the icon-hash comparison runs. Do not call `parentItem()` on node-graph items from Python, because PySide deletes the item and Natron crashes.
- 2026-10-11 — **P7.T1 (end build):** the first build in the worktree was green, with no compile errors. ctest passes 1632/1632, once `build/assets` was symlinked into the worktree so the CImg reference tests compile (a worktree has no assets of its own). `m56_visual.py` passes 150/0 at 1× and at 2×; `m24_border.py` passes 47/0. `m55_input_pipes.py` fails 3 checks (Mask stubs auto-hiding) because M55 isn't merged yet; this clears on rebase. One product bug was found and fixed in `875df5bd3`: a node with a preview and a wrapped label drew the label below the node. Visual checks pass: the dope-sheet markers are upright with no red/blue swap, `feather` and `pen-tool` arcs render in Qt 6.8.3 (no re-save needed), the opacity glyphs show their fade, and the brush reads at 18 px. At 2× the link-cursor hotspot is about 1 logical px off, because Qt's X11 backend doesn't scale the hotspot; this is within tolerance. The after shots are in `.worktrees/m56/build/m56-shots/after{,-2x}/`. The before/after review with the user comes next.
- 2026-10-11 — **P7.T2 AppImage:** `build/appimages/M56-875df5bd3.AppImage` with `build/appimages/M56-uat.md`. It launches; `check-relocatable.sh` on the extracted bundle finds 266 binaries that all resolve, `libQt6Svg.so.6` included. Waiting on the user's shot approval (claude.ai artifact `6y7CdSBwvuiThopTMEmBYJ`) and UAT.
- 2026-10-12 — **Shot review round 1 (user):** eight fixes, listed in P8.T1. **Deferred:** IDistort offers only RGBA where it should have a layer dropdown. The user asked to defer this to a later milestone, so it is added to M69 - Minor Cleanup.
- 2026-10-12 — **Review round 2 (user):** the node label block is centred again, with its lines left-aligned and an 8 px hanging indent. The channel row's "[-]" button moves to the end of the row, so the layer menu and "+" align with the field column (`2deb32ca3`). The branch was rebased onto `main` with M55 (clean). Gate on `2deb32ca3`: `m56_visual.py` 202/0 at 1× and at 2×; `m55_node_graph.py` 162/0, `m55_input_pipes.py` 118/0 and `m24_border.py` 47/0 (on `4b534b725`); full ctest 1654/1655. The one failure, `BlurKernels.CImgReferenceAvailable`, was a stale ccache object compiled before `build/assets` was linked into the worktree; recompiled, the CImg tests pass 10/10. UAT AppImage: `build/appimages/M56-2deb32ca3.AppImage`.
- 2026-10-12 — **UAT round 1 (user) on `M56-2deb32ca3.AppImage`, not yet passed.** Findings (screenshot `.local/Screenshot_20261012_094720.png`):
  - The icons in the pane header and properties-bin buttons are tiny.
  - The bin's "2" max-panels box is offset and crowded.
  - The time-display format dropdown is too small for its text.
  - Several Preferences items show the wrong text colour.
  - Remove the Windows-only "use drive letters" preference.
  - Some icons look white instead of the text colour (about 0.78 linear).
  - Changing the text colour is very slow, and it doesn't recolour the icons; the user wants that to happen live.
  - Legacy icons remain, e.g. in the save dialog; do another sweep.

  All of these are being fixed together; the build waits for M69's cold build.
- 2026-10-12 — **UAT round 1 fixes** (`9227c65b2` before rebase), all nine items:
  - **Rendering and recolouring:** a new `ThemedIcon` icon engine renders the SVG at the requested size and reads the theme colours when it paints. That fixes the tiny header icons and makes recolouring live, through a new `GuiApplicationManager::themeChanged`.
  - **Speed:** colour changes are debounced into one stylesheet reload.
  - **White icons:** these were icons that bypassed theming (desktop-theme File-menu icons, Qt standard icons in the file dialog and message boxes). They are now themed glyphs. There was no linear/sRGB mismatch.
  - **Dark disabled text:** a new `%12` stylesheet colour (the text colour blended into the base) replaces black.
  - **Combo sizing:** combos size to their text, including the time-format dropdown.
  - **Panel-count box:** the bin's "2" box is sized and centred.
  - **Drive letters:** the drive-letters preference is removed.
  - **New Lucide glyphs:** 15 more were vendored (File menu, file dialog, messages), and the colour-picker cursor now uses `pipette`.
  - **Rebase:** onto `main` with M84, with no conflicts.
  - **Needs the gate build:** these have only been compiled object by object so far. The build waits for M69's end-build agent.
- 2026-10-12 — **The 21 new UAT-fix icons are approved (user)** (claude.ai artifact `VL8xyza9LEeqzorFzdNJvf`). The one exception is Reload Project, which uses the standard `refresh-cw` instead of `folder-sync` (`c12e54587`). `folder-sync` was removed.
- 2026-10-12 — **UAT fixes verified:** live recolouring now reaches labels. The viewer info bar had hard-coded its text colour (`e8c57b03b`). `m56_visual.py` 272/0 at 1× and 2×; ctest 2042/2042. The Vulkan loader is bundled (decision `2026-10-12-bundle-vulkan-loader`), and the round-2 AppImage is being repackaged.
- 2026-10-12 — **UAT round 3 (user, `.local/uat-feedback-2026-10-12.md`):** the save/discard/cancel dialog still has old-style icons, and the curve editor's colours should match the M56 look. The curve-editor item came from the UAT's "possibly not for one of these milestones" list, and the user chose to fold it in here. User answers: no icons on any standard button; the curve editor uses the node graph's greys, with the theme orange for the selected curve; a clean break for the defaults, with no migration or prompt. These became Phase 56.9.
- 2026-10-13 — **P9.T3 end build green, awaiting UAT sign-off:** `d6a42d2f8`, a full `fast` build. ctest 2054 tests: 2 GPU tests disabled and 3 failures, all environmental (2 CImg tests whose `build/assets` symlink was unreachable from the container, fixed with a real copy; 1 known shutdown segfault that passes on re-run). `m56_visual.py` 263/0 at 1× and 2×; `m55_input_pipes.py` 118/0; `m55_node_graph.py` 159/0 (one flaky first pass); `m45_tab_menu.py` 97/0; `m24_border.py` 47/0. The AppImage is `build/appimages/M56-d6a42d2f8.AppImage`, with `M56-uat.md` rewritten; screenshots are in `.worktrees/m56/build/m56-shots/uat3/`. Not M56's: at 2× a click on a curve doesn't select it, so hit-testing likely ignores the device pixel ratio (inbox).
- 2026-10-13 — **Shipped:** PR #59 squash-merged as `1fabf3a01`. The Codex review gave 3 findings, all fixed in `a25a3528d`: the lock-icon DPR, the tab-group grid spacing and a narrating comment. M56 was rebased onto M93 before CI; `build-and-test`, `format` and `lint-ci` were green.
