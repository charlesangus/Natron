# M146 - Roto Hosting For Any Node

Let a node other than RotoPaint host Roto curves, the curve-editing viewer tools and the curve-list panel, without inheriting RotoPaint's render tree, paint pages or compositing knobs. The work splits a geometry-only base out of `RotoContext`, and a curve-editing interact out of `RotoPaintInteract`. Roto and RotoPaint become a paint layer on top of both. M147 - SplineWarp Node Core is the first real client. This milestone proves the split with a test-only host.

Scouted facts (2026-10-20, `main` at `8fdb5741f`; re-check line numbers at claim, after M137 - Native Base Class Flexibility merges):
- **Context creation** is gated in one place: `Node::createRotoContextConditionnally()` (`Engine/Node.cpp:245-250`), on `isRotoNode() || isRotoPaintingNode()`. It is called from `Engine/NodeMain.cpp:108`. `isRotoNode()` compares the plugin ID with `PLUGINID_OFX_ROTO` (`Node.cpp:4662`). `isRotoPaintingNode()` forwards to the virtual `EffectInstance::isRotoPaintNode()` (`EffectInstance.h:466`), which only `RotoPaint` overrides (`RotoPaint.h:59`).
- **`RotoContext` assumes a paint host:**
  - `makeBezier` and `makeStroke` call `RotoDrawableItem::createNodes()` (`RotoContext.cpp:455,516`; `RotoDrawableItem.cpp:155-300`). That creates an out-of-project OFX Roto node and a Merge per Bezier, and more for brush strokes.
  - `refreshRotoPaintTree` (`RotoContext.cpp:4673`) is called from `RotoLayer.cpp:189,227,253`, `RotoDrawableItem.cpp:529,542`, `RotoSerializationConvert.cpp:400`, `RotoPanel.cpp:2705,2727` and `RotoContext.cpp:4760`.
  - `RotoContextPrivate`'s constructor (`RotoContextPrivate.h:1445-1546`) `dynamic_cast`s to `RotoPaint`. It unconditionally creates the General, Shape, Stroke, Clone and Transform pages on the host. Stroke (`:1803-1928`) and Clone (`:1596-1798`) are paint-only. Feather and featherFallOff (`:1566-1580`) are generic.
  - The paint-only APIs are `getRotoPaintBottomMergeNode`/`getRotoPaintTreeNodes` (`RotoContext.cpp:145-200`) and `getActiveRotoDrawingStroke` (`:844`).
- **`RotoPaintInteract`** (`Engine/RotoPaintInteract.{h,cpp}`) only reads `p->publicInterface`:
  - `redrawOverlayInteract`, `getNode()->getRotoContext()`, `triggerAutoSave`, the viewport pixel scale, `getCurrentTime` and `pushUndoCommand`.
  - Its paint-only members are the stroke/clone state, the brush/clone/effect/merge roles and tools, and the 16 paint-button knobs (`RotoPaintInteract.h:~480`). `onRoleChangedInternal`/`onToolChangedInternal` (`.cpp:652,712`) dereference those knobs with no null check.
- **Overlay entry points** are in `Engine/RotoPaint.cpp`: `drawOverlay` 1592-2033, `onOverlayPenDown` 2163-2620, `onOverlayPenMotion` 2621-3047, `onOverlayPenUp` 3048-3139, and the key/focus handlers 3140-3250. About 300 of their ~1,650 lines are paint-specific (brush circle and clone cross 1962-2030, eraser 2182, brush tools 2551-2610, stroke build 2974-3030, stroke finish 3091-3125).
- **The toolbar** is a generic knob page. `RotoPaint::initializeKnobs` (`RotoPaint.cpp:186-1058`) builds it with `setAsToolBar(true)` and tool-button knobs, and `Gui/NodeViewerContext.cpp:192-204` picks up any node's toolbar page.
- **Undo commands** (`Engine/RotoUndoCommand.{h,cpp}`, 15 commands) take `RotoPaintInteractPtr` and never use `RotoPaint` itself. `AddStrokeUndoCommand` (`.cpp:639`) and `AddMultiStrokeUndoCommand` (`:687`) are paint-only.
- **Generic gates on a non-null `getRotoContext()`:**
  - `Node::shouldDrawOverlay` (`NodeOverlay.cpp:47`)
  - the curve editor: `Gui/CurveEditor.cpp:361,374,1092,1254,1299,1425,1494,1605,1658`, `CurveEditorUndoRedo.cpp:467,585,1069` and `CurveWidget.cpp:1193-1237`
  - save and restore: `ProjectSerializationConvert.cpp:217` and `Node.cpp:2325`
  - Python export: `NodeGroup.cpp:2604,2840`
  - `node.roto`: `RotoContext::declarePythonFields` (`RotoContext.cpp:4795`), `Node.cpp:686`
