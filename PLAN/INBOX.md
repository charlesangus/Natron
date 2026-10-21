## Pending

### 2026-10-21T14:00:00Z — info
- refs: none
- Pre-existing bug on main, found by M107 - GPU Viewer Hand-off's UAT and outside its scope (user: inbox note only, for /cat-plan to plan). Disconnecting a viewer's active input sets A to -1 and the viewer goes black even when another input is still connected: Read → Grade1 → Viewer input 0, `viewer.connectInput(1, Grade2)` (connecting also makes input 1 the active A, via `InspectorNode::refreshActiveInputs`), then `viewer.disconnectInput(1)` → `getAInput()` is -1. Same on main `1c0b5ab3b` and the M107 lane. Fix belongs in `Engine/NodeInputs.cpp`. Repro: `.local/m107/p6t1c/viewer_ab_zoom_repro.py` with `repro.sh` (on the gpu host).
