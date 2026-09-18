# Geometry is a fourth data kind, split SOP/LOP-style from the scene stack

2026-09-18. Editable geometry flows through the graph as a fourth
`DataKindEnum` value, `eDataKindGeometry`, carrying an immutable
copy-on-write `GeoDetail` (UsdGeom's attribute vocabulary without a
stage), rather than as dense attribute overrides authored into the
`ScenePayload` layer stack. Bridge nodes (`GeoToScene`, `SceneToGeo`)
convert at explicit points, exactly as `DeepToImage`/`DeepFromImage` do
for deep. Rationale: modeling-style operators mutate whole arrays, want
per-frame `Image`-like semantics, and need groups — none of which the
layer-stack convention provides — while the container built from
`VtArray`/`TfToken`/`VtValue` keeps the bridge a lookup table and lets
the same `Viewport3D`/Storm path display geometry through a scratch
stage. The design is `PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 3; the work is M54.
