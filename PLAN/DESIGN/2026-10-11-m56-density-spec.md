# M56 density spec (P6.T1)

Status: **pitch, awaiting user approval.** Nothing here is applied yet. Once the user approves, the token values go into M56's `## Decisions`, and P6.T2–T4 apply exactly those values.

Mockups: `m56-density.html`, a before/after page that the PM publishes as a claude.ai artifact.

Scope (user, 2026-10-11):
- Spacing, padding and grouping only.
- Colours stay unchanged: `%1`–`%11` and the `Engine/Settings.cpp` defaults.
- Nuke is the reference look.
- Menus get styling only: item padding, separators and the icon column.

Code base: `.worktrees/m56` at `ab21ebfc1`. Line numbers will move after the M44 and M55 rebases.

"Before" evidence:
- `build/m55-shots/out/render-dialog-write-panel.png`: properties panel and tab bars, at 1×.
- `render-foreground-mid.png`: pane tab bar.
- `build/m55-shots/m24/01-categories-screen.png`: node graph tab bar.
- There are no viewer or menu shots yet. Those two "before" mockups are drawn from the code below, not from pixels. P1.T1's density shots should add both.
- The test host has no Droid Sans, so the shots show a serif fallback. The mockups use a sans stack.

## 1. Catalogue of spacing sources

Values are in logical px. "DPI" means the value is wrapped in `TO_DPIX`/`TO_DPIY`. "raw" means it is not scaled, which is a latent hi-DPI bug whenever the logical DPI is not 96.

### 1a. `Gui/Resources/Stylesheets/mainstyle.qss` (807 lines)

| # | Selector | Line | Current value | Notes |
|---|---|---|---|---|
| Q1 | `QToolTip` | 42-48 | border 2px, radius 5px | Out of scope. |
| Q2 | `QToolBar` | 50-54 | `spacing: 3px` | The left toolbox is an `AutoHideToolBar` (a QToolBar), so this sets the gap between toolbox buttons. |
| Q3 | `QGroupBox::title` | 64-68 | `padding: 0 3px` | Group boxes in dialogs. |
| Q4 | `QGroupBox::indicator` | 70-79 | 10×10 | Fold arrow size. |
| Q5 | `QScrollBar:vertical` | 81-86 | width 15, margin 23px 0 | Out of scope. |
| Q6 | `QScrollBar:horizontal` | 120-125 | height 15, margin 0 23px | Out of scope. |
| Q7 | `ComboBox` | 200-204 | radius 2px, border 1px | No padding rule; Fusion's default applies. |
| Q8 | `QPushButton` | 287-293 | `padding: 3px`, radius 3px | Text buttons, for example "OCIO config help…" and "Render". |
| Q9 | `QTabWidget::pane` | 428-431 | `margin-top: -1px`, border 1px black | Properties "Controls / Node / Info" pane. |
| Q10 | `QLineEdit` | 433-441 | `padding: 1px`, radius 1px | Also sets the knob text-field height. |
| Q11 | `QLineEdit:focus` | 443-447 | border 2px | Focus border; padding is commented out. |
| Q12 | `QSplitter::handle` | 466-471 | 3px | Pane splitter width. Out of scope. |
| Q13 | `QTabBar::tab` | 479-490 | `padding: 4 6 2 9` (top right bottom left), `min-width: 5em`, radius 5px top | Measured: 28 px outer height, and the Node Graph tab is 122 px wide. Left and right padding are asymmetric. |
| Q14 | `QTabBar::tab#DockablePanelTabWidget` | 491-494 | `min-width: 3em` | Properties-panel page tabs. |
| Q15 | `QTabBar::tab:selected / :first / :last / :only-one` | 510-523 | margins 0 | Overlap margins are commented out. |
| Q16 | `QTabBar::close-button` | 524-532 | positioned right | Qt leaves about 12 px between the text and the ×. Fine as is. |
| Q17 | `QMenu` | 553-560 | `padding: 1px 0 0 1px`, `margin: 0` | The comment explains this: top must be > 1px, otherwise the first item is selected on click. |
| Q18 | `QMenu::item` | (none) | Qt/Fusion default | There is no item padding, separator or icon rule. Items are about 24 px tall (estimated, no shot), with an icon gutter of about 20 px and a lead-in of about 4 px. |
| Q19 | `QMenuBar::item` | 746-750 | (none) | Default item padding. |
| Q20 | `QTextEdit:focus` | 640-644 | `padding: 2px 4px` | Out of scope. |

