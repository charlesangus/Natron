# M348 - Volume Viewer Support

Make volumes first-class in the 3D viewer and inspection tools: a bounds guide, click-to-select, type display in the scene graph, grid details in the prim inspector, and the volume settings artists need on the render settings tabs.

## Scope

**In:**
- A bounds guide for selected volumes, using the existing guide display settings.
- Viewer picking of a volume, and scene graph selection and framing, through the existing selection paths.
- A volume type label or icon in the scene graph.
- Grid details in the prim inspector: name, type, voxel size, active voxels, file and memory estimate.
- Curated volume settings on USDRenderSettings' Storm and Cycles tabs where they are missing.
- A message when Storm downsamples a grid to fit its texture memory setting.

**Out:**
- Volume rendering fixes — M345 - VDB Rendering Fixes.
- Authoring and shading nodes — M346 - USDVolume Node, M347 - Volume Material Node.
- Slicing, voxel grids and other volume display modes — none.
- Geometry Spreadsheet views of voxel data — none (M337 - Geometry Spreadsheet stays point and mesh data).

**Edge cases:**
- Missing or unreadable file: bounds come from the authored extent, the inspector shows the error, and no read blocks the UI.
- No authored extent: guide falls back to file metadata through `VolumeFields`, or shows nothing if that fails too.
- A huge grid: inspector values come from metadata only, never a voxel load.
- Scrubbing a sequence: the inspector reads metadata asynchronously or on selection, never per-frame on the UI thread.
- Storm can't pick volumes: a click on the bounds guide selects instead, and that limit goes in the node docs.

**Assumptions:**
- Unconfirmed: Storm's default for Volume Max Texture Memory Per Field is 4 GB per field; check `HdStVolume::defaultMaxTextureMemoryPerField`.
- Unconfirmed: Storm's pick pass doesn't draw volumes; M348.P1.T2 decides.
- Unconfirmed: a downsampling message can be computed from grid metadata against the setting, without a hook inside Storm.

## Phase 348.1: Viewer

- [ ] M348.P1.T1 — Volume bounds guide
  - files: `Gui/SceneGuides.cpp`, `Gui/SceneGuides.h`, `Tests/gui/scene_volume_viewer.py`
  - approach: Draw a bounds box for selected `UsdVolVolume` prims, reusing the guide display settings and colours from M229 - 3D Viewer Guides And Display. Check frame selection uses the same bounds.
  - verify: A GUI test on a fixture checks that selecting the volume draws the guide and that frame selected fits its bounds.
  - size: M
- [ ] M348.P1.T2 — Pick volumes in the viewer
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneViewport.h`, `Tests/gui/scene_volume_viewer.py`
  - approach: Test clicking a volume in Storm through M232 - Scene Selection To Nodes' path. If Storm's pick pass omits volumes, add a pick test against each visible volume's bounds, behind opaque-geometry hits.
  - verify: A GUI test clicks the volume and asserts its prim becomes selected, and that a mesh in front of it still wins.
  - size: M

## Phase 348.2: Inspection

- [ ] M348.P2.T1 — Volume type in the scene graph
  - files: scene graph panel sources from M231 - Scene Graph Panel, `Tests/gui/scene_volume_viewer.py`
  - approach: Show a distinct type label or icon for `Volume` and `OpenVDBAsset` prims, using the panel's existing type display.
  - verify: A GUI test reads the type shown for both prims.
  - size: M
- [ ] M348.P2.T2 — Grid details in the prim inspector
  - files: prim inspector sources from M333 - Prim Inspector, `Tests/gui/scene_volume_viewer.py`
  - approach: For a selected `Volume` or `OpenVDBAsset`, show each field's name, type, voxel size, active voxels, file and memory estimate through `VolumeFields`, read off the UI thread.
  - verify: A GUI test on a fixture asserts the shown values against known grid values, and a missing file shows an error instead of blocking.
  - size: M

## Phase 348.3: Settings

- [ ] M348.P3.T1 — Curate volume render settings
  - files: `Engine/Nodes/Scene/RenderSettingsKnobs.cpp`, `Engine/Nodes/Scene/CyclesSettings.cpp`, `Tests/Scene/RenderSettingsKnobs_Test.cpp`
  - approach: Storm's `volumeRaymarchingStepSize`, `volumeRaymarchingStepSizeLighting` and `volumeMaxTextureMemoryPerField` come through `GetRenderSettingDescriptors`; confirm they appear, and group them with clear labels. Add Cycles' volume step rate to its curated tab if missing. Keep USD's default memory limit (user, 2026-11-05).
  - verify: `RenderSettingsKnobs` tests assert the knobs exist, and changing Storm's step size changes a fixture render.
  - size: M
- [ ] M348.P3.T2 — Tell the artist when Storm downsamples
  - files: `Engine/Nodes/Scene/DelegateSupport.cpp`, `Engine/Nodes/Scene/DelegateSupport.h`, `Tests/Scene/RenderDelegates_Test.cpp`
  - approach: Estimate each field's dense size from `VolumeFields` metadata and compare it with the active memory setting. When over, raise a persistent message for Storm naming the prim and the setting. Other renderers are unaffected and raise nothing.
  - verify: A test with the setting lowered below the fixture's size gets the message, and raising it clears the message.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`, including `RenderSettingsKnobs`. `Tests/gui/scene_volume_viewer.py` passes through `Tests/gui/run-gui-test.sh`. On the AppImage, a volume shows its bounds when selected, can be picked and framed, and shows its grids in the inspector; screenshots approved by the user. Run the GUI bench if the diff touches a file the GUI bench rule names.

## Decisions

- 2026-11-05 — Keep USD's default Storm texture memory limit, expose it, and tell the artist when it downsamples (user): it only affects the preview, and final Cycles and MoonRay renders stay at full resolution. This limit isn't a Natron safety cap, so the no-caps rule doesn't call for removing it.
