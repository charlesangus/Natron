## Pending

- **Pausing at a playback loop wrap leaves the view a frame behind** (PM, 2026-11-26, from M600 - Native Geometry UAT Round 2): when playback is paused just after the scheduler moves the timeline from the last frame back to the first, the first frame's evaluation is aborted, so the timeline shows frame 1 while the 3D view still shows the last frame; seeking to 1 is then a no-op (`TimeLineGui::seek` returns early when the time is unchanged). A pause should leave the view on the timeline's frame (re-render the current frame after an abort).
