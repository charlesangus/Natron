# M56 icon direction: options

2026-10-10. The user chose a monochrome SVG line-icon set under a GPL-compatible licence, recoloured by the theme, with the three GroupingIcons sets cut to one. This note sets out the options to pick from. The visual pitch, which puts 37 concepts × 5 sets beside today's icons at 1x and 2x on Natron's `#303030`, is `m56-icon-pitch.html` in the session scratchpad (`/tmp/claude-1000/-home-bosley-git-Natron/bedb5452-a463-428c-b1b0-c13c24f8028f/scratchpad/`).

## Audit

| Item | Value |
|---|---|
| `Gui/Resources/Images/**` | 419 files: 361 PNG, 55 SVG, plus icns/pdf/txt. 314 at the top level, 33 in `NativeNodes/`, 2 in `Other/`, and 10/15/45 in `GroupingIcons/Set1`–`Set3` |
| `Gui/GuiResources.qrc` | 411 `<file>` entries. All of Set1 (lines 68–77) is listed but no code references it |
| Pixmap ids | 229 `NATRON_PIXMAP_*` in `Global/Enums.h`, mapped to paths in the switch in `GuiApplicationManager::getIcon()` (`Gui/GuiApplicationManager.cpp:86-809`), cached with `QPixmapCache` and loaded by `QPixmap::load` |
| Path-based loads | About 145 `NATRON_IMAGES_PATH` uses outside `getIcon`: `Engine/RotoPaint.cpp` 48, `Engine/AppManager.cpp` 48 (built-in node icons), `Engine/TrackerNode.cpp` 25, `Gui/DopeSheetView.cpp` 18, and a few others. The roto and tracker paths reach `KnobButton::setIconLabel`, which `Gui/KnobGuiButton.cpp:99` loads |
| Source size | Most PNGs are 32×32 (grouping icons 32×28; a few 41, 128 or 22) and are scaled down with `Qt::SmoothTransformation`. That's why they look soft |
| Display sizes (`Gui/GuiDefines.h`) | Toolbox `NATRON_TOOL_BUTTON_ICON_SIZE` = 24 (`Gui20.cpp:840-856`); `NATRON_MEDIUM_BUTTON_ICON_SIZE` = 18 (73 uses: viewer toolbar `ViewerTab.cpp:223-431`, properties panel, script editor, dope sheet); `NATRON_SMALL_BUTTON_ICON_SIZE` = 11 (10 uses). All are multiplied by `TO_DPIX` |
| Grouping sets | `getIconsBlackAndWhite()` (setting `useBwIcons`, `Engine/Settings.cpp:759`) picks Set2 (on) or Set3 (default). The group-to-icon mapping is `getPixmapForGrouping` in `Gui/Gui20.cpp:105` |
| SVG support | Qt6::Svg isn't linked (`CMakeLists.txt:69-71`: Core Gui Network Widgets Concurrent OpenGLWidgets). Linking it gives you `QSvgRenderer`: replace `currentColor` (and `stroke-width` if needed) in the bytes, then render to a pixmap of size × devicePixelRatio. A `QIconEngine` subclass can produce the Normal, Disabled, Active and Selected looks, each On or Off, from one file |
| Bug | `getIcon(e, size, pix)` at `GuiApplicationManager.cpp:820` stores the scaled pixmap under the unsized key `QString::number(e)`. That overwrites the full-size entry, and the `e@size` key is never filled |
| Theme greys (Settings defaults) | base 0.19 (`#303030`), sunken 0.12 (`#1f1f1f`), raised 0.28 (`#474747`), text 0.78 (`#c7c7c7`), selection (0.95, 0.54, 0) (`#f28a00`) |

## Options

