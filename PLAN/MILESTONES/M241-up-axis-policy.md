# M241 - Up Axis Policy

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Split out of M226 - Scene Hierarchy From Topology's UAT (user, 2026-10-27: "we'll consider the up axis separately"). M226 shipped the project knobs `upAxis` (Y/Z) and `metersPerUnit`, authors them on every layer Natron creates (edit layers included), and has USDRead warn when a file disagrees; nothing is converted.

Open question for the user before elaborating (M226 UAT item 7): when a scene's data comes from a Y-up file and the project is Z-up, which axis wins?
- (a) the file's axis wins; only scenes with no file follow the project, with the USDRead warning as now;
- (b) the project's axis always wins (Natron's authored layers override the file's metadata, as M226 does today);
- or a conversion option (an explicit up-axis/units correction transform), which M226 ruled out by default.

Research first (see [[usd-research-before-design]]): `.local/docs/usd` (UsdGeom stage metrics, `UsdGeomGetStageUpAxis`, `UsdGeomLinearUnits`), Solaris's handling (`.local/docs/solaris/docs`, Configure Stage / Reference LOP up-axis and units options), Katana and Gaffer.

Correct M226's "Units on edit layers" decision to match the answer.

Input from M226's Codex review (PR #126, declined there 2026-10-27 and deferred here): under USDXformPrim/USDScopePrim each graft layer copies its input's `metersPerUnit`/`upAxis` and sits stronger than the node's override layer, so a mismatching file's metrics win downstream, contrary to the M226 decision text ("downstream of any Natron node the composed scene always reports the project's units"), while the approved UAT and `Tests/gui/m226_scene_hierarchy.py` expect the file's axis to survive. A project-wins answer means: grafts carry only the time-code rate, every Natron layer authors the project metrics, and the structure/graft memo keys include them.

