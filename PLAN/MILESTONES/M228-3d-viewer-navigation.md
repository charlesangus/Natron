# M228 - 3D Viewer Navigation

Make the Viewer's 3D view navigable the way artists expect. Navigation bindings become
rebindable in Natron's shortcut editor, with Nuke (default), Maya, Blender and Houdini
presets. The view also gains frame-selected and frame-all, multi-select with marquee,
ortho views, a look-through lock, user clip planes, and create-camera-from-view. From the
M19/M20 UAT feedback (2026-10-26); survey in `PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.
Code: `Gui/SceneViewport.{h,cpp}` (today: plain-LMB tumble, MMB/Shift-LMB pan, Alt-RMB and
wheel dolly, `F` = frame all, a single selected path, fixed near/far planes).

## Scope

**In:**
- Every 3D navigation gesture is a bound action in `Gui/ActionShortcuts` (mouse button + modifiers, plus keys), edited in the existing shortcut editor. A Preferences chooser applies the Nuke (default), Maya, Blender or Houdini preset.
- Plain LMB click selects. Shift adds, Ctrl toggles, and LMB-drag on empty space marquees. The selection becomes a set of prim paths.
- `F` frames the selection (everything when empty), and `A` frames all. Both are rebindable.
- Perspective plus Front/Side/Top orthographic views, following the stage's up axis.
- Look-through lock. With the lock on, navigation moves the looked-through camera by writing its node's knobs (one undo step per drag), or is refused with a HUD message when the camera has no knobs to write (a file camera). With it off, navigating leaves look-through as today.
- Near/far clip for the free camera (view menu, saved per viewer).
- "Create camera from view": a `USDCamera` node matching the current free camera (transform, focal length, aperture).

**Out:**
- Camera frustums, light icons and display toggles — M229 - 3D Viewer Guides And Display.
- Transform widgets and their shortcuts — M230 - 3D Transform Widgets.
- Scene graph panel selection sync — M231 - Scene Graph Panel.
- 2D-viewer navigation presets — none.

**Edge cases:**
- Right-click under a preset that uses RMB for dolly (Maya/Nuke Alt-RMB): the context menu opens only on a plain RMB click without drag. The stuck-dolly bug is fixed by M20 - 3D Node Vocabulary's UAT fix, and this milestone's bindings must not reintroduce it (state cleared on menu exec).
- Frame selection on a prim with an empty or unbounded bbox (a camera, a light, a scope): frame its world position with a default radius.
- Ortho view while looking through a perspective camera: leaves look-through.
- Lock on with a camera whose transform is animated: the drag writes a key at the current frame when the knob is animated, otherwise sets the value.
- Create camera from view in a Z-up project: the authored transform is in the project's axes.

**Assumptions:**
- `MouseAction` in `Gui/ActionShortcuts.h` can carry modifier+button bindings for the shortcut editor. Today it has a fixed, non-editable button, so making it editable is part of M228.P1.T1.

## Phase 228.1: Bindings

- [x] M228.P1.T1 — Rebindable mouse navigation actions
  - files: `Gui/ActionShortcuts.{h,cpp}`, `Gui/PreferencesPanel.{h,cpp}` (the shortcut editor page), `Gui/GuiApplicationManager*.cpp` (action registration), `Tests/` (a shortcut serialization gtest)
  - approach: make `MouseAction`'s button and modifiers editable and persisted like key shortcuts, then show mouse bindings in the shortcut editor with a "press a mouse combo" capture. Register a `3DViewer` group: tumble, pan, dolly (drag), dolly (wheel), select, add to selection, toggle selection, marquee, frame selection, frame all, and the ortho views.
  - verify: gtest — a rebound mouse action round-trips through the shortcut settings; Xvfb GUI script — the shortcut editor lists the 3DViewer group and rebinding tumble to Alt+LMB takes effect in a 3D view.
  - size: L

- [x] M228.P1.T2 — Navigation presets and the SceneViewport dispatch
  - files: `Gui/SceneViewport.{h,cpp}`, `Engine/Settings.{h,cpp}` (preset choice), `Gui/ActionShortcuts.cpp` (preset tables)
  - approach: SceneViewport resolves press/move/release through the 3DViewer bound actions instead of hard-coded buttons. Presets: Nuke (Alt+LMB tumble, Alt+MMB pan, Alt+RMB / wheel dolly, LMB select), Maya (same with Alt), Blender (MMB tumble, Shift+MMB pan, Ctrl+MMB/wheel dolly), Houdini (Space/Alt+LMB tumble, +MMB pan, +RMB dolly). Choosing a preset in Preferences rewrites the group's bindings; later edits make it "Custom". Clear drag state on focus-out and on context-menu exec.
  - verify: Xvfb GUI script — under each preset, synthetic drags tumble/pan/dolly as tabled and a plain LMB click selects; a right-click menu followed by mouse moves does not move the camera.
  - size: M

## Phase 228.2: Selection and framing

- [x] M228.P2.T1 — Multi-selection and marquee
  - files: `Gui/SceneViewport.{h,cpp}`, `Tests/gui/scene_viewer_select.py`
  - approach: replace the single `selected` path with an ordered set (the last-clicked path is "current"). Click replaces the set, Shift adds, Ctrl toggles. A marquee on empty space selects the prims whose pick IDs fall in the rectangle (`TestIntersection` over the rect, or the primId AOV under it). Highlight all through Storm's selection. Emit `selectionChanged(QStringList)` replacing `selectedPrimChanged(QString)`.
  - verify: Xvfb GUI script on a 3-prim scene — click, shift-click, ctrl-click and a marquee each give the expected set (read through a test hook).
  - size: M

- [x] M228.P2.T2 — Frame selection and frame all
  - files: `Gui/SceneViewport.cpp`
  - approach: `frameSelection()` takes the union world bbox of the selected prims (default+proxy purposes). A degenerate bbox frames the prim's world origin with a default radius. `frameScene()` stays as frame-all. Bound to the M228.P1.T1 actions, plus context-menu entries.
  - verify: Xvfb GUI script — after selecting a far prim, frame selection puts its bbox centre at the view centre (pick at the centre returns it); with an empty selection it frames all.
  - size: S

## Phase 228.3: Views and cameras

- [x] M228.P3.T1 — Orthographic views and user clip planes
  - files: `Gui/SceneViewport.{h,cpp}`
  - approach: a view-mode state (Perspective, Front, Side, Top) on the free camera. Ortho modes use an orthographic `GfCamera` aligned to the stage's up axis, pan and dolly scale the ortho extent, and tumble returns to perspective. Near/far knobs live in the view menu, persisted per viewer in its settings. Defaults stay the current distance-relative values, and setting them switches to absolute.
  - verify: Xvfb GUI script — Top view of a cube shows a square silhouette (a pixel test at the corners); a near clip beyond the cube hides it.
  - size: M

- [x] M228.P3.T2 — Look-through lock
  - files: `Gui/SceneViewport.{h,cpp}`, `Gui/ViewerTab.cpp` (finding the camera's owning node in the viewed branch)
  - approach: a lock toggle in the view menu/HUD. When locked and looking through a camera whose prim was authored by a `USDCamera` in the viewed branch (match the node's Prim Path), navigation drags compute the new camera transform and write that node's TRS knobs, as one undo entry per drag, keying when animated. A file camera with no owning node refuses with a HUD message. Unlocked keeps today's behaviour (navigation leaves look-through).
  - verify: Xvfb GUI script — locked tumble on a USDCamera changes its rotate knob and Undo restores it; on a USDRead camera the view doesn't move and the HUD message shows.
  - size: M

- [x] M228.P3.T3 — Create camera from view
  - files: `Gui/SceneViewport.cpp`, `Gui/ViewerTab.cpp`, `Gui/NodeGraph*` only for placing the new node
  - approach: context-menu action creating a `USDCamera` node whose transform, focal length and aperture reproduce the current free camera (converting with the project's up axis/units). It is placed near the viewed node in the graph, unconnected (create nodes have no inputs, M226 - Scene Hierarchy From Topology).
  - verify: Xvfb GUI script — after navigating, create the camera, merge it into the viewed scene and look through it: the image matches the pre-creation view within tolerance.
  - size: M

**Prior verification gate:** release gate green; scene GUI scripts (select, navigation presets, ortho, lock, create camera) pass under Xvfb; screenshots of each preset's shortcut-editor page and an ortho view shared before sign-off; user tries the Nuke preset on real hardware.

## Phase 228.4: UAT round 1 fixes (user, 2026-10-27)

- [x] M228.P4.T1 — Nuke preset: plain drag marquee-selects
  - files: 3D viewer navigation bindings and GUI test
  - approach: Keep Ctrl-drag for tumble in the Nuke preset and make plain left-drag on empty space marquee-select while a click still selects.
  - verify: The Nuke-preset GUI test covers click, plain marquee and Ctrl-drag tumble.
  - size: M
- [x] M228.P4.T2 — Locked look-through viewport performance
  - files: locked-camera navigation path, camera authoring, 3D GUI bench
  - approach: Profile the locked path and remove per-event camera authoring, engine rebuild or full-scene resync so it is as fast as the free camera.
  - verify: The locked-view bench remains within the signed-off delta from free navigation.
  - size: M
- [x] M228.P4.T3 — One-step lock
  - files: 3D viewer camera menu and GUI test
  - approach: With no current look-through camera, lock directly when the scene has one camera and open a camera menu when it has several; expose the same list in the look-through control.
  - verify: GUI coverage checks zero, one and several cameras and reaches the locked state in one action.
  - size: M
- [ ] M228.P4.T4 — Rebuild, AppImage, re-UAT
  - files: release build artifacts, M228 UAT guide
  - approach: Run the release and GUI gates, package the AppImage and refresh the UAT checklist and screenshots.
  - verify: Gates are green and the replacement artifact launches on the target host.
  - size: S

## Decisions

- 2026-10-28: GUI checks pass on Actions (M269 remote runs); AppImage and UAT doc in build/appimages/, contact sheet build/uat-shots/; awaiting user UAT.
- 2026-10-27 — Gate run 40531328785: only failure was RenderIsolation timing out after all 12 cases passed (hang at process exit, taskgraph leg; ~1 s everywhere else). Treated as a flake; M229's gate (contains M228 HEAD) is the re-run. If it recurs, file a shutdown-deadlock milestone.
- 2026-10-27 — A plain left-drag marquee-selects wherever it starts, in every preset (Nuke docs: "click and drag a marquee"); a click selects, Ctrl+drag tumbles. Locked navigation: the view draws a transient camera during the gesture and writes the camera knobs once on release (one undo step); wheel runs commit 300 ms after the last step. Root cause was a knob write → evaluate → full scene re-sync per mouse event.
- 2026-10-27 — UAT round 1 (user): fixes in Phase 228.4. Lock with several cameras offers a menu (PM call: one camera locks directly).
- 2026-10-27 — **Choices (bosley-native-nodes):** Nuke preset follows the Nuke 17 docs (Ctrl+left tumble, Alt+left pan, Alt+middle dolly; plain middle-drag pan added); Back/Left/Bottom views added; clip planes per viewer, not saved; a camera created from the view is not auto-connected (as Nuke). Release gate 568/568; GUI checks pass; draft PR #137; UAT pending.

## Phase 228.5: UAT round 2 fixes

- [ ] M228.P5.T1 — Keep locked-view camera knobs synchronized
  - files: `Gui/SceneViewport*`, camera node knob synchronization, 3D viewer GUI test
  - approach: Commit navigation changes back to the locked camera as one undoable edit and refresh its visible knobs immediately, without reintroducing per-motion evaluation stalls.
  - verify: Navigate while locked, then inspect and undo the camera knobs; values and view remain consistent.
  - size: M

- [ ] M228.P5.T2 — Fix NVIDIA USDRender crashes in the UAT scene
  - files: USDRender/Hydra GL lifecycle, crash regression harness, M228 UAT fixture
  - approach: Reproduce with `M228-r2-scene.ntpz` and the supplied crash log, isolate the invalid GL/Hydra lifetime or command path, and fail safely if the driver cannot execute it.
  - verify: The supplied scene renders repeatedly on the NVIDIA target without a segfault and still passes llvmpipe coverage.
  - size: L

**Verification gate:** release and 3D GUI suites pass; the supplied NVIDIA scene is crash-free; camera knobs track locked navigation; the user accepts the replacement AppImage.