### 1b. C++ layouts

| # | File:line | Call | Current | DPI? | What it spaces |
|---|---|---|---|---|---|
| C1 | `DockablePanel.cpp:116-117` | main layout spacing / margins | 0 / 0,0,0,0 | raw | Header-to-body stack. |
| C2 | `DockablePanel.cpp:178` | header margins | 0,0,0,0 | raw | Panel header strip (icon, name, buttons). |
| C3 | `DockablePanel.cpp:180` | header spacing | 2 | raw | Gap between header buttons. |
| C4 | `DockablePanel.cpp:185` | icon label margins | 2,2,2,2 | raw | Plugin icon in the header. |
| C5 | `DockablePanel.cpp:419` | `_horizLayout` margins | 0,3,3,0 | raw | Colour bar plus page area. This is the panel's outer margin, and it is asymmetric. |
| C6 | `DockablePanel.cpp:420` | `_horizLayout` spacing | 2 | raw | Gap between the node-colour bar and the pages. |
| C7 | `DockablePanel.cpp:424-425` | right container spacing / margins | 0 / 0 | raw | Page stack. |
| C8 | `KnobGuiContainerHelper.cpp:49` | `NATRON_FORM_LAYOUT_LINES_SPACING` | 0 | — | Grid spacing (C9). |
| C9 | `KnobGuiContainerHelper.cpp:261` | page grid `setSpacing` | 0 | DPI | Both row and column gap of the knob grid. The label-to-field gap comes only from C10 and C11. |
| C10 | `KnobGuiContainerHelper.cpp:50,690` | field row margins | L 3, B `NATRON_SETTINGS_VERTICAL_SPACING_PIXELS`=3 | DPI | **The row gap** (3 px below every row) and a 3 px left inset on the field. |
| C11 | `KnobGuiContainerHelper.cpp:691` | field row spacing | 2 | DPI | Gap between widgets inside one knob, for example a value box and its slider. |
| C12 | `KnobGuiContainerHelper.cpp:715-716` | label cell margins / spacing | L 3, B 3 / 2 | DPI | The label column's own row gap, mirroring C10. |
| C13 | `KnobGuiContainerHelper.cpp:135-137` | `getItemsSpacingOnSameLine()` | 15 | DPI | Gap before a second knob on the same line, for example "Compression … Compression Level". Used by `KnobGui.cpp:216`. |
| C14 | `TabGroup.cpp:38,113` | `NATRON_FORM_LAYOUT_LINES_SPACING` | 0 | raw | Tab-style groups. |
| C15 | `KnobGuiGroup.cpp:155` | group arrow `setFixedSize` | 22×22 | **raw** | Group header arrow. Children are not indented at all, so the group indent is 0 today. |
| C16 | `KnobGuiSeparator.cpp:99` | separator line height | 2 | DPI | The section divider. There is no extra space above or below it beyond C10. |
| C17 | `KnobGuiValue.cpp:247,380` | value container spacing | 3 | raw | Value box to slider. |
| C18 | `KnobGuiValue.cpp:355-356,603` | spin-box grid | 1 / 1 | DPI | Multi-dimension boxes. |
| C19 | `KnobGuiColor.cpp:846,881` | `addSpacing` | 10, 5 | DPI | Slider to "4" button to swatch on colour rows. |
| C20 | `KnobGuiLayerChannelBase.cpp:184` | spacing | 2 | raw | Layer combo and R G B A toggles. |
| C21 | `TabWidget.cpp:260` | pane main layout margins | 0,**5**,0,0 | raw | 5 px above the pane tab bar. |
| C22 | `TabWidget.cpp:261,266-267` | main / header spacing and margins | 0 | raw | Pane header row. |
| C23 | `TabWidget.cpp:285` | `headerLayout->addSpacing(10)` | 10 | raw | Gap between the left-corner button and the first tab. |
| C24 | `TabWidget.cpp:908` | splitter margins | 0 | raw | Out of scope. |
| C25 | `ViewerTab.cpp:89,98` (`addSpacer`) | spacing either side of the VLine | 5 + 1 + 5 | raw | Viewer toolbar group separators. Used at `:170,251,316,356`. |
| C26 | `ViewerTab.cpp:129-130` | main layout | 0 / 0 | raw | |
| C27 | `ViewerTab.cpp:140-141` | first toolbar row | margins 0, spacing 0 | raw | Buttons touch; combos touch. |
| C28 | `ViewerTab.cpp:415-416` | second toolbar row | margins 0, spacing 0 | raw | Gain/gamma row. |
| C29 | `ViewerTab.cpp:562-563,568-569` | viewer area | 0 / 0, 0 / 1 | raw | GL area. Out of scope. |
| C30 | `ViewerTab.cpp:617-618` | player (timeline) row | 0 / 0 | raw | Playback controls. |
| C31 | `ViewerTab.cpp:964-995` | player `addSpacing` | 5, 10 ×5 | DPI | Playback control groups. |
| C32 | `ViewerTab.cpp:1016` | `mainLayout->addSpacing` | 5 | DPI | Under the player row. |
| C33 | `ViewerTab.cpp:224-225` | toolbar button size | 22 / icon 18 | DPI | `NATRON_MEDIUM_BUTTON_SIZE`. Sizes stay unchanged. |
| C34 | `Gui05.cpp:106-109` | toolbox `setMaximumWidth` | `NATRON_TOOL_BUTTON_SIZE` = 30 | DPI | The brief says `Gui20.cpp`; the toolbox is actually built here. |
| C35 | `GuiPrivate.cpp:556-557` | toolbox button `setFixedSize` | 30×30 (icon 24 + 6 border) | DPI | |
| C36 | `GuiPrivate.cpp:333` | properties-bin button row spacing | 5 | raw | "10 × □" row above the panels. |
| C37 | `ToolButton.cpp` | — | none | — | Holds only the menu and icon model, with no layout. Toolbox spacing comes from Q2 and C34–C35. |
| C38 | `Gui20.cpp:276-290` | `loadStyleSheet` | font prelude plus `%1`–`%11` | — | The only place to add a qss substitution if a token needs DPI scaling (see §4). |

