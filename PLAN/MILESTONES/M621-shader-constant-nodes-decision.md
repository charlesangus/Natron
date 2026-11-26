# M621 - Shader Constant Nodes Decision

From the user's M351 - Shader Graph Presentation preliminary test (2026-11-26): shader graphs need basic-type constant nodes (float, int, vector, colour). Cycles and MoonRay provide only some natively (Cycles Value, RGB, Combine XYZ; MoonRay ConstantScalarMap, ConstantColorMap; no int constant in either, no vector constant in MoonRay). M351.P3.T5 adds search aliases for the native ones. This milestone decides how to give users true int and vector constants.

## Options

1. **Per-renderer Natron constant nodes** (e.g. "Constant Float (Cycles)", "Constant Vector (MoonRay)"): each node belongs to one target and authors as that renderer's native equivalent (vector → Combine XYZ in Cycles; int → a float value). Lowering exists but is target-specific; fits the target model if per-target lowering is acceptable.
2. **Generic constant/parameter nodes** usable under any target, lowered per renderer at authoring. Simplest for users and shareable across targets; reverses the 2026-11-17 user decision (Houdini's target model, no lowering).

Considerations: how each authors to USD (native shader prims vs material interface inputs — a constant may be better as a promoted material input / NodeGraph interface), round trip on import, the VOP and channel network contexts (M482, F32) that reuse the same port network base, and what the reference DCCs do (VOP Constant/Parameter nodes; MaterialX constant nodes).

## Phase 621.1: Decide

- [ ] M621.P1.T1 — Options brief for the user
  - approach: research how USD/MaterialX and the reference DCCs model constants and interface parameters in material networks; lay out the two options (and any better third, e.g. material interface parameters) with trade-offs; the user decides; follow-up implementation milestones are filed
  - size: M

## Decisions