- **Paint gates that stay paint-only:** `ParallelRenderArgs.cpp:174,185,1106,1163,1225`, the `Node.cpp` render-tree and caching sites (`:1070,1097,2559,2600,5511,7673,7725,7896,7948,8048,8189`), and `OutputSchedulerThread.cpp:3642,3649,4074,4079` and `ViewerInstance.cpp:348-382,594,2943` (the active stroke).
- **Panel:** `RotoPanel` is created only when `isRotoPaintingNode()` (`Gui/NodeSettingsPanel.cpp:133-139`) and added at `Gui/DockablePanel.cpp:765`. It uses the Label, Activated, Locked, Operator, Overlay and Color columns (`RotoPanel.cpp:80-85`), and builds an Operator combobox for every drawable (`:1247`).
- **Viewer slot:** `findActiveNodeContextForPlugin` (`Gui/ViewerTabPrivate.cpp:442-445`) treats `PLUGINID_NATRON_ROTO` and `PLUGINID_NATRON_ROTOPAINT` as one slot. It is called from `ViewerTab30.cpp:667,796`.
- **Serialization:** `Serialization/RotoSerialization.h` covers the item, drawable, Bezier, stroke and layer structs. Versions are in `RotoXml.h:54-80`. The context carries baseLayer, selectedItems, autoKeying, rippleEdit and featherLink.
- **Python:** `Engine/PyRoto.{h,cpp}` (`ItemBase`, `Layer`, `BezierCurve`, `Roto`) and `Engine/typesystem_engine.xml:1871-2120`.
- **Tests:** `Tests/RotoLayer_Test.cpp`, `RotoContentHash_Test.cpp`, `RotoFeatherDrag_Test.cpp` and `CurveTangentJoin_Test.cpp`. Test-only plugins register in `Tests/wmain.cpp:136`.

## Scope

**In:**
- A geometry-only curve context base class, `RotoCurveContext`: curves, layers, animation, selection, feather, serialization, content hashing and change evaluation. `RotoContext` derives from it and keeps everything paint-specific.
- Render-node creation and `refreshRotoPaintTree` move behind paint-context hooks, so a geometry host never builds per-curve nodes.
- One capability gate that says which context a node hosts (none, geometry or paint). It replaces the plugin-ID and `isRotoPaintNode()` checks for context creation and for every generic consumer.
- A curve-editing interact, `RotoCurveInteract`, that any host can use: the curve tools, overlay drawing and pen/key handling, and the undo commands. `RotoPaintInteract` derives from it and adds the paint roles and tools.
- A reusable toolbar builder, so a host picks a subset of the curve tools and appends its own.
- `RotoPanel` for any curve host. It hides the paint-only columns on geometry contexts, and has a hook a host uses to add its own columns and context-menu actions.
- One viewer tool slot shared by every curve host.
- A test-only geometry host that proves all of the above. Roto and RotoPaint behave exactly as before.

**Out:**
- SplineWarp itself → M147 - SplineWarp Node Core and M148 - SplineWarp Viewer Tools.
- Splitting `Bezier`'s geometry from `RotoDrawableItem` (per-item knobs stay on the item; only node creation moves) → none.
- The dope sheet's skip of roto nodes (`Gui/DopeSheet.cpp:337`) → none.
- Track links and cross-node curve copy → M152 - Curve Track Links And Roto Copy.

