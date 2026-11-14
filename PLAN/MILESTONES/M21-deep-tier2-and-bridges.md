# M21 - Deep Tier-2 Nodes

Full title: Deep tier-2 nodes

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

"M-Deep-2" in the design doc: DeepC-style tier-2 deep nodes (DeepCGrade-alike, position and
normal mattes, DeepSlice, DeepHoldout variants), each small on the native deep transport M18 -
Deep Compositing v1 built and M60 - Deep Layers And Channels gave layers. A member of F23 -
Deep Nodes.

Blocked on: the user choosing the node list (board `# Open questions`). The design doc's
scope-gravity rule requires amending `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md`
with the agreed list before this milestone is elaborated.

Acceptance sketch:
- The chosen DeepC-equivalent nodes ship on the native deep API (`renderDeepRoI`,
  `Engine/Nodes/Deep/`), each with a unit render test, docs and layer-aware channel knobs
  matching DeepRecolor and DeepExpression.

## Scope

**Out:**
- DeepDefocus — M295 - Native DeepDefocus.
- Deep from 3D renders — F24 - Deep Rendering (M286 - Utility Renderer Deep Output, M257 -
  Cycles Deep Output, M258 - MoonRay Deep Output).
- USDRender depth AOV into DeepFromImage, and DeepToPoints in the 3D viewer — dropped, no
  milestone.

## Decisions

- 2026-11-14 — Bridges dropped (user): true in-process deep from F24 - Deep Rendering makes
  depth-AOV deep redundant, so the USDRender depth AOV + DeepFromImage bridge and DeepToPoints
  are cut. DeepDefocus moved out to M295 - Native DeepDefocus, which already owns it. Renamed
  from "Deep Tier-2 And Bridges".
