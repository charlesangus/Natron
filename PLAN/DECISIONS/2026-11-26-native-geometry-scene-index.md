# Native geometry is drawn from a Hydra scene index, not staged as USD

Decided by the user on 2026-11-26, after the M617 - Native Geometry Viewer Architecture Spike and its follow-up research (`PLAN/DESIGN/2026-11-26-m617-research/`).

The Storm viewer draws native geometry from a Hydra scene index built from GeoUsdConvert's partitions (F41 - Native Geometry Scene Index), with no Sdf layer or UsdStage in between. This is the approach Blender (its default HYDRA export method) and Omniverse (Fabric Scene Delegate) take, and it matches Katana's direct Geolib → Hydra bridge. None of the DCCs researched stages native data as USD to draw it interactively. A custom GL/Vulkan drawer and a forked Storm were rejected: they would mean redoing picking, selection, display modes and delegates, and maintaining two renderers.

Scope is the viewer only. Final render, USDWrite and GeoToUSD keep authoring USD through GeoUsdConvert, and so do the Cycles and MoonRay viewers, whose render time dwarfs composition. Native geometry composed onto USD prims stays staged. Native prims do not appear in the USD panels (outliner, prim inspector, prim picker, match search); the Geometry Spreadsheet is their inspector. The staged Storm path is removed outright, with a test-only parity oracle comparing the scene index with UsdImaging over the `natrongeo` layer.