**Edge cases:**
- Two curve hosts with open panels: whichever panel was opened last owns the viewer tools. The other's curves are still drawn, but not editable.
- A geometry host's curves: no per-curve effect or Merge node is created, on creation, paste or project load.
- Project load of a Roto/RotoPaint project: it gets the paint context exactly as before. A geometry host's project restores its curves through the same serialization path.
- `RotoPaintInteract`'s paint knobs are null on a geometry host: the curve interact never reaches the paint-role code, so the missing knob dereference can't happen. A real check guards it, because asserts are inert in this build.

**Assumptions:**
- Per-item knobs (opacity, colour, operator) still exist on a geometry host's items, unused and hidden from its panel. Feather and featherFallOff stay, because M147 - SplineWarp Node Core's boundaries use them.
- M137 - Native Base Class Flexibility's merge doesn't touch the roto files beyond includes. Re-check at the freshness check.

## Phase 146.1: Geometry-only curve context

- [x] M146.P1.T1 — Extract `RotoCurveContext` from `RotoContext`
  - files: `Engine/RotoCurveContext.{h,cpp}` (new), `Engine/RotoContext.{h,cpp}`, `Engine/RotoContextPrivate.h`, `Engine/CMakeLists.txt`
  - approach:
    - Move the geometry-only parts into the base: item and layer management, `makeBezier`/`makeEllipse`/`makeSquare`, selection, auto-key and ripple-edit, feather link, serialization, the content hash, `evaluateChange` and Python field declaration.
    - Only the General page's generic knobs and the Shape page's feather knobs are created by the base. `RotoContext` creates the Stroke, Clone and Transform pages, along with `makeStroke`, the render tree, the bottom merge, the active stroke and `refreshRotoPaintTree`.
    - Keep the `RotoContext` name and API for the paint class, so its ~97 existing call sites compile unchanged.
    - Remove the `dynamic_cast<RotoPaint*>` from the private constructor. The paint context passes its flag explicitly.
  - verify: the build passes and every existing Roto test (`RotoLayer_Test`, `RotoContentHash_Test`, `RotoFeatherDrag_Test`, `CurveTangentJoin_Test`) passes unchanged at the phase build.
  - size: L

- [x] M146.P1.T2 — Move per-curve render-node creation and tree refresh behind paint hooks
  - files: `Engine/RotoCurveContext.cpp`, `Engine/RotoContext.cpp`, `Engine/RotoDrawableItem.cpp`, `Engine/RotoLayer.cpp`, `Engine/RotoSerializationConvert.cpp`
  - approach:
    - Add virtual `onItemCreated(item)` and `onStructureChanged()` hooks to `RotoCurveContext`, both no-ops.
    - `RotoContext` overrides them to call `createNodes()` and `refreshRotoPaintTree()`.
    - Route every current `createNodes` and `refreshRotoPaintTree` call site (the scouted list, minus `RotoPanel`, which is P3.T1) through the hooks, so item construction, paste and load never build nodes on a geometry host.
  - verify: the existing Roto tests pass. A new case in `Tests/RotoLayer_Test.cpp` checks that a RotoPaint Bezier still gets its effect node. The geometry case is P3.T3.
  - size: M

- [x] M146.P1.T3 — Replace the context-creation gate and the generic engine gates with a capability
  - files: `Engine/EffectInstance.h`, `Engine/Node.{h,cpp}`, `Engine/NodeOverlay.cpp`, `Engine/ProjectSerializationConvert.cpp`, `Engine/NodeGroup.cpp`
  - approach:
    - Add a virtual `EffectInstance::getCurveContextKind()` returning `{None, Geometry, Paint}`. It defaults to `Paint` for `RotoPaint` and for the `isRotoNode()` OFX ID, and `None` otherwise.
    - `createRotoContextConditionnally` builds the matching class. Add `Node::getCurveContext()`, returning the base.
    - Switch the generic gates to `getCurveContext()`: `shouldDrawOverlay`, save and restore, Python export and `node.roto`. Paint-only gates keep `getRotoContext()` and `isRotoPaintingNode()`.
  - verify: the existing Roto tests and `Tests/Serialization/*` pass. A project with a RotoPaint node round-trips byte-identically before and after.
  - size: M

