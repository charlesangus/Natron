# A node's layers are a per-frame fact

2026-09-24, user direction. An input's layers can change from one frame to the next. The causes include:
- an image sequence whose AOVs come and go,
- a Switch whose `which` is keyed,
- an upstream node whose Disable is keyed.

`getAvailableLayers(time, view, …)` must report what the stream carries at that time. Every consumer that validates or renders uses the render's `(time, view)`, never the timeline's current frame. The current frame is only for UI such as menus and labels.

This corrects M34's review round 2, which declined a render-time validation test on the grounds that no node's layers vary with time. M61 adds the tests and fixes any graph that doesn't vary per frame.
