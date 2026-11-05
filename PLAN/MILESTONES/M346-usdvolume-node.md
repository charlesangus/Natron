# M346 - USDVolume Node

A native `USDVolume` node that brings a `.vdb` file or sequence into the scene, authoring a `UsdVolVolume` with one `OpenVDBAsset` field per grid. Today volumes only arrive through hand-written USD files read by `USDRead`.

## Scope

**In:**
- File knob with Natron's sequence patterns and path variables.
- One field per grid in the file, with an on/off control and a Field Name per grid (default: the grid's name).
- Sequence controls matching `Read`: Before, After and Frame Offset, with a missing frame as an error unless the artist chooses otherwise.
- Extent from the file's metadata, and the same transform handling as other USD primitive nodes.
- Node docs, icon, and a save, load and undo round trip.

**Out:**
- Delegate fixes — M345 - VDB Rendering Fixes.
- Shading controls — M347 - Volume Material Node.
- Viewer bounds and inspection — M348 - Volume Viewer Support.
- Writing or editing VDB voxel data — none (no native VDB operations, user 2026-11-05).
- Field3D files — none.

**Edge cases:**
- Grid list differs between sequence frames: the list is the union across the first and last frames, and a grid missing on a frame raises M345 - VDB Rendering Fixes' grid warning rather than failing the node.
- File changes to one with different grids: rows for grids that still exist keep their toggles and Field Names; others are dropped.
- All grids toggled off: the Volume prim is authored with no fields and draws nothing, without an error.
- Grid names that aren't valid USD identifiers: the prim name is sanitised, with `fieldName` keeping the real grid name.
- Two grids sharing a name in one file: both get rows and use `fieldIndex` to tell them apart.
- A very long sequence: no cap on frame count or file size (project rule).
- Project-relative paths: resolved the way `USDRead` resolves its file, so the saved project stays portable.

**Assumptions:**
- Unconfirmed: a per-grid row can be built with the existing dynamic knob patterns; M346.P1.T2 picks the mechanism.
- Unconfirmed: authoring per-frame `filePath` time samples is cheap enough for long sequences; otherwise author only the current frame.

## Phase 346.1: Node core

- [ ] M346.P1.T1 — Add the USDVolume node with file, prim path and static single-file authoring
  - files: `Engine/Nodes/Scene/USDVolume.h`, `Engine/Nodes/Scene/USDVolume.cpp`, `Engine/Nodes/Scene/USDVolume.reg.inc`, `Engine/CMakeLists.txt`, `Tests/Scene/USDVolume_Test.cpp`
  - approach: Follow `USDCube` for node structure and transform handling and `USDRead` for file resolution. Author `UsdVolVolume`, an `OpenVDBAsset` per grid via `VolumeFields` from M345 - VDB Rendering Fixes, `field:<name>` relationships, `fieldName`, `fieldDataType` and `vectorDataRoleHint`, and extent from the grid bounding boxes. Check `pxr/usd/usdVol` headers before settling attribute use, and see `.local/docs/solaris` and `.local/docs/usd` for how similar tools present it.
  - verify: The new test creates the node on a fixture and asserts the composed stage's prims, relationships, `fieldName` values and extent.
  - size: L
- [ ] M346.P1.T2 — Per-grid toggles and Field Names
  - files: `Engine/Nodes/Scene/USDVolume.cpp`, `Engine/Nodes/Scene/USDVolume.h`, `Tests/Scene/USDVolume_Test.cpp`
  - approach: On a file change, rebuild one row per grid: a toggle and a Field Name string defaulting to the grid's name. Reuse a dynamic knob mechanism already in the tree (`RenderSettingsKnobs` is the nearest example). Labels follow script names. Rows must serialize, restore on load and support undo without re-reading the file during load.
  - verify: Tests cover toggling a grid off, renaming a field, a file change keeping matching rows, and a project save and load restoring all of it.
  - size: L

## Phase 346.2: Sequences

- [ ] M346.P2.T1 — Share Read's frame range logic
  - files: `Engine/Nodes/IO/NativeRead.cpp`, new shared helper under `Engine/Nodes/IO/`, `Tests/Native/`
  - approach: Extract the Before, After, offset and missing-frame mapping behind `kKnobBefore` and `kKnobAfter` into a helper taking a frame and a range and returning the file frame or a missing-frame outcome. `NativeRead` calls it with no behaviour change.
  - verify: Existing `NativeRead` tests pass unchanged, and a new unit test covers hold, loop, bounce, black and missing-frame outcomes.
  - size: M
- [ ] M346.P2.T2 — Author volume sequences
  - files: `Engine/Nodes/Scene/USDVolume.cpp`, `Engine/Nodes/Scene/USDVolume.h`, `Tests/Scene/USDVolume_Test.cpp`
  - approach: Accept `####` and `%04d` through Natron's sequence parsing, add Before, After and Frame Offset knobs from the shared helper, and author time-sampled `filePath` and extent. Outside the range, "black" means the prim is invisible at that time.
  - verify: Tests assert the time samples for each mode on the two-frame fixture, plus a missing-frame error.
  - size: M

## Phase 346.3: Delivery

- [ ] M346.P3.T1 — Node docs and icon
  - files: `Engine/Nodes/Scene/docs/USDVolume.md`, `Gui/Resources/Images/NativeNodes/USDVolume.png`, `Gui/GuiResources.qrc`
  - approach: Document the controls and each renderer's limits, including that MoonRay takes density, emission and velocity from one file. No Nuke references.
  - verify: The docs build lists the node, and the icon appears in the node menu.
  - size: M
- [ ] M346.P3.T2 — GUI check across renderers
  - files: `Tests/gui/usd_volume_node.py`
  - approach: Under the GUI harness at 1920x1080, create `USDVolume` on a fixture, connect it to `USDRender`, and render with each renderer. Shoot the panel, one file per screenshot.
  - verify: The script passes through `Tests/gui/run-gui-test.sh`, and the panel screenshots go to the user for sign-off.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`, including `USDVolume`. On the AppImage, a two-frame VDB sequence through `USDVolume` renders and moves in Storm, Cycles and MoonRay; panel screenshots approved by the user.

## Decisions

- 2026-11-05 — All grids bound automatically with per-grid toggles (user): works immediately, and renderers load only the grids their shaders use.
- 2026-11-05 — Sequences match the `Read` node, through shared logic (user): familiar controls and one sequence behaviour across the app; a missing frame is visible rather than hidden.
- 2026-11-05 — Per-grid Field Name added: needed so a file whose density grid has another name still reaches renderers expecting `density`.