`DockablePanelPrivate.cpp` and `DockablePanelTabWidget.cpp` have no spacing calls.

## 2. Tokens

All values are logical px. In C++, wrap them in `TO_DPIX` (horizontal) or `TO_DPIY` (vertical). In qss, write the literal px value (see §4). Field and button sizes (22 px, the 30 px toolbox) do not change.

| Token | Now | Proposed | Applies to |
|---|---|---|---|
| `row-gap` | 3 (C10/C12 bottom margin) | **4** | Vertical space between knob rows. Pitch goes from about 25 to 26 for 22 px widgets. |
| `label-gap` | ≈ 4 measured (C10 left inset 3; grid spacing 0) | **8** | Right edge of the label column to the first field. |
| `inline-gap` | 15 (C13) | **12** | Before a second knob on the same line. |
| `group-indent` | 0 | **12** | Left margin on the label cells of a non-tab group's child rows. Labels stay right-aligned in the shared column, so the margin only moves a row whose label is the widest. On every row it guarantees the child label starts at least 12 px right of the group title. |
| `section-gap` | 0 (2 px rule only, C16) | **8** | Space above and below a separator line or a group header. |
| `panel-margin` | 0 / 3 / 3 / 0 (C5), pane −1 (Q9) | **6** on all sides of the panel body; **2** around the header strip | Properties panel body and header. |
| `header-gap` | 2 (C3) | **2**, plus **8** between button clusters | Panel header buttons. Keep 2 inside a cluster; add 8 between the node-identity, nav, and window-control clusters. |
| `tab-padding` | 4 / 6 / 2 / 9 (T R B L) | **2 / 10 / 2 / 10** | `QTabBar::tab`. This makes it symmetric. |
| `tab-height` | 28 outer, measured (about 20 content + 6 padding + 2 border) | **26** outer (20 + 4 + 2); no `min-height` | Pane and page tabs. The pane top margin also goes from 5 to 2 (C21). |
| `menu-item-padding` | Fusion default (≈ 3 / 12 / 3 / 4) | **3 px 24 px 3 px 8 px**, plus the icon column | `QMenu::item`. Rows are about 24 px at 11 pt (18 px text + 6). |
| `menu-icon-column` | ≈ 18–20 (style default) | **24** | `QMenu::icon` and `::indicator` gutter. Every item's text starts at the same x, with or without an icon. |
| `menu-separator-margin` | default 1 px line, ≈ 2 px margin | **4 px 8 px** (1 px line, 9 px total) | `QMenu::separator { height:1px; margin:4px 8px; }`. |
| `menu-padding` | 1 0 0 1 (Q17), no frame | **4 0 4 0**, plus `border: 1px solid %4` | Space at the top and bottom of the popup. The "top > 1px" rule in the comment still holds. The frame uses an existing colour, so the popup no longer melts into the `%2` node graph. |
| `toolbar-spacing` | 3 (Q2) toolbox; 0 (C27/C28) viewer | **2** within a group | Toolbox buttons and viewer toolbar buttons. |
| `toolbar-group-gap` | 5 + line + 5 (C25) | **6 + line + 6** | Viewer toolbar group separators. |

