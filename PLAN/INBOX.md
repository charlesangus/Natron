## Pending

### 2026-11-07T14:00:00-05:00 — change-request
- refs: none
- User asked for a new milestone under F9 - Delegate Projection: cancel and stop responsiveness for Cycles and MoonRay. Viewers: an old render can run on until the next sync; changing renderer or settings joins the session under the stage lock and can freeze the UI, and the engine may only be destroyed under that lock. Offline: cancel is only checked between renders, and with no abort callback a stop can take up to 600 s. Goal: every renderer cancels quickly, and the UI never waits on a stop.

### 2026-11-07T14:00:00-05:00 — change-request
- refs: none
- User asked for a new standalone milestone: Cycles, MoonRay and Natron Utility viewers use the viewer's display transform (OCIO view, gain, gamma, lighting mode) as Storm does. Progressive frames are 8-bit sRGB from `srgbTable()` today, so frames must carry float, and it needs a design pass. This also answers the display transform question logged under M254 - Progressive Viewer Render on the board.

