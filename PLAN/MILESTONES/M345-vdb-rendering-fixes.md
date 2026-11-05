# M345 - VDB Rendering Fixes

Make a USD `Volume` with `OpenVDBAsset` fields render correctly in Storm, Cycles and MoonRay. An audit on 2026-11-05 rendered a real `.vdb` through `USDRead -> USDRender -> Write` on the M249 - M254 AppImage: Storm and MoonRay draw it, Cycles draws nothing, MoonRay ignores time-sampled file paths, and failures are silent. This milestone fixes the delegates and reporting; authoring nodes follow in M346 - USDVolume Node and M347 - Volume Material Node.

## Scope

**In:**
- Cycles: NanoVDB built in, OpenVDB initialised by the delegate, unbound volumes drawn with a default volume shader.
- MoonRay: time-sampled `filePath` advances, the `emission` field reaches `emission_grid`, the `TfToken` coding errors are gone.
- Natron: volume-only materials stop triggering "finds no surface", and missing files, missing grids and fields a renderer can't take raise a persistent warning.
- A cross-delegate render test with small synthetic VDB fixtures, including velocity motion blur.

**Out:**
- `USDVolume` authoring — M346 - USDVolume Node.
- Shared volume shading controls — M347 - Volume Material Node.
- Viewer bounds, picking, inspection and curated volume settings — M348 - Volume Viewer Support.
- Cycles GPU devices — none (a future named milestone; NanoVDB here is what they will need).
- Field3D assets — none (no `hioField3D` is bundled and neither path tracer reads them).
- Native VDB operations (mesh-to-volume, combine, filter) — none (user chose render, author and shade only, 2026-11-05).

**Edge cases:**
- Bound material with a surface but no volume output: Cycles uses the default volume shader, the same as an unbound volume.
- Fields from several files on MoonRay: `VdbGeometry` has a single `model` attribute, so this stays unsupported and raises the warning instead of hdMoonray's log-only error.
- A grid with no material use on Cycles: `need_attribute` skips it, which is correct and raises no warning.
- OpenVDB already initialised by Storm's `hioOpenVDB` in the same process: `openvdb::initialize()` is idempotent, so the Cycles test must run in its own process (M155 - Per-Suite Test Processes) or it can pass with the bug present.

**Assumptions:**
- Unconfirmed: the image's OpenVDB install includes the NanoVDB headers; if not, take them from the matching OpenVDB tag into a private include directory.
- Unconfirmed: Cycles' `scene->default_volume` reads the `density` field unchanged when assigned through Hydra.
- Unconfirmed: velocity motion blur works once volumes render; the test decides, and a failure is fixed here only if it is a small delegate fix, otherwise it becomes a new milestone.

## Phase 345.1: Cycles

- [ ] M345.P1.T1 — Build Cycles with NanoVDB
  - files: `tools/ci/local/build-cycles.sh`
  - approach: Set `-DWITH_CYCLES_NANOVDB=ON`. `src/scene/volume.cpp`'s `create_volume_mesh` only builds a volume mesh under `WITH_OPENVDB && WITH_NANOVDB`, so with it off a volume can never render. Find out why M249 - Bundle Cycles Delegate turned it off (commit 680e291ca) and resolve that reason. If the image lacks `nanovdb/NanoVDB.h`, add the headers from the OpenVDB tag matching `libopenvdb.so.13.0`.
  - verify: The script's configure output reports NanoVDB enabled, and `nm -C` on the built `hdCycles.so` shows `VolumeMeshBuilder` symbols or `openvdb_to_nanovdb`.
  - size: M
- [ ] M345.P1.T2 — Initialise OpenVDB in the Cycles delegate
  - files: charlesangus/cycles `natron/` branch: `src/hydra/field.cpp`
  - approach: Call `openvdb::initialize()` before the first file read in `HdCyclesVolumeLoader`. Nothing in Cycles calls it, because Blender does so itself; without it `readGrid` throws and the loader logs "Error loading OpenVDB file: Unknown error". A preload shim calling it cleared the error in the audit. Keep it a single commit with its reason.
  - verify: Cycles-side build of the fork succeeds and the commit is pushed to the fork with its reason in the message.
  - size: M
- [ ] M345.P1.T3 — Default volume shader for unbound Cycles volumes
  - files: charlesangus/cycles `natron/` branch: `src/hydra/geometry.inl`, `src/hydra/volume.cpp`, `src/hydra/volume.h`
  - approach: `HdCyclesGeometry::Sync` falls back to `scene->default_surface`, which has no volume, so `create_volume_mesh` finds no volume shader and returns. For `HdCyclesVolume`, use `scene->default_volume` when no material is bound or the material has no volume output (user, 2026-11-05). Don't change other geometry types' fallback.
  - verify: Fork builds; a usda with an unbound volume renders a non-empty silhouette through `hdCycles` when run against the fork's build.
  - size: M
- [ ] M345.P1.T4 — Pin the fork commit and rebuild the image
  - files: `tools/ci/local/build-cycles.sh`, `tools/ci/local/Dockerfile`
  - approach: Pin the fork commit holding T2 and T3 with its SHA-256 in the script's existing source record, and rebuild the dev image as M249 - Bundle Cycles Delegate did. Confirm exactly one `libopenvdb` is loaded.
  - verify: `CyclesDelegate` ctest passes on the new image, and `lddtree` or `ldd` on `hdCycles.so` shows the one bundled `libopenvdb`.
  - size: M

## Phase 345.2: MoonRay

All patches go in the fork from M325 - MoonRay Fork, not as `sed` lines in `build-moonray.sh`.

