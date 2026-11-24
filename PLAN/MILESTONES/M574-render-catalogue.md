# M574 - Render Catalogue

A per-project strip of render snapshots. Each snapshot holds the image the viewer showed, a name, a star and notes, plus the graph state that made it. Any snapshot can be shown as the viewer's A or B side, compared with the wipe and compositing operators the viewer already has, and restored to the graph as one undo step (user, 2026-11-24: "Images + graph state"). Nothing like it exists today. The parts it builds on:
- Graph state: `Project::toSerialization` → `Serialization::editableState()` (`Serialization/Ops/SnapshotDiff.h`). That state drops the playhead, cache IDs and knob ages, so two captures of an unchanged graph compare equal.
- Restore: `Serialization::diffSnapshots(before, after)`, applied through `ProjectMerge::applyAllOrNothing` (`Engine/ProjectMerge.h`) and recorded with `ApplyGraphOpsCommand::applyAndPush` (`Gui/NodeGraphUndoRedo.h`). These are M104 - Merge Resolution UI's undo glue, now on `milestone/m104-merge-resolution-ui` / F33.
- Bundles: `ProjectBundleWriter`/`ProjectBundleReader` (`Serialization/Bundle/ProjectBundle.h`) encode a state with its roto and tracker blobs.
- Viewer A/B: the `aInput`/`bInput` int knobs (`kViewerParamAInput`/`kViewerParamBInput`, `Engine/ViewerInstance.cpp` ~:866), `ViewerTab::setInputA/B` (`Gui/ViewerTab30.cpp` ~:937), the wipe knobs, and `ViewerCompositingOperatorEnum` (`Global/Enums.h:421`: Wipe/Stack × Under/Over/Minus/OnionSkin).
- Where renders land: an image input's pixels go through the node render path into the cache. What the viewer shows is `ViewerGL::getLastRenderedImage` (mip level and RoI only). A scene input renders in `Gui/SceneViewport.cpp`, whose linear colour AOV is `engine->GetAovTexture(HdAovTokens->color)` (~:2555). USDRender's output is an ordinary image input.
- Panels: the new panel registers the way `SceneGraphPanel` does (`Gui::addNewSceneGraphPanel`, `Gui/Gui20.cpp` ~:623; `Gui/Gui05.cpp` ~:126).
- Image files: `Engine/Nodes/IO/OiioWriteSupport` and NativeRead/NativeWrite.
- Render jobs: `AppInstance::startWritersRendering(RenderModeEnum, RenderWork list)` (`Engine/AppInstance.h` ~:117, ~:422). Foreground jobs report through `RenderEngine::frameRendered`/`renderFinished` (`Engine/OutputSchedulerThread.h` ~:816, ~:824). Background jobs run in a separate process on the project as saved, through `ProcessHandler` (`Engine/ProcessHandler.h`: `frameRendered`, `processFinished`, `processCanceled`). A writer's output path for a frame is `KnobOutputFile::generateFileNameAtTime` (`Engine/KnobFile.h:284`). USDRender (`Engine/Nodes/Scene/USDRender.{h,cpp}`) has no render job or output path of its own. It is a pull node: `USDRender::render` (~:1130) and `renderGpu` (~:1304) render whole frames (`supportsTiles` false) through `HydraRender::render`, running to convergence, whenever a viewer or a Write asks for a frame.

## Scope

**In:**
- A **Render Catalogue** dockable panel. It shows a horizontal strip of snapshot thumbnails with the name under each, a star toggle and A/B badges. It opens from the pane menu like the Scene Graph panel and fits 1920x1080 docked under the viewer.
- **Snap** (toolbar button and shortcut): snapshots the active viewer's A side at the current frame and view.
  - An image input is rendered at full resolution (mip 0) over its format, with every layer it produces, off the main thread and cancellable. Pixels already cached are reused.
  - A scene input (the 3D viewport) captures the viewport's last presented linear colour AOV at viewport size, beauty only.
  - Snap also captures the graph state at that moment.