| Set | Licence | GPL-2.0-or-later | Count | Grid / style | Compositing coverage | Verdict |
|---|---|---|---|---|---|---|
| **Tabler Icons** | MIT | Compatible | 5,184 outline (+ filled variants) | 24 px, 2 px round stroke | Best: keyframe family, `vector-bezier`, `layers-intersect/subtract/union`, `blur`, `color-picker`, `transform`, `crosshair`, a full `player-*` set | **Recommended** |
| Lucide | ISC (Feather parts MIT) | Compatible | 1,877 | 24 px, 2 px round stroke | Most consistent drawing, good viewer and transport icons, `blend`, `pen-tool`, `pipette`. No blur, keyframe, checkerboard or deep icon | Runner-up |
| Phosphor | MIT | Compatible | 1,512 × 6 weights | 256 unit, filled-outline paths | Has `checkerboard`, `bezier-curve`, `intersect`, `eyedropper`. Stroke can't be tuned per size; soft, rounded look | Viable |
| Material Symbols | Apache-2.0 | Not compatible with GPLv2; compatible with GPLv3, so usable only by distributing Natron under GPLv3 | ~3,900 (7,858 files incl. fills) | 960 unit, filled shapes | Broad: `blur_on`, `contrast`, `fit_screen`, `colorize` | GPL-3 route only |
| Iconoir | MIT | Compatible | 1,383 | 24 px, 1.5 px stroke | Has `color-wheel` and `design-nib`, but no layers/stack or contrast icon. A 1.5 px stroke renders at about 1.1 px at 18 px, too faint | Weak at 18 px |
| Remix Icon | Remix Icon License v1.0 (Jan 2026) | Incompatible: the ban on standalone redistribution is an extra restriction | — | — | — | Ruled out |
| Heroicons | MIT | Compatible | 324 | 24 px, 1.5 px | Too few glyphs; almost no media icons | Ruled out |
| Blender icons | CC-BY-SA 4.0 | GPLv3 only (one-way) | — | Multi-colour pixel art | Not a monochrome line set | Ruled out |
| Breeze (KDE, Kdenlive) | LGPL-3.0-or-later | GPLv3 route only | — | Coloured, 16/22/32 grids | KDE-styled | Ruled out |

**No set covers these; we'd draw them on the same 24/2 grid:** Deep (depth-sample stack), multi-view/stereo (eyeglasses is only close), the 34 merge-operator icons, curve interpolation (constant/linear/smooth/catmull/cubic/horizontal/break), roto point tools (feather, cusp/smooth, open/close curve, select points/feather/curves), tracker controls (track fwd/bwd/range/keyframe), viewer gain/gamma/zebra, and possibly the checkerboard (Tabler only has a grid). The 33 `NativeNodes/` node icons need their own scope decision.

## Recommendation

Use **Tabler Icons outline** (MIT). It has the most compositing glyphs of any set that's clean for GPL-2.0-or-later. Lucide shares the 24 px / 2 px round grid, so Lucide glyphs and our own fill-ins can sit beside Tabler without clashing; record each one's origin in the licence file. Keep one grouping set (the new SVGs) and remove the `useBwIcons` setting.

## Open questions for the user

1. Tabler or Lucide?
2. At 18 px, keep the native 2 px stroke (which renders at 1.5 device px) or thin it to 1.75 or 1.5? The pitch page has a toggle to compare.
3. Should the toolbox stay monochrome, or tint each group icon with its node-graph category colour (the in-progress `NodeCategoryEnum` work)?
4. Remove `useBwIcons` outright, or keep the knob hidden and ignore it?
5. Are `NativeNodes/` (33) and the merge-operator icons (34) in scope?
6. Is GPLv3-only distribution acceptable? If so, Apache-2.0 and LGPL-3 sets become options.

## Draft task breakdown (M56 Phase 3: icons)

This replaces the scoping draft's P3.T1 (audit) and P3.T2..Tn.

- [ ] M56.P3.T1 — Link Qt6::Svg and vendor the chosen set's SVGs with their licence
  - files: `CMakeLists.txt` (`QT_COMPONENTS`), `Gui/CMakeLists.txt` (link `Qt6::Svg`), `Gui/Resources/Icons/<set>/*.svg` (new, only the glyphs we use), `Gui/Resources/Icons/LICENSE-<set>.txt`, `Gui/GuiResources.qrc`, packaging dependency list
  - approach: Copy only the glyphs we use, under the upstream names, from a pinned upstream release. Strip comments and keep `currentColor`. Record the upstream version and licence text.
  - verify: a clean configure and build links Qt6::Svg, and `:/Resources/Icons/...` resolves at runtime (log check).
  - size: M
- [ ] M56.P3.T2 — Add a themed SVG path to the pixmap loader, and fix the scaled-cache bug
  - files: `Gui/GuiApplicationManager.cpp`, `Gui/GuiApplicationManager.h`, `Gui/GuiApplicationManagerPrivate.h`
  - approach: In `getIcon`, when a path ends in `.svg`, read the bytes, replace `currentColor` with the Settings text colour (and the stroke width chosen per size), and render with `QSvgRenderer` at size × devicePixelRatio, then call `setDevicePixelRatio`. The cache key becomes id + size + colour + dpr. Fix `:820` so the sized key gets the scaled pixmap. Clear the cache when the theme colour changes.
  - verify: an Xvfb shot of one converted viewer button at 1x and 2x is sharp. Changing the text colour in preferences recolours it after a reload.
  - size: M