- [x] M146.P1.T4 — Move the curve editor's gates to the curve context
  - files: `Gui/CurveEditor.cpp`, `Gui/CurveEditorUndoRedo.cpp`, `Gui/CurveWidget.cpp`
  - approach: replace each scouted `getRotoContext()` check and use with `getCurveContext()`, so a geometry host's curve and control-point animation shows in the curve editor.
  - verify: the build passes. The Xvfb gate check opens a RotoPaint node's curve animation in the curve editor.
  - size: M

## Phase 146.2: Curve-editing interact

- [x] M146.P2.T1 — Extract `RotoCurveInteract` and move the undo commands onto it
  - files: `Engine/RotoCurveInteract.{h,cpp}` (new), `Engine/RotoPaintInteract.{h,cpp}`, `Engine/RotoUndoCommand.{h,cpp}`
  - approach:
    - The base holds the curve tool state (select all/points/curves/feather, add/remove points, remove feather, open-close, smooth, cusp, draw Bezier/BSpline/Ellipse/Rectangle/OpenBezier), the selection, and the built-Bezier state.
    - It is bound to a `RotoCurveContext` and a small host interface: redraw, viewport pixel scale, time, node, `pushUndoCommand`, autosave.
    - `RotoPaintInteract` derives from it, keeps the paint roles, tools and knobs, and its role/tool changes null-check the paint knobs.
    - Undo commands take `RotoCurveInteractPtr`. The two stroke commands stay paint-side.
  - verify: the build passes and the existing Roto tests pass at the phase build. Behaviour is checked at the gate's Xvfb pass.
  - size: L

- [x] M146.P2.T2 — Move the generic parts of the overlay entry points into `RotoCurveInteract`
  - files: `Engine/RotoPaint.cpp`, `Engine/RotoCurveInteract.cpp`, `Engine/RotoPaintInteract.cpp`
  - approach:
    - Move the curve-editing bodies of `drawOverlay`, `onOverlayPenDown/Motion/Up/DoubleClicked`, `onOverlayKey*` and the focus and viewport-selection handlers into `RotoCurveInteract`.
    - Add virtual paint hooks at the scouted paint-specific points (brush circle, eraser, brush tools, stroke build and finish). `RotoPaintInteract` implements them.
    - `RotoPaint`'s overrides become one-line delegations.
  - verify: the build passes. The gate's Xvfb pass covers RotoPaint Bezier drawing and editing, and a paint stroke.
  - size: L

- [x] M146.P2.T3 — Reusable curve toolbar builder
  - files: `Engine/RotoCurveInteract.{h,cpp}`, `Engine/RotoPaint.cpp`
  - approach:
    - Factor the toolbar page and the curve tool-button knob creation out of `RotoPaint::initializeKnobs` into `RotoCurveInteract::createToolbarKnobs(effect, toolSet)`. `toolSet` selects which curve tool groups to create. It returns the page, so the host can append its own tool groups.
    - `RotoPaint` calls it with the full set, then adds the paint groups as today. Keep the default-tool choice (`RotoPaint.cpp:1043-1053`) in the host.
  - verify: RotoPaint's toolbar knob names and order are unchanged. A check in `Tests/RotoLayer_Test.cpp` lists the toolbar page's children before and after.
  - size: M

## Phase 146.3: Panel, viewer slot and proof host

- [x] M146.P3.T1 — `RotoPanel` for any curve host, with a host column hook
  - files: `Gui/RotoPanel.{h,cpp}`, `Gui/NodeSettingsPanel.cpp`, `Gui/DockablePanel.cpp`
  - approach:
    - Create the panel when `getCurveContext()` is non-null.
    - Show the Operator and Color columns only on paint contexts, and route the panel's `refreshRotoPaintTree` calls through `onStructureChanged()`.
    - Add a host hook, a small interface the effect can return, for extra columns (header, per-item text or widget) and extra item context-menu actions.
  - verify: the build passes. The gate's Xvfb pass shows the RotoPaint panel unchanged, and the test host's panel without Operator/Color.
  - size: M