- Per snapshot: an editable **name** (default `<source node> f<frame>`), a **star**, free-text **notes**, the creation time, the source node (ID and name), frame, view, render time, resolution and layer names. The metadata shows in the thumbnail tooltip.
- Strip actions:
  - Click selects, Ctrl toggles, Shift extends. Double-click the name to rename. Del deletes.
  - Context menu: Rename, Star/Unstar, Set as A, Set as B, Stop Comparing, Restore Graph, Go to Frame, Select Source Node, Export Image…, Delete, Delete Unstarred.
  - Toolbar: Snap, A, B, Starred Only, a text filter (name and notes), and the storage path label.
- **A/B compare in the viewer:** either side of the active viewer can show a snapshot instead of an input. Switching between snapshots never re-renders the graph. The existing wipe (centre, angle, mix) and compositing operators, layer and channel menus, gain, gamma, colour picker and histogram all apply unchanged.
- **Restore Graph:** puts the project back to the snapshot's graph state as one undo step named "Restore Graph to <name>". Ops are applied all or nothing, and a failure leaves the graph unchanged and posts a non-modal message. Viewer nodes are left as they are, so the comparison survives the restore.
- **Export Image…:** writes the snapshot's pixels to a chosen path in any writable format (`OiioWriteSupport::isWritablePath`). EXR keeps every layer.
- **Auto snapshots** (Auto toolbar toggle, off by default), with options in a non-modal popup that are saved with the catalogue:
  - Limit to One Snapshot Every N Seconds (default 10) and Wait N Seconds After a Change (default 2).
  - An auto snap fires when the active viewer's A side finishes re-rendering after a graph edit. Pan, zoom and timeline moves don't trigger one.
  - Auto entries carry an `auto` flag and appear collapsed in the strip, except the newest. Hovering or selecting one shows it in full.
  - A Show Non-Auto toolbar filter hides them. Starring one, or renaming it, makes it a normal entry.
- **USDRender to the catalogue, no Write needed (user):** a Render To Catalogue button (`renderToCatalogue`) on USDRender renders the node's current frame and view at full resolution, with every layer, to convergence, in the session on a worker. While it renders it is a live entry with a rendering badge and Stop Rendering, and the result becomes the entry's image. Its graph state is captured when the button is pressed. The button is hash-exempt and never triggers a render of its own node.
- **Render-job snapshots:** a writer render (Write or NativeWrite, whatever feeds it, USDRender included), foreground or background, adds one live entry while it runs.
  - Its thumbnail follows the frame nearest the timeline's frame as it is written, read back from the written file, and shows a rendering badge and the job's progress.
  - Stop Rendering in its context menu cancels the job, and the entry keeps the last frame written.
  - The entry's graph state is the state the job renders: the live state for a foreground job, the saved project file for a background job.
  - A Catalogue Renders toolbar toggle (on by default) turns this off.
- Snapshots persist with the project in a folder beside it, `<projectdir>/<stem>.catalogue/` (user). An unsaved project keeps them in a session temp folder until its first save.
- `docs/render-catalogue.md` (user-facing description plus the storage layout), and a params→feature audit table in the PR body.