## 3. Source-to-token mapping

| Source | Token | New value | Where it is set |
|---|---|---|---|
| C10, C12 bottom margin (`NATRON_SETTINGS_VERTICAL_SPACING_PIXELS`) | `row-gap` | 4 | C++ via `GuiDensity.h` (P6.T3) |
| C9 grid spacing (`NATRON_FORM_LAYOUT_LINES_SPACING`) | `label-gap` (horizontal only) | `setHorizontalSpacing(8)`; vertical stays 0 | C++ via `GuiDensity.h` (P6.T3) |
| C10/C12 left inset 3 | `label-gap` | 0 (the gap moves to the grid column spacing) | C++ via `GuiDensity.h` (P6.T3) |
| C11 field-row spacing 2 | unchanged (intra-knob) | 2 | — |
| C13 `getItemsSpacingOnSameLine` | `inline-gap` | 12 | C++ via `GuiDensity.h` (P6.T3) |
| C14 `TabGroup.cpp` | `row-gap` | 4 | C++ via `GuiDensity.h` (P6.T3) |
| C15 group children | `group-indent` | 12 | C++, **new code path** (P6.T3; see §4) |
| C15 arrow `setFixedSize` raw 22 | hi-DPI fix | `TO_DPIX(22)` | C++ via `GuiDensity.h` (P6.T3) |
| C16 separator | `section-gap` | 8 above and below | C++ via `GuiDensity.h` (P6.T3) |
| C5 `_horizLayout` margins | `panel-margin` | 6,6,6,6 | C++ via `GuiDensity.h` (P6.T3) |
| C2 header margins | `panel-margin` (header) | 2,2,2,2 | C++ via `GuiDensity.h` (P6.T3) |
| C3 header spacing | `header-gap` | 2, plus `addSpacing(8)` between clusters | C++ via `GuiDensity.h` (P6.T3) |
| Q9 `QTabWidget::pane` | `panel-margin` | keep `margin-top:-1px`; the pane padding comes from C5 | qss (P6.T2) |
| Q13 `QTabBar::tab` padding | `tab-padding` | `padding: 2px 10px` | qss (P6.T2) |
| Q13 height | `tab-height` | follows from the padding (26 outer) | qss (P6.T2) |
| Q14 page-tab min-width | `tab-padding` | keep 3em | qss |
| Q16 close button | unchanged | Qt already leaves about 12 px before the × (measured) | — |
| C21 `TabWidget` top margin 5 | `tab-height` (header rhythm) | 2 | C++ via `GuiDensity.h` (P6.T4) |
| C23 corner-button gap 10 | `toolbar-group-gap` | 6 | C++ via `GuiDensity.h` (P6.T4) |
| Q17 `QMenu` padding | `menu-padding` | `padding: 4px 0px` | qss (P6.T2) |
| Q18 `QMenu::item` | `menu-item-padding` + `menu-icon-column` | `padding: 3px 24px 3px 32px` (8 + 24 column); `QMenu::icon { padding-left: 8px; }`; `QMenu::indicator { width: 14px; left: 8px; }` | qss (P6.T2) |
| Q18 separator | `menu-separator-margin` | `QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }` | qss (P6.T2). Uses the existing `%4`; no new colour. |
| Q19 `QMenuBar::item` | `menu-item-padding` (horizontal) | `padding: 3px 8px` | qss (P6.T2) |
| Q2 `QToolBar` spacing | `toolbar-spacing` | 2 | qss (P6.T2) |
| C34/C35 toolbox | unchanged size | 30 | — |
| C27/C28 viewer rows spacing 0 | `toolbar-spacing` | 2 | C++ via `GuiDensity.h` (P6.T4) |
| C25 `addSpacer` 5/5 raw | `toolbar-group-gap` | `TO_DPIX(6)` / `TO_DPIX(6)` | C++ via `GuiDensity.h` (P6.T4) |
| C27/C28 row margins 0 | `panel-margin` (toolbar) | 4,2,4,2 | C++ via `GuiDensity.h` (P6.T4) |
| C31/C32 player spacing | `toolbar-group-gap` | keep (already DPI, already grouped) | — |
| C36 properties-bin row | `toolbar-spacing` | 2 | C++ (optional, P6.T4) |
| C17–C20 knob internals | unchanged | — | Outside P6.T3's file list. Leave as is. |
| Q3, Q8, Q10 | unchanged | — | The button, line-edit and group-box padding already read fine in the shots. |