- [x] M146.P3.T2 — One viewer tool slot for every curve host
  - files: `Gui/ViewerTabPrivate.cpp`, `Gui/ViewerTab30.cpp`
  - approach: replace the plugin-ID pair in `findActiveNodeContextForPlugin` with "any node whose curve context kind is not None", so opening any curve host's panel takes the tools from the previous one.
  - verify: the gate's Xvfb pass opens a Roto node, then a RotoPaint node, and only the second has active tools.
  - size: M

- [x] M146.P3.T3 — Test-only geometry curve host and its tests
  - files: `Tests/CurveHost_Test.cpp` (new), `Tests/wmain.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - Register a test-only effect that returns `Geometry`, builds the toolbar with the Bezier and point tools only, and hosts a `RotoCurveInteract`.
    - Test that:
      - `makeBezier` creates no internal nodes (the app's node count and the item's effect node);
      - the content hash changes on a CP move;
      - feather points exist;
      - a project save/load round-trips the curves;
      - `node.roto` resolves from Python;
      - a RotoPaint node in the same project still builds its tree.
  - verify: `CurveHost_Test` passes at the phase build.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including `CurveHost_Test` and every existing Roto and serialization test.
- Xvfb GUI check, with screenshots shared before sign-off:
  - RotoPaint: draw and edit a Bezier, paint a stroke, use the panel.
  - Roto: draw an ellipse.
  - The curve editor shows CP animation.
  - The viewer-slot handover between two curve hosts.
- Run the GUI bench if the diff touches the bench rule's paths.
- An AppImage built with `package.sh` for UAT of Roto/RotoPaint regressions.

## Decisions

- 2026-10-25 — **Panel hook columns are text only (bosley-native-nodes):** Engine can't create widgets; text covers M148's Role and Linked-to columns. The hook lives in `Engine/RotoCurvePanelHook.h`, so M148's panel hook can live with its effect. T1's Xvfb check on the test host moves to M147/M148 with a shipped geometry host (the test host exists only in the Tests binary).
- 2026-10-25 — **Geometry hosts get the full Roto UI (bosley-native-nodes):** the Roto viewer buttons, right-click menu knobs, change slots and `refreshExtraStateAfterTimeChanged` move out of RotoPaint into the curve interact/context, so a non-paint host is complete — added to this milestone rather than deferred to M147.
- 2026-10-25 — **RotoPaint load→save isn't idempotent on main (bosley-native-nodes):** a reloaded RotoPaint loses the clone reset buttons' disabled state and gains `resetTransformCenter`, `RotoTransformInteractive`, `resetTransform` and `blendingModeButton` entries. Fixed here, since M146 owns RotoContext knob creation; the reference test normalises the savedBy commit and the projectPaths temp dir.
- 2026-10-20 — **Split base class, not a mode flag (user, grill):** a geometry-only `RotoCurveContext` and `RotoCurveInteract`, with Roto/RotoPaint layered on top, so the compiler enforces the paint boundary instead of an if-branch on every paint path.
- 2026-10-20 — **One viewer slot for all curve hosts (user, grill):** two interacts never compete for pen events on overlapping curves.
- 2026-10-20 — **Prove it with a test-only host (user, grill):** hosting bugs surface here, not in M147 - SplineWarp Node Core.
- 2026-10-20 — **The item hierarchy stays as is (planner):** only node creation moves. Splitting `Bezier` from `RotoDrawableItem` is a 3,670-line refactor with no SplineWarp need behind it.

2026-10-26: UAT approved (user). Merges with main merged in and PR CI re-run. A pre-existing RotoPaint interactivity problem went to M224 - Fluid Roto And RotoPaint Interaction.