**Out:**
- Farm submission and a separate Live Render streaming node. Render jobs cover background renders. No milestone.
- Per-entry render logs: the progress panel keeps a job's log. No milestone.
- Snapshots of writers that write no image (USDWrite, DeepWrite's deep samples): there are no pixels to show.
- Tags, colour labels and the filter sidebar (any/all, by colour, by tag, by creation date) (user). Star, the text filter, Starred Only and Show Non-Auto cover finding snapshots.
- Katana slots, stacks and locks, moving entries between slots, Lock 2D, Duplicate Catalog Item, Regenerate Thumbnail, Adopt Render ROI. One strip with stars replaces slots and locks, and snapshots are immutable.
- Import Image / Sequence into the catalogue. Read nodes load files.
- Viewer comparison modes beyond today's operators: absolute difference, highlight-differences threshold, red/cyan anaglyph, horizontal/vertical split panes, synced pan/zoom across two monitors. Wipe plus Minus/OnionSkin covers split, subtract and blend. New viewer operators are viewer work, not catalogue work.
- Render-stats panels (memory, time to first pixel, ray counts): Natron's delegates report none. Render time is kept.
- A Catalogue graph node and CatalogueSelect (Gaffer's node form): snapshots reach the viewer through a hidden source instead (user). A Python API for the catalogue: no milestone.
- A graph diff preview before restoring (M104's compare overlay could show one): no milestone.

**Edge cases:**
- Snap with no viewer, or with side A empty: the button is disabled and its tooltip says why.
- Snap during a render in progress (a progressive delegate in USDRender or the viewport) captures the image as it stands when Snap is pressed: the full-resolution request for an image input, or the last presented viewport frame. A snap still rendering shows a busy thumbnail and can be cancelled from its context menu. Cancelling discards the entry.
- The source node is deleted after the snap: the entry keeps the name it recorded. Select Source Node is disabled.
- A snapshot whose graph state names a plug-in that is no longer installed: Restore fails as a whole with the op error, and the graph stays as it was.
- Restore with the project unchanged since the snap: there are no ops and no undo entry, and a status message says nothing changed.
- Undo after a restore brings back the exact pre-restore graph (`ApplyGraphOpsCommand` inverse ops).
- A project's timeline position, cache IDs and knob ages are not restored (`editableState` drops them).
- A snapshot shown as A or B is deleted: that side falls back to none.
- Deep images: the snap stores the flattened image the viewer shows. Deep samples are not kept.
- A missing or corrupt image file on reopen: the entry shows a broken thumbnail and keeps its graph state, so Restore still works. One non-modal warning lists the missing files.
- Several viewers: Snap and A/B act on the active (last focused) viewer.
- Large snapshots (4K multi-layer float): the image is written off the main thread, and the UI never waits on it.
- An auto snap is pending when a newer edit arrives: the pending one is cancelled and the wait restarts. An auto snap whose graph state equals the previous entry's is skipped.
- An auto snap while a render-job entry is live: the auto snap is taken as normal; the two entries are independent.
- A render job whose frame range doesn't contain the timeline frame follows its first frame. A multi-view job follows the viewer's current view.
- A background job's process crashes: the entry keeps the last frame read and notes the failure. A job writing a format OIIO can't read back keeps a busy thumbnail and notes why.
- Render To Catalogue pressed again while the node's previous one is live: a second entry starts. Each renders independently and stops on its own.
- Render To Catalogue on a USDRender that cannot render (no settings prim, a missing renderer, no GL context for Storm): the entry is not added, and the node shows its usual error.
- A Render To Catalogue entry's thumbnail stays busy until the frame converges: `HydraRender::render` publishes no intermediate passes.
- The project is saved elsewhere (Save As) while a job is live: the entry moves with the catalogue, and the job's frames still land.

**Assumptions:**
- M104's `ApplyGraphOpsCommand` and `ProjectMerge::applyAllOrNothing` land on main unchanged (they are on F33 today).
- An out-of-project node (`kCreateNodeArgsPropOutOfProject`) can feed a viewer side without being serialised or appearing in the node graph. P3.T1 checks this first. If it cannot, the consultant chooses another route that keeps the source hidden and never saved.
- The background process writes frames to the writer's path as the project saved it, so reading the file back after `ProcessHandler::frameRendered` sees the frame complete.
- The scene viewport's colour AOV texture can be read back on the GL thread without stalling presentation (one `glGetTexImage` per snap).

## Phase 574.1: Catalogue engine

- [ ] M574.P1.T1 — Catalogue entries and their store
  - files: `Engine/RenderCatalogue.{h,cpp}`, `Engine/CMakeLists.txt`, `Tests/RenderCatalogue_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - `CatalogueEntry` holds the In metadata: id (32-hex), name, starred, notes, createdAt, sourceNodeId, sourceNodeName, frame, view, renderMs, format, layers, plus the relative paths of its image, its thumbnail and its graph state.
    - `RenderCatalogue` is a per-project list with add/remove/rename/setStarred/setNotes/entries/find. It emits Qt signals on every change.
    - Storage sits behind `CatalogueStorage`, implemented by a directory store: an `index.yaml` (the entries, with a format version) and one folder per entry (`<id>/image.exr`, `<id>/thumb.png`, `<id>/graph.ntpz`). Writes go to a temp name and are renamed into place.
    - The root folder is `<projectdir>/<stem>.catalogue/` (user), given by the caller. A store with no root yet keeps its files in a session temp folder and `moveTo(root)` relocates them.
  - verify: gtest — add three entries (one flagged auto), rename one, star one, remove one, reload from the folder and compare. moveTo carries the files across. A hand-truncated index.yaml loads its readable entries and reports one warning. A missing image keeps the entry with an `imageMissing` flag.
  - size: M

- [ ] M574.P1.T2 — Capture and restore graph state
  - files: `Engine/RenderCatalogueState.{h,cpp}`, `Tests/RenderCatalogueState_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - `captureGraphState(project)` runs `toSerialization` → `editableState` and returns the bundle bytes from `ProjectBundleWriter`, which keeps roto and tracker blobs as members.
    - `restoreOperations(project, bytes, &ops, &error)` reads the bundle with `ProjectBundleReader` and diffs the live editable state against it (`diffSnapshots`). It removes ops whose target is a Viewer node, and Viewer-node creations and deletions too.
  - verify: gtest on a live project (as `Tests/ProjectMerge_Test.cpp` sets one up), sketched as capture → edit → restore → compare:
    - Capture a Constant → Blur → Viewer graph. Then change Blur's size, add a Grade, delete the Constant, and change the Viewer's gain.
    - Apply the restore ops with `applyAllOrNothing`. The editable state now equals the captured one, except the Viewer's gain, which keeps its new value.
    - An unchanged project gives no ops. A state naming an unknown plug-in fails and changes nothing.
  - size: M

- [ ] M574.P1.T3 — Snap an image input
  - files: `Engine/RenderCatalogueCapture.{h,cpp}`, `Tests/RenderCatalogueCapture_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - `snapImageInput(node, frame, view, colourContext)` renders the node on a worker at mip 0 over its format, with every layer it produces, through the same render request the Write path uses. It honours the abort flag.
    - It writes a float zip EXR through OIIO with layer-named channels. It also writes a PNG thumbnail (256 px long side) in the active viewer's display/view, plus render time, resolution and layer names.
    - Pixels already cached are reused.
  - verify: gtest — snap a CheckerBoard and a two-layer graph (a Shuffle adding a layer). Read the EXR back with OIIO: the pixels equal the node's render and both layers are present. An abort mid-render returns cancelled and leaves no files.
  - size: L

- [ ] M574.P1.T4 — Snap the scene viewport
  - files: `Gui/SceneViewport.{h,cpp}`, `Engine/RenderCatalogueCapture.{h,cpp}`, `Tests/gui/m574_catalogue_scene_snap.py`
  - approach:
    - `SceneViewportRenderer::grabPresentedColour(callback)` reads back the last presented `HdAovTokens->color` texture as linear float RGBA at viewport size. It runs on the GL thread, after the next present, never blocking the UI.
    - The capture module writes it as a one-layer EXR plus thumbnail, the same way as P1.T3.
  - verify: an Xvfb script builds USDSphere → viewer (Storm) and grabs the colour. The EXR's size equals the viewport's, and its centre pixel is non-black while its corner is the background colour.
  - size: L

- [ ] M574.P1.T5 — Snapshots from render jobs
  - files: `Engine/RenderCatalogueJobs.{h,cpp}`, `Engine/AppInstance.cpp`, `Engine/RenderCatalogue.{h,cpp}`, `Tests/RenderCatalogueJobs_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - Add a `RenderCatalogueJobs` watcher that `AppInstance::startWritersRendering` notifies for each `RenderWork` it starts, in every mode.
    - It skips writers that write no image. It adds a live entry with the In metadata, and captures graph state with `captureGraphState` for a foreground job. For a background job it reads the saved project (the bundle at `ProcessHandler::getProjectPath()`) through `ProjectBundleReader` → `editableState`.
    - It connects `RenderEngine::frameRendered`/`renderFinished`, or `ProcessHandler::frameRendered`/`processFinished`/`processCanceled`.
    - When the followed frame (nearest the timeline frame, else the first) is written, a worker reads the file from `KnobOutputFile::generateFileNameAtTime` with OIIO, then stores the EXR and thumbnail as P1.T3 does. Progress and the live flag update on each frame.
    - `stopJob(entryId)` aborts a foreground job or cancels the process.
  - verify: gtest — a blocking render (`eRenderModeBlocking`) of CheckerBoard → NativeWrite over frames 1-3, with the timeline at 2, yields one entry whose EXR equals frame 2 and whose graph state equals the captured one; live is false after finish. A Write fed only by a USDWrite-style non-image writer adds none. Stopping a foreground job mid-range keeps the last frame written.
  - size: L

- [ ] M574.P1.T6 — Auto snapshot scheduler
  - files: `Engine/RenderCatalogueAuto.{h,cpp}`, `Tests/RenderCatalogueAuto_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - `AutoSnapScheduler` takes graph-edit notices and viewer-render-finished notices, plus an injectable clock and timer.
    - It fires a snap request once the viewer has finished a render that started after the last edit, and at least `wait` seconds after that edit, and at most once per `limit` seconds.
    - A newer edit cancels a pending request.
    - It skips the request when the captured graph state equals the newest entry's.
    - `limit`, `wait` and `enabled` are saved in `index.yaml`.
  - verify: gtest with a fake clock. Edits at t=0 and t=1 followed by a render finished at t=1.5 give one request at t=3 (wait 2). Edits every second for 30 s give at most one request per 10 s. A pan-only render (no edit) gives none. An unchanged state is skipped.
  - size: M

- [ ] M574.P1.T7 — Render To Catalogue on USDRender
  - files: `Engine/Nodes/Scene/USDRender.{h,cpp}`, `Engine/RenderCatalogueJobs.{h,cpp}`, `Engine/Nodes/Scene/docs/USDRender.md`, `Tests/Scene/USDRenderCatalogue_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - Add a `renderToCatalogue` button (label "Render To Catalogue", hash-exempt, no evaluate on change) in `USDRender::initializeKnobs`.
    - Its `knobChanged` calls `RenderCatalogueJobs::startNodeRender(node, frame, view)`. That adds a live entry with graph state captured now, runs `snapImageInput` (P1.T3) on the USDRender node, at mip 0 with all layers, on a worker, and completes the entry. Abort goes through the shared `stopJob(entryId)`.
    - Document the button in `USDRender.md` without naming another application.
  - verify: gtest with the delegate the existing `Tests/Scene/USDRender*_Test` use (Cycles at a low sample cap). Pressing the button on a USDSphere → USDRender graph adds one live entry that ends non-live, and its EXR equals the node's full-resolution render at that frame. `stopJob` mid-render keeps no image and marks the entry stopped. A USDRender naming a missing renderer adds no entry.
  - size: M

## Phase 574.2: Catalogue panel

- [ ] M574.P2.T1 — The strip panel and Snap
  - files: `Gui/RenderCataloguePanel.{h,cpp}`, `Gui/Gui05.cpp`, `Gui/Gui20.cpp`, `Gui/GuiPrivate.h`, `Gui/CMakeLists.txt`, `Tests/gui/m574_catalogue_panel.py`
  - approach:
    - A dockable panel registered like `SceneGraphPanel`: created, saved in layouts and reachable from the pane menu.
    - A horizontal `QListView` in icon mode over a model of `RenderCatalogue` entries: thumbnail, name, star, A/B badges.
    - The toolbar has Snap, A, B, Starred Only, a filter field and the path label.
    - Snap picks the active viewer's side A. It routes an image input to P1.T3 and a scene input to P1.T4, captures graph state (P1.T2), and adds the entry with a busy thumbnail until the capture finishes.
    - Selection rules, inline rename and Del are as In.
  - verify: an Xvfb script at 1920x1080 docks the panel under the viewer, snaps twice, renames one entry, stars it, checks Starred Only shows one entry and filters by text, then deletes one. Screenshots of the strip go to the user.
  - size: L

- [ ] M574.P2.T2 — Context menu actions
  - files: `Gui/RenderCataloguePanel.cpp`, `Tests/gui/m574_catalogue_panel.py`
  - approach:
    - The context menu offers Rename, Star/Unstar, Go to Frame (sets the timeline), Select Source Node (selects and centres it by ID, disabled when gone), Delete, Delete Unstarred (with an undo-less confirmation in the panel's message line, no modal), and cancel for a busy snap.
    - A Notes field edits the notes in a small popup. The tooltip shows the metadata.
  - verify: the Xvfb script snaps at frame 5, moves to frame 1, runs Go to Frame and asserts frame 5. It runs Select Source Node and asserts the selection. It deletes the source node and asserts the action is disabled. Delete Unstarred leaves only the starred entries.
  - size: M

- [ ] M574.P2.T3 — Export Image
  - files: `Gui/RenderCataloguePanel.cpp`, `Engine/RenderCatalogueCapture.{h,cpp}`, `Tests/RenderCatalogueCapture_Test.cpp`
  - approach: `exportSnapshot(entry, path)` copies the EXR when the target is `.exr` and otherwise converts its pixels to the extension's format through OIIO (beauty layer for single-layer formats). The panel's file dialog checks `OiioWriteSupport::isWritablePath`.
  - verify: the gtest exports a two-layer snapshot to `.exr` (both layers present) and to `.png` (RGBA, sized like the source). An unwritable extension is refused.
  - size: M

- [ ] M574.P2.T4 — Auto snapshots in the panel
  - files: `Gui/RenderCataloguePanel.{h,cpp}`, `Gui/ViewerTab.cpp`, `Tests/gui/m574_catalogue_auto.py`
  - approach:
    - The Auto toggle and an Options popup (non-modal, saved with the catalogue) hold Auto, Limit to One Snapshot Every N Seconds and Wait N Seconds After a Change.
    - Feed `AutoSnapScheduler` (P1.T6) the project's graph-edit notices and the active viewer's render-finished signal for side A, and run its requests through the Snap path with the auto flag.
    - Auto entries are drawn collapsed, except the newest, and expand on hover or selection. The Show Non-Auto filter hides them. Starring or renaming one clears its flag.
  - verify: an Xvfb script turns Auto on with Wait 1 and Limit 2 and changes a Grade's gain three times within a second. It asserts one auto entry appears, then that a further edit 3 s later adds a second. Show Non-Auto hides both, and starring one keeps it visible. Screenshots of the collapsed strip go to the user.
  - size: M

- [ ] M574.P2.T5 — Live render-job entries in the panel
  - files: `Gui/RenderCataloguePanel.{h,cpp}`, `Tests/gui/m574_catalogue_jobs.py`
  - approach:
    - Live entries from P1.T5 show a rendering badge and the job's progress, and their thumbnail refreshes as frames land.
    - The context menu adds Stop Rendering for live entries.
    - The Catalogue Renders toolbar toggle turns the P1.T5 watcher on and off.
  - verify: an Xvfb script presses Render To Catalogue on a USDRender and asserts a live entry that finishes. It then saves a project and starts a background render (`eRenderModeBackground`) of CheckerBoard → Write over frames 1-5, and asserts a live entry appears and finishes with a non-live thumbnail equal to the written timeline frame. It starts a foreground render, runs Stop Rendering and asserts the job stops and the entry stays. With Catalogue Renders off, a render adds nothing.
  - size: M

## Phase 574.3: Viewer compare

- [ ] M574.P3.T1 — A snapshot as a viewer side
  - files: `Engine/ViewerInstance.{h,cpp}`, `Engine/RenderCatalogue.{h,cpp}`, `Tests/RenderCatalogueViewer_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - Each viewer side can hold a snapshot ID beside its `aInput`/`bInput` index. While it does, the viewer renders that side from a hidden, never-saved (user) out-of-project NativeRead of the snapshot's EXR (`kCreateNodeArgsPropOutOfProject`), not serialised and not in the node graph.
    - The snapshot IDs are viewer state, not graph state: they are hash-exempt and saved with the viewer's UI state.
    - Clearing a side, or deleting its snapshot, falls back to none.
    - The existing wipe, compositing operators and layer menus act on it as on any input.
  - verify: gtest — set side A to a snapshot of a red Constant and side B to a live green Constant. With Wipe Minus, the viewer render's centre pixel is red minus green. Deleting the snapshot sets side A to none. Saving and reloading the project leaves no extra node in the graph.
  - size: L

- [ ] M574.P3.T2 — Set as A/B from the strip and the viewer menus
  - files: `Gui/RenderCataloguePanel.cpp`, `Gui/ViewerTab30.cpp`, `Gui/ViewerTab.cpp`, `Tests/gui/m574_catalogue_compare.py`
  - approach:
    - The A and B buttons and the Set as A, Set as B and Stop Comparing actions set the active viewer's side. Selecting two snapshots and pressing A sets both, first as A and second as B.
    - The viewer's A and B input menus list the catalogue's snapshots after the inputs, under a separator, and show the active one by name.
    - Setting B turns on the Wipe operator if it is None.
  - verify: an Xvfb script snaps two looks, sets them as A and B, and asserts the viewer's A/B menus show both names and the operator is Wipe. It reads the colour picker on each side of the wipe line. Stop Comparing clears B. Screenshots of the compare go to the user.
  - size: M

## Phase 574.4: Restore and persistence

- [ ] M574.P4.T1 — Restore Graph
  - files: `Gui/RenderCataloguePanel.cpp`, `Tests/gui/m574_catalogue_restore.py`
  - approach:
    - Restore Graph pushes `restoreOperations` (P1.T2) through `ApplyGraphOpsCommand::applyAndPush` with the text "Restore Graph to <name>".
    - A failure, or no change, shows in the panel's message line, never a modal.
    - The node graph and open properties panels refresh as M104's apply does.
  - verify: an Xvfb script snaps, changes a Grade's gain, adds a node and deletes one, then restores. It asserts the graph matches the snap and that the comparison sides are unchanged. One undo restores the edited graph exactly, and redo applies the restore again.
  - size: M

- [ ] M574.P4.T2 — Persist the catalogue with the project
  - files: `Engine/Project*.cpp` (save/load hooks), `Engine/RenderCatalogue.{h,cpp}`, `Tests/RenderCatalogue_Test.cpp`, `Tests/gui/m574_catalogue_panel.py`
  - approach:
    - On project load, open `<projectdir>/<stem>.catalogue/` (user). On save, flush the index, and on save-as, `moveTo` the new location.
    - An unsaved project uses the session temp store and moves it on first save.
    - Closing an unsaved project discards the temp store.
    - Missing files raise the one non-modal warning from Edge cases.
  - verify: gtest — snap into an unsaved project, save it, reload it, and the entries, stars, notes and images are all back. Save-as to a new path carries them along. The Xvfb script reopens a saved project and sees the strip restored.
  - size: M

## Phase 574.5: Docs and audit

- [ ] M574.P5.T1 — Document the catalogue and audit the reference features
  - files: `docs/render-catalogue.md`, the PR body
  - approach:
    - Write `docs/render-catalogue.md` covering what a snapshot holds, Snap for image and scene inputs, compare, restore and undo, export, and the storage layout. It names no other application.
    - The PR body carries a features→Natron table covering every Render Gallery, Catalog/Monitor and Catalogue feature in this file's Scope, each marked In (where) or Out (why).
  - verify: the doc matches the shipped panel. `grep -i -E 'nuke|houdini|katana|gaffer|solaris'` on the new doc and the new sources is empty.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`. `RenderCatalogue_Test`, `RenderCatalogueState_Test`, `RenderCatalogueCapture_Test`, `RenderCatalogueJobs_Test`, `USDRenderCatalogue_Test`, `RenderCatalogueAuto_Test` and `RenderCatalogueViewer_Test` pass. `Tests/gui/m574_catalogue_panel.py`, `m574_catalogue_compare.py`, `m574_catalogue_restore.py`, `m574_catalogue_scene_snap.py`, `m574_catalogue_auto.py` and `m574_catalogue_jobs.py` pass under Xvfb at 1920x1080. The user OKs the strip, compare and auto-strip screenshots. The GUI bench runs (the diff touches `Gui/NodeGraph*` through the restore refresh) and its result is recorded here.

## Decisions

- 2026-11-24 — **Images + graph state (user):** a snapshot keeps the rendered image and the graph state that made it, so "restore this look" works.
- 2026-11-24 — The graph state is stored whole (an editable-state bundle), and the restore diff is computed against the live graph when Restore runs (planner). A stored diff would go stale as soon as the graph changed again. The diff/merge machinery (M103, M104) supplies the ops and the single undo step.
- 2026-11-24 — **Storage beside the project (user):** snapshots live in `<projectdir>/<stem>.catalogue/`, in a temp folder until the first save.
- 2026-11-24 — **Hidden source for the viewer (user):** a snapshot reaches a viewer side through a hidden, never-saved source node, not a Catalogue node in the graph.
- 2026-11-24 — **Auto snapshots in (user):** with the rate limit, wait-after-change, collapsed entries and the Show Non-Auto filter.
- 2026-11-24 — **Render-job snapshots in (user):** foreground and background writer renders add live entries.
- 2026-11-24 — **Tags and colour labels stay out (user).**
- 2026-11-24 — **USDRender feeds the catalogue directly (user):** a Render To Catalogue button on USDRender adds its full-resolution frame, with no Write needed.