## 4. Rationale against Nuke's density

The Nuke figures below are approximate, from familiarity with Nuke 13–15 at 1×. They are not measured, so treat them as targets to check against the user's own Nuke.

- **row-gap 4.** Nuke's knob rows sit at roughly a 24–26 px pitch, with fields barely touching. Natron's 25 px pitch is already Nuke-like, so this is a +1 nudge for breathing room, not a redesign. The cramped feel comes from horizontal hugging and the missing section rhythm, not row pitch.
- **label-gap 8.** Nuke leaves a clear gutter (about 6–8 px) between the right-aligned label and the field. Natron's 3 px makes labels read as glued to the value boxes ("Input Colorspace|ACES"). This is the single biggest de-cramping move.
- **inline-gap 12.** Nuke's same-line knobs ("min … max") sit about 10–12 px apart. 15 px is slightly loose next to an 8 px label gap; 12 keeps the second knob visibly separate without drifting.
- **group-indent 12.** Nuke draws a group's contents under a disclosure triangle with a small indent, so you can see where a group ends. Natron does not indent at all, so a folded group's children are indistinguishable from the rows below it. 12 px is the arrow's visual width.
- **section-gap 8.** Nuke dividers have air on both sides. Natron's 2 px sunken line sits flush on the 3 px row margin, so sections run together. 8 above and below gives a 17–18 px band that reads as a break.
- **panel-margin 6 / header 2.** Nuke's properties pages have an even margin of about 6 px. Natron's 0/3/3/0 is asymmetric: content touches the left colour bar and the bottom edge.
- **header-gap 2 + 8.** Nuke's panel header groups its buttons. Natron's 13 header buttons sit at a uniform 2 px, so the "close / float / minimize" cluster blends into "undo / redo / reset".
- **tab-padding 2/10, tab-height 26.** Nuke's pane tabs are low and symmetric, at about 22–24 px. Natron's 4/6/2/9 padding puts text off-centre in both axes, and at 28 px outer (measured) its tab bars are the tallest strips in the UI. Symmetric 2/10 padding gives 26 px. Going lower would mean squeezing Qt's 20 px text box, so 26 is the floor without a font change. Together with the pane top margin dropping from 5 to 2, each tab bar gives back 5 px.
- **menu-item-padding 3/24/3/8, icon column 24, separator 4/8, menu padding 4.** Nuke's menus have about 22–24 px rows, a fixed icon gutter so labels align with or without an icon, a right-side gap before the shortcut, and inset separators. Natron's menus run Fusion defaults under a stylesheet with only a 1 px frame padding. The popup has no frame (`border: 0` on a `%2` background, the same as the node graph), labels sit tight to the left edge, shortcuts crowd the text, and separators run edge to edge. Row height barely changes; the horizontal room and the frame do. The frame is `border: 1px solid %4`, an existing colour. Menus stay styling-only (user decision).
- **toolbar-spacing 2, group gap 6.** Nuke's viewer toolbar buttons have a hairline gap, and its groups sit apart with a little more space. Natron's buttons touch (spacing 0), so adjacent toggles read as one wide control.

## 5. What the qss cannot change (C++ only)

