# M362 - Geometry Operators II

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The second batch of SOP-style operators, cut from F15 - Native Geometry during the
M54 - SOPs-Style Geometry grill (2026-11-08): PolyExtrude, Boolean, Fuse, attribute create
and attribute promote nodes, and dict attributes. Each follows the matching SOP
(`PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`). Not part of F15 - Native
Geometry, so it ships on its own once the Feature has landed.

Blocked on: F15 - Native Geometry landing — M359 - Geometry Operators fixes the operator
conventions (attribute promotion, group handling, tombstones) these nodes follow, and
M360 - Geometry Kernel Language's builtins show which of them a wrangle already covers.

Acceptance sketch:
- PolyExtrude, Boolean, Fuse, AttribCreate and AttribPromote behave like their SOPs on the reference cases.
- Dict attributes store, survive operators, and bridge as customData or JSON strings.
