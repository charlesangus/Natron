# M602 - 3D Viewer Toolbars

Give the Viewer's 3D view the toolbar layout the reference apps use. A left vertical **tool bar** holds selection and transform tools, pick modes and a snapping slot. A right vertical **display bar** holds display toggles. A **top bar** holds the renderer, look-through camera and draw mode, plus a per-node **tool-options area** that the viewed or edited node fills with its viewer-context knobs. Today these controls live only in the 3D view's right-click menu (`SceneViewport::showContextMenu`, `Gui/SceneViewport.cpp` ~:13851) and in the horizontal geometry strip (`Gui/GeoDisplayBar.{h,cpp}`, `ViewerTab::geoDisplayRow`, `Gui/ViewerTab.cpp` ~:951). The right-click menu stays as a mirror of the bars.

Member of F40 - 3D Viewer Tools; its UAT is collected at the Feature (PLAN.md context, "Features save rebase churn").

## Scope

**In:**
- Left tool bar, 28 px wide and icon-only, with tooltips that name the shortcut: Select (Q), Translate (T), Rotate (R), Scale (E) from `kTransformTools` (`SceneViewport.cpp` ~:7125, keys in `Gui/ActionShortcuts.cpp` ~:533); the geometry pick modes Objects/Points/Edges/Primitives, moved from `GeoDisplayBar`; an empty slot reserved for M603 - Combined Transform Handle's Transform tool and one for M606 - Viewer Snapping.
- Right display bar, 28 px wide and icon-only: draw mode, Purposes (guide/proxy/render), Lighting, grid, and the geometry marker toggles (Point Markers, Point Numbers, Primitive Numbers, Normals), moved from `GeoDisplayBar`. Normal Scale and Highlight Group move into a "Display Options" popup opened from the bar. M607 - Geometry Marker Controls extends that popup.
- Top bar: Renderer (delegate) menu, Look Through camera menu, View/Clipping menus, and the progressive-render controls folded in from `sceneRenderRow` (`ViewerTab.cpp` ~:893). Also the **tool-options area**: viewer-context knobs (`Knob::addKnobToViewerUI`, `Engine/Knob.h` ~:3399) of the node the 3D tools target. USDEditTransform is the first user.
- A viewer-menu toggle for each bar, saved per viewer in the layout. By default the bars show in the 3D view and hide in the 2D view. Everything fits 1920x1080 at scale 1.
- Icons drawn in the style of the existing viewer icons (M519 - Raw And Loop Toolbar Icons).

**Out:**
- The combined handle and handle options (M603 - Combined Transform Handle).
- Snapping behaviour (M606 - Viewer Snapping); only the slot is here.
- New marker types and sizes (M607 - Geometry Marker Controls).
- The 2D viewer's per-node toolbars (`Gui/NodeViewerContext.cpp`) are unchanged; no milestone.

**Edge cases:**
- Viewer switching between the 2D and 3D views: the bars follow the view kind, and each keeps its own toggle state.
- Geometry-only controls (pick modes, markers) are disabled, not hidden, when the active input is a USD scene. This keeps the bar layout stable.
- A tool-options area with no targeted node is empty and takes no height.
- Two viewers keep independent bar state.

**Assumptions:**
- Unconfirmed: the bars are plain Qt widgets laid out around the `SceneViewport` GL widget, with no overlay compositing.

## Phase 602.1: Bars

- [x] M602.P1.T1 — Left tool bar with the transform tools and pick modes
  - files: new `Gui/SceneToolBar.{h,cpp}`, `Gui/ViewerTab.cpp`, `Gui/ViewerTabPrivate.h`, `Gui/SceneViewport.{h,cpp}`, `Gui/GeoDisplayBar.{h,cpp}`, `Gui/Resources/Images/` (icons), `Gui/GuiResources.qrc`, `Gui/CMakeLists.txt`
  - approach: A vertical `QToolBar`-like widget left of the scene viewport. One checkable, exclusive button per `kTransformTools` entry, wired to `SceneViewport::setTransformTool` and updated when the tool changes by key (connect to whatever signal `setTransformTool` emits, or add one). Move the pick-mode buttons out of `GeoDisplayBar` into the same bar, after a separator, unchanged in behaviour. Leave two empty, hidden slots named `transformToolSlot` and `snapSlot` for later milestones. Tooltips use the shortcut text from `ActionShortcuts`. Width 28 px.
  - verify: Xvfb GUI script `Tests/gui/m602_viewer_toolbars.py` at 1920x1080: clicking each tool button sets the tool (read back through the existing Python/viewer API, or a test hook if none exists); pressing T checks the Translate button; pick modes still switch.
  - size: M
- [x] M602.P1.T2 — Right display bar and the Display Options popup
  - files: new `Gui/SceneDisplayBar.{h,cpp}`, `Gui/ViewerTab.cpp`, `Gui/GeoDisplayBar.{h,cpp}` (removed or reduced), `Gui/SceneViewport.cpp`, `Engine/ViewerInstance.{h,cpp}` (only if a toggle has no knob yet)
  - approach: Vertical icon bar right of the viewport. Buttons for draw mode (menu of `kDrawModes`), Purposes, Lighting and grid, all driving the same state as the context-menu entries (one action object shared by the bar and the menu). After those come the four geometry marker toggles, bound to the existing ViewerInstance knobs (`pointMarkers`, `pointNumbers`, `primitiveNumbers`, `normals`, `ViewerInstance.cpp` ~:1027) through `editViewerKnobs`, so edits stay undoable. A "Display Options" button opens a popup holding Normal Scale and Highlight Group. Delete `geoDisplayRow` once it is empty.
  - verify: the same GUI script: each toggle changes its knob and the context menu shows the same state; the popup edits `normalScale`; with a USD input the geometry toggles are disabled.
  - size: M