- [ ] M345.P2.T1 — Advance MoonRay volumes over time
  - files: fork `moonray/hydra/hdMoonray/lib/hydramoonray/Volume.cc`, `Volume.h`
  - approach: In the audit, a `filePath` with time samples at frames 1 and 2 rendered identically in MoonRay and moved correctly in Storm. `OpenVdbAsset::Sync` reads the path only on `DirtyParams`, and `Volume::syncAttributes` runs only when the rprim is dirty. Find where the change is lost and make a field path change update the geometry's `model`.
  - verify: A two-frame usda renders different silhouettes on frames 1 and 2 through MoonRay.
  - size: M
- [ ] M345.P2.T2 — Map the emission field in hdMoonray
  - files: fork `moonray/hydra/hdMoonray/lib/hydramoonray/Volume.cc`
  - approach: `syncAttributes` maps only `density` and `velocity`, though `VdbGeometry` also has `emission_grid`. Add `emission`. Other field names stay unmapped because the geometry class has no attribute for them.
  - verify: A file with an emission grid bound as `field:emission` changes the MoonRay render compared with the same scene unbound.
  - size: M
- [ ] M345.P2.T3 — Fix hdMoonray's volume coding errors
  - files: fork `moonray/hydra/hdMoonray/lib/hydramoonray/Volume.cc`
  - approach: A volume render logs twice "Attempted to get value of type 'TfToken' from empty VtValue". Grid names are honoured (a wrong name draws nothing), so trace which `Get` fails, likely under scene-index emulation, and guard or correct it. `MarkAllRprimsDirty` errors are separate and also appear without volumes; leave them unless the trace shows otherwise.
  - verify: A MoonRay volume render's log has no `_FailGet` line.
  - size: M

## Phase 345.3: Natron reporting and tests

- [ ] M345.P3.T1 — Stop the false surface warning on volume materials
  - files: `Engine/Nodes/Scene/DelegateSupport.cpp`, `Tests/Scene/RenderDelegates_Test.cpp`
  - approach: `warningText` reports "finds no surface" using only `ComputeSurfaceSource`. Also accept `ComputeVolumeSource(contexts)` so a material with only a volume output for this renderer's contexts isn't flagged.
  - verify: A new test case passes: a material with only `outputs:cycles:volume` raises no surface warning for Cycles, and a material with neither for its contexts still does.
  - size: M
- [ ] M345.P3.T2 — Add a shared VDB file query helper
  - files: `Engine/Nodes/Scene/VolumeFields.h`, `Engine/Nodes/Scene/VolumeFields.cpp`, `Engine/CMakeLists.txt`, `Tests/Scene/VolumeFields_Test.cpp`
  - approach: A small helper returning per-file grid names, value types, bounding boxes and voxel sizes, reading metadata only and calling `openvdb::initialize()` itself. Link `OpenVDB::openvdb` (the stand-in target from `cmake/usd-image-deps/`). M346 - USDVolume Node and M348 - Volume Viewer Support reuse it. Missing or unreadable files return an error, never throw.
  - verify: The new test reads a fixture's grid list and reports an error for a missing file and a non-VDB file.
  - size: M
- [ ] M345.P3.T3 — Warn about volume problems
  - files: `Engine/Nodes/Scene/DelegateSupport.cpp`, `Engine/Nodes/Scene/DelegateSupport.h`, `Tests/Scene/RenderDelegates_Test.cpp`
  - approach: Extend `warningText` to walk `UsdVolVolume` prims and report by prim path, through the same persistent message path: file not found, grid not in file, and for MoonRay, fields from more than one file or names other than density, emission and velocity. Keep the walk cheap per M327 - Heavy Scene Spike's concerns: query each distinct file once.
  - verify: New test cases assert each warning's text and that a valid volume raises none.
  - size: M
- [ ] M345.P3.T4 — Add VDB fixtures and a cross-delegate render test
  - files: `Tests/fixtures/volume-sphere.0001.vdb`, `Tests/fixtures/volume-sphere.0002.vdb`, `Tests/fixtures/make-volume-fixtures.py`, `Tests/fixtures/scene-volume.usda`, `Tests/Scene/VolumeRender_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Commit small fixtures (about 32³, `density`, `temperature` and `velocity` grids, sphere offset left on frame 1 and right on frame 2), with the Python generator kept for regeneration. Don't generate them at test time: an in-process writer would call `openvdb::initialize()` and hide a regression of M345.P1.T2. Check `du` per the LFS rule. For each delegate, assert that the volume darkens the correct side compared with a mesh-only scene, moves on frame 2, and that a wrong grid name draws nothing. Add a velocity motion blur case for Cycles and MoonRay. Build on `CyclesTestUtils.h`.
  - verify: `VolumeRender` passes in ctest for all three delegates, run in its own process.
  - size: L

**Verification gate:** release gate green per `.local/build-mode`, including `VolumeRender`, `VolumeFields`, `CyclesDelegate` and `MoonRayDelegate`. On the AppImage, a `.vdb` scene renders through all three renderers with an unbound volume and over a two-frame sequence, with no `_FailGet` or OpenVDB load error in the log.

## Decisions

- 2026-11-05 — Scope is render, author and shade, with no native VDB operations (user): covers the normal artist workflow without adding a volume data type to the graph.
- 2026-11-05 — Unbound Cycles volumes use a default volume shader through a fork patch (user): a volume then appears in all three renderers without setup, and needs no Natron-side machinery for one renderer.
- 2026-11-05 — Diagnostics live in `DelegateSupport`, reusing the persistent message path: audit showed Storm and MoonRay draw nothing and report nothing for a wrong grid name, and Cycles only logs.
- 2026-11-05 — Fixtures are committed rather than generated in-process: generating them would initialise OpenVDB and hide the Cycles initialisation bug.
