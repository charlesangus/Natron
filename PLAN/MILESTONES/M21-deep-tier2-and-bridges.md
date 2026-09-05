# Milestone 21: Deep tier-2 nodes and deep/3D bridges

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

"M-Deep-2 / M-Bridge" in the design doc: DeepC-style tier-2 deep nodes
(DeepCGrade-alike, position/normal mattes, DeepSlice, DeepHoldout variants,
DeepDefocus — each small once M18's transport exists, with the author holding
DeepC copyright for any relicensing), plus the first deep/3D convergence
points: `HydraRender` depth AOV + `DeepFromImage` for cheap deep from 3D
renders, and `DeepToPoints` to inspect deep data in `Viewport3D`. A deep-output
render delegate stays out of scope.

Blocked on: real-world use of M18's deep pipeline and M20's `HydraRender` AOVs —
the tier-2 selection and the bridge APIs should be chosen from observed need,
and the design doc's scope-gravity rule requires amending
`PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md` with the agreed node
list before this milestone is elaborated.

Acceptance sketch:
- A chosen subset of DeepC-equivalent nodes ships on the native deep API, each
  with a unit render test.
- A 3D scene rendered via HydraRender becomes a deep stream via depth AOV +
  DeepFromImage, and holds out a deep element correctly.
- DeepToPoints displays a deep image's samples as points in Viewport3D.
