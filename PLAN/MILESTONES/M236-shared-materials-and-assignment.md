# M236 - Shared Materials And Assignment

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Asset-style material workflow, secondary to the comp-first design of M226 - Scene Hierarchy From Topology (each geometry carries its own material). A project setting names a shared materials scope (default `/materials`, a root prim); material nodes can author into it; `USDAssignMaterial` binds a material to prims matched by a pattern after assembly, authored as a collection binding (`material:binding:collection`) on each matched root so a later graft needs one repaired relationship per root (probe 2026-10-26: ~5 ms vs ~900 ms for 20k direct bindings). Re-parenting nodes pass the scope through untouched and repair bindings into it.

Acceptance sketch:
- A USDRead asset bound by pattern to a shared material renders with it, and still does after re-parenting.
- USDWrite exports the scope once and flattens repair overrides.
