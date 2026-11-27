## Pending

- **Pausing at a playback loop wrap leaves the view a frame behind** (PM, 2026-11-26, from M600 - Native Geometry UAT Round 2): when playback is paused just after the scheduler moves the timeline from the last frame back to the first, the first frame's evaluation is aborted, so the timeline shows frame 1 while the 3D view still shows the last frame; seeking to 1 is then a no-op (`TimeLineGui::seek` returns early when the time is unchanged). A pause should leave the view on the timeline's frame (re-render the current frame after an abort).

### 2026-11-27 — change-request — USDToGeo info message opens a dialog
- from: bosley-executor (cat-pm)
- refs: M418
- body: `USDToGeo.cpp` (~:426) posts its info through an info-type persistent message, which is never stored on the node: on a render thread it only reaches the error log, on the main thread it opens a modal dialog that can block a re-render. M418 - VDB Conversion Ops fixed the same problem in VDBFromPolygons/VDBFromParticles by using the node's quiet info message. Small fix: do the same in USDToGeo (and audit other nodes for info-type persistent messages).