- [x] M602.P1.T3 — Top bar with renderer, camera and the progressive-render controls
  - files: new `Gui/SceneTopBar.{h,cpp}`, `Gui/ViewerTab.cpp`, `Gui/SceneViewport.cpp`
  - approach: A horizontal bar above the viewport holding Renderer, Look Through, View and Clipping menus. They reuse the menu builders in `showContextMenu`: factor each submenu into a function that both the context menu and the bar call. Fold `sceneRenderRow`'s Max Samples, Pause, Render Everything and refinement label into the bar, keeping their show rule (progressive, non-Storm). Show it only in the 3D view.
  - verify: the GUI script switches renderer and look-through from the bar and checks the viewport state; with Storm the progressive controls are hidden; the 1920x1080 screenshot shows nothing clipped.
  - size: M
- [x] M602.P1.T4 — Tool-options area fed by viewer-context knobs
  - files: `Gui/SceneTopBar.{h,cpp}`, `Gui/SceneViewport.cpp` (`refreshTransformHandles` ~:12595), `Engine/Nodes/Scene/USDEditTransform.{h,cpp}`, `Gui/NodeViewerContext.cpp` (read for the 2D pattern)
  - approach: When the 3D tools target a node (the node `refreshTransformHandles` resolves, or `SceneNodeAuthoring::getShowingViewerContext`, `USDEditTransform.cpp` ~:315), build that node's viewer-context knobs (those registered with `addKnobToViewerUI`) into the top bar's right-hand area. Reuse the 2D viewer's knob-widget creation from `NodeViewerContext` rather than writing a second one. USDEditTransform registers its operation choice and Space as viewer-context knobs. Clear the area when no node is targeted.
  - verify: the GUI script opens a USDEditTransform panel and finds its viewer-context knobs in the top bar; editing one there changes the knob; closing the panel empties the area.
  - size: M
- [x] M602.P1.T5 — Bar visibility toggles saved in the layout
  - files: `Gui/ViewerTab.cpp`, `Gui/ViewerTabPrivate.h`, the viewer layout serialization (`Serialization/` viewer/tab state, found from where ViewerTab saves its toggles)
  - approach: Three checkable viewer-menu actions, Tool Bar, Display Bar and Top Bar. Store their state per viewer and per view kind (2D or 3D) alongside the existing viewer UI state. Defaults: shown in 3D, hidden in 2D. No old-project import (no backward compat).
  - verify: the GUI script hides the display bar, saves, reloads and finds it hidden; a gtest round-trips the serialized flags.
  - size: M

**Verification gate:** CI green; release gate green per `.local/build-mode` with `--gui` running `Tests/gui/m602_viewer_toolbars.py` under Xvfb at 1920x1080; the pre-existing scene and geometry GUI scripts still pass; screenshots of the 3D view with a USD scene and with a geometry node, each with all three bars, are shared one file per screenshot and collected for F40 - 3D Viewer Tools' UAT.

## Decisions
- 2026-11-25 — Layout follows the three-bar pattern of the reference app (left tools, right display, top bar with node tool options), with the right-click menu kept as a mirror (user, grill).
- 2026-11-25 — The tool-options slot is built now, so later handle milestones put their options there (user, grill).
- 2026-11-25 — Bars are 28 px icon columns, toggleable, saved per viewer; shown in 3D and hidden in 2D by default (user, grill).
- 2026-11-25 — Snapping becomes its own milestone, M606 - Viewer Snapping; M602 only reserves the slot (user, grill).
- 2026-11-26 (PM) — P1.T4: USDEditTransform registers only Mode in the top bar; Append/Replace already means local versus world, so no Space knob was added. P1.T5: bar visibility is saved in the project's viewer node (ViewerBarsSerialization), where viewer UI state already lives, not in a layout file.
- 2026-11-27 — **Screenshot review (user):** (1) The left tool bar gets a node-specific handle tool after Scale: for transforms the multi-transform handle; other nodes register whatever handle they implement. (2) Right bar: Draw Mode's button shows the selected mode (wireframe / solid / shaded box). Purposes needs a different icon than layers, and should also show the selection (e.g. outline for proxy, solid for render/default, a ruler for guide) — PM pitches options. Lighting's button shows the selected setting. Show Grid must not reuse the pane-menu grid icon; it should look like a construction grid with three axes and real squares. (3) Top bar: none of the four icons (Renderer, Look Through, View, Clipping) communicate their job — PM pitches clearer options.
- 2026-11-27 — **Icon and tool choices (user):** Purposes = option C (letter tiles P / R / D / G, stacked tiles with + for several), and the button shows the current selection. Top bar: Renderer = option B (film frame); Look Through = an eye top-left with a camera bottom-right; View = a state icon showing a camera with a frustum for perspective and a camera with parallel lines for orthographic; Clipping = option A (side-on frustum cut by near and far planes). Select no longer draws a node's handles (only the node handle tool does). If the node's panel closes while the node handle tool is active, the tool switches back to Select. Open (PM): M603 - Combined Transform Handle planned a generic Transform tool in the slot the node handle tool now uses — re-scope M603 when it starts.
- 2026-11-27 — **Icon tweaks (user):** the Scene Lights icon gets rays coming from the light (it read as off); the Show Grid icon must be simpler, it was too busy.