- [ ] M56.P3.T3 — Draw state variants from one SVG with a QIconEngine
  - files: `Gui/SvgIconEngine.{h,cpp}` (new), `Gui/CMakeLists.txt`, call sites that pair `*Enabled/*Disabled` or `*_on/*_off` pixmaps (`ViewerTab.cpp`, `Gui/KnobGuiButton.cpp`)
  - approach: Paint Normal, Disabled, Active and Selected, each On or Off, by changing the colour (text, disabled grey, or the selection orange for On). Keep two-glyph toggles where the shape itself changes (play/pause, lock/unlock).
  - verify: Xvfb shots of the viewer toolbar with RoI, clip-to-project and auto-contrast toggled on and off.
  - size: L
- [ ] M56.P3.T4 — Send Engine's path-based icon labels through the themed loader
  - files: `Gui/KnobGuiButton.cpp`, `Gui/DopeSheetView.cpp`, `Gui/MultiInstancePanel.cpp`; path strings in `Engine/RotoPaint.cpp`, `Engine/TrackerNode.cpp`, `Engine/TrackerContextPrivate.cpp`
  - approach: Put the colour-and-render step in a Gui helper that both the enum path and the file-path loads call. Engine keeps passing `:/Resources/...` strings and only the file names change.
  - verify: Xvfb shots of the roto and tracker toolbars.
  - size: M
- [ ] M56.P3.T5 — Make the grouping icons one SVG set
  - files: `Gui/GuiApplicationManager.cpp` (`*_GROUPING` cases, about lines 278-330), `Gui/Gui20.cpp` (`getPixmapForGrouping`), `Engine/Settings.cpp`/`.h` (`_useBWIcons`, `getIconsBlackAndWhite`), `Gui/GuiResources.qrc`, `Gui/Resources/Images/GroupingIcons/**`
  - approach: Point every grouping case at `Icons/<set>/...svg` with no set index. Remove the B&W knob (or hide it, depending on Q4). Delete Set1–Set3. Coordinate with the uncommitted `NodeCategoryEnum` edits in Settings and Enums.
  - verify: an Xvfb shot of the left toolbox at 1x and 2x. An old settings file containing `useBwIcons` still loads without warnings.
  - size: M
- [ ] M56.P3.T6 — Viewer toolbar batch (about 25 ids: transport, fit, clip, RoI, proxy, refresh, pause render, auto contrast, checkerboard, gain/gamma, zebra)
  - files: `Gui/GuiApplicationManager.cpp` paths, `Gui/Resources/Icons/<set>/`, `Gui/GuiResources.qrc`
  - approach: Map each id to the glyph the user approved on the pitch sheet.
  - verify: before and after Xvfb shots of the viewer toolbar and the timeline.
  - size: M
- [ ] M56.P3.T7 — Properties panel batch (keyframe and animation menu, reset default, settings, help, minimize/close/float, lock, hide-unmodified, undo/redo)
  - files: same as T6, plus `Gui/DockablePanel.cpp` if sizes change
  - approach: Same as T6.
  - verify: before and after Xvfb shots of a Grade node's properties panel.
  - size: M
- [ ] M56.P3.T8 — Draw the custom glyphs (deep, multi-view, merge operators, interpolation, roto point tools, tracker controls)
  - files: `Gui/Resources/Icons/natron/*.svg` (new, GPL-2.0-or-later)
  - approach: Draw them on the 24 px / 2 px round grid to match the chosen set, using `currentColor` only. Add them to the pitch sheet for approval before wiring them in.
  - verify: the user approves a contact sheet at 18 and 24 px.
  - size: L
- [ ] M56.P3.T9 — Node graph, roto, tracker and curve editor batch, using T8's glyphs
  - files: `Gui/GuiApplicationManager.cpp`, `Engine/RotoPaint.cpp`, `Engine/TrackerNode.cpp`, `Gui/DopeSheetView.cpp` path strings
  - approach: Same as T6.
  - verify: before and after Xvfb shots of the roto toolbar, the tracker panel and the curve editor interpolation menu.
  - size: M
- [ ] M56.P3.T10 — Delete orphaned PNGs and their qrc entries
  - files: `Gui/GuiResources.qrc`, `Gui/Resources/Images/*.png` (only those no longer referenced)
  - approach: Use `grep` to build the list of PNGs no code references any more, remove their `<file>` lines and `git rm` the files. Leave `natronIcon*`, `splashscreen*` and `NativeNodes/` alone.
  - verify: the build passes, and running every `getIcon` id (a startup log of the `assert(!"Missing image.")` path) reports nothing missing.
  - size: S
