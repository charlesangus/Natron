## Pending


### 2026-11-27 — change-request — Cycles VDB field changes may not refresh a live volume
- from: bosley-executor (cat-pm)
- refs: M554
- body: In the Cycles fork, `src/hydra/field.cpp` (~:140) marks a single rprim dirty during a scene-index sync, which Hydra rejects ("requires emulation"). A VDB field that changes on a live engine probably won't refresh its volume. M554 - Scene Camera For Delegate Renders fixed the same pattern in mesh.cpp by calling `MarkAllRprimsDirty(DirtyTopology)`. Apply the same fix to field.cpp and add a live-edit volume refresh test (F1 - VDB Volumes area).

### 2026-11-28T02:29:57Z — info
- refs: F33
- from: /cat-plan. F33 - Graph Diff And Merge gained two new todo members from the UAT round 1 grill: M646 - Template Node And Versioning and M647 - Template Compare Layout. Per §9b, a shipping Feature with an unfinished member goes back to `open` with `Owner` cleared, and the `response` flag can be cleared because the redesign question is answered (decisions are in both milestone files). UAT round 2 follows the landing of both members.
