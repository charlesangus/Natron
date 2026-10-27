# M241 - Up Axis Policy

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Split out of M226 - Scene Hierarchy From Topology's UAT (user, 2026-10-27: "we'll consider the up axis separately"). M226 shipped the project knobs `upAxis` (Y/Z) and `metersPerUnit`, authors them on every layer Natron creates (edit layers included), and has USDRead warn when a file disagrees; nothing is converted.

Open question for the user before elaborating (M226 UAT item 7): when a scene's data comes from a Y-up file and the project is Z-up, which axis wins?
- (a) the file's axis wins; only scenes with no file follow the project, with the USDRead warning as now;
- (b) the project's axis always wins (Natron's authored layers override the file's metadata, as M226 does today);
- or a conversion option (an explicit up-axis/units correction transform), which M226 ruled out by default.

Research first (see [[usd-research-before-design]]): `.local/docs/usd` (UsdGeom stage metrics, `UsdGeomGetStageUpAxis`, `UsdGeomLinearUnits`), Solaris's handling (`.local/docs/solaris/docs`, Configure Stage / Reference LOP up-axis and units options), Katana and Gaffer.

Correct M226's "Units on edit layers" decision to match the answer.
