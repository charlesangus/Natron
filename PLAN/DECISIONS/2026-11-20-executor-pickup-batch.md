# Executor pickup batch (user, 2026-11-20)

The user asked bosley-executor to pick up, in parallel with its in-flight lanes: M365, M370, M400, M438, M299, M278, M103 and M104 (as one Feature with one UAT, registered as F33 - Graph Diff And Merge), M481 and M423, F2 - Native Shader Networks, M447, M256, and F5 - Live Sessions V1.

- M481 - Viewer Polygon Budgets and M423 - USDPrune Isolate Mode start now, based on F4 - Heavy Scene Viewer's work, and don't merge until F4 lands. This overrides M481's unfinished dependency on M336. M481 needs M336's viewer code, which is on M336's branch and not yet on feature/f4-heavy-scene-viewer, so it is stacked on M336's branch. M423 branches from feature/f4-heavy-scene-viewer.
- F2 and F5 run their members in dependency order. Their first members are M349 - Shader Node Spike and M105 - Live Session Core.