Everything in §1b is a layout call, and **no stylesheet rule reaches it**. §6 says where these values live instead. In particular:
- The row gap, label gap, inline gap, group indent, section gap and panel margins (C5, C9–C16). These are QLayout margins and spacing set in `DockablePanel.cpp` and `KnobGuiContainerHelper.cpp`. QSS `margin`/`padding` on the container widgets would add to them and would not replace them.
- The viewer toolbar spacing and separator gaps (C25, C27, C28). These are the `QHBoxLayout::setSpacing(0)` and `addSpacer()` literals in `ViewerTab.cpp`.
- The pane header (C21, C23). These are `TabWidget.cpp` layout literals.
- The toolbox button size and bar width (C34, C35). They are fixed in C++. Only the inter-button `spacing` (Q2) is qss.
- **Group indent needs a new code path.** Nothing indents group children today. P6.T3 adds a left margin on the child rows' label cell (or a leading spacer) when the parent is a non-tab `KnobGroup`. This is a small but new behaviour, not a literal swap.

Hi-DPI notes for the implementers:
- Raw (unscaled) literals that should become `TO_DPIX`/`TO_DPIY` while they are being touched: C1–C7, C15, C17, C20–C23, C25–C28 and C36.
- QSS px are **not** passed through `TO_DPIX`. Under Qt 6, `QT_SCALE_FACTOR=2` scales both qss px and layouts, and `TO_DPIX` stays near 1, so the two stay consistent. They diverge only when the screen's *logical* DPI is not 96 (for example `Xft.dpi: 144`): then `TO_DPIX` is 1.5 and the qss stays at 1×. If P7.T1's 2× shots show a mismatch, P6.T2 can add one substitution to `loadStyleSheet`'s prelude (`Gui20.cpp:276-290`), for example `%12` = the DPI ratio. Do **not** reorder `%1`–`%11`.

## 6. Where the C++ values live

Qt stylesheets cannot set layout spacing: `QLayout::setSpacing` and `setContentsMargins` don't respond to qss. A qss `margin` or `padding` on a container widget adds to the layout's values and never replaces them. The C++ tokens therefore need their own single source of truth, rather than more bare literals spread across six files.

**Fix (P6.T3/T4):** add one header, `Gui/GuiDensity.h`, with named constants in logical px:

```cpp
#define NATRON_DENSITY_ROW_GAP            4
#define NATRON_DENSITY_LABEL_GAP          8
#define NATRON_DENSITY_INLINE_GAP        12
#define NATRON_DENSITY_GROUP_INDENT      12
#define NATRON_DENSITY_SECTION_GAP        8
#define NATRON_DENSITY_PANEL_MARGIN       6
#define NATRON_DENSITY_HEADER_MARGIN      2
#define NATRON_DENSITY_HEADER_CLUSTER     8
#define NATRON_DENSITY_TOOLBAR_SPACING    2
#define NATRON_DENSITY_TOOLBAR_GROUP_GAP  6
```

Every call site in §3 marked C++ uses these, always through `TO_DPIX`/`TO_DPIY` (for example `fieldLayout->setContentsMargins(0, 0, 0, TO_DPIY(NATRON_DENSITY_ROW_GAP))`). The constants replace:
- the file-local `NATRON_SETTINGS_VERTICAL_SPACING_PIXELS` and `NATRON_FORM_LAYOUT_LINES_SPACING` in `KnobGuiContainerHelper.cpp:49-50` and `TabGroup.cpp:38`;
- the 15 in `getItemsSpacingOnSameLine()`;
- the raw literals in `DockablePanel.cpp`, `TabWidget.cpp`, `ViewerTab.cpp` and `GuiPrivate.cpp`.

The header sits beside `GuiDefines.h`, which keeps the widget sizes (button and tool sizes). This supersedes P6.T3's "constants kept local to these files" approach.

**Optional later step (not M56):** expose the same values as `Q_PROPERTY`s on the container widgets (`DockablePanel`, `ViewerTab`, `TabWidget`), with defaults from `GuiDensity.h`. A user stylesheet could then theme them (`Natron--DockablePanel { qproperty-rowGap: 4; }`), and the qss and C++ tokens would be set in one place. Each setter must reapply its value to the layout, and the values still pass through `TO_DPIX`.

## 7. Approval

On approval, record the §2 values in M56 `## Decisions` verbatim. P6.T2 does the qss rows, P6.T3 the `DockablePanel` and `KnobGuiContainerHelper` rows, and P6.T4 the `ViewerTab`, `TabWidget`, `Gui05` and `GuiPrivate` rows.
