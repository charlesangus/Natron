# M642 - Pause At Loop Wrap

Pausing at a playback loop wrap leaves the view a frame behind (PM, 2026-11-26, from M600 - Native Geometry UAT Round 2). In loop mode, `getNextFrameToPlay` (`Engine/OutputSchedulerThread.cpp` ~:667-:720) moves the timeline from the last frame to the first. Pausing right after calls `ViewerTab::startPause` (`Gui/ViewerTab10.cpp:296`), which aborts rendering first (:315), killing frame 1's evaluation. So the timeline shows frame 1 while the 3D view still shows the last frame. Seeking to 1 then does nothing, because `TimeLineGui::seek` (`Gui/TimeLineGui.cpp:969-981`) returns early when the time is unchanged. A pause should leave the view on the timeline's frame.

## Scope

**In:**
- After a pause, the 2D and 3D viewers show the timeline's current frame, whichever frame the abort interrupted.

**Out:**
- Playback scheduling and frame-dropping behaviour.

**Edge cases:**
- **Pause not at a wrap:** same rule; if the aborted frame was the timeline's frame, it is re-rendered.
- **Viewer already showing the timeline's frame:** no extra render.
- **Bounce mode:** same fix.

## Phase 642.1: Re-render after the pause's abort

- [ ] M642.P1.T1 — Re-render the timeline's frame after a pause aborts it
  - files: `Gui/ViewerTab10.cpp` (`startPause` :296-:320, `abortRenderingAutoRestart` ~:502), `Engine/OutputSchedulerThread.cpp` (`ViewerCurrentFrameRequestScheduler` ~:3667), `Gui/TimeLineGui.cpp` (`seek` :969-981); a playback test in `Tests/` (find the one around `getNextFrameToPlay`, or add one) and a GUI script in `Tests/gui/`
  - approach: after the pause's abort, compare the frame each viewer last displayed with the timeline's current frame and request a render of the current frame when they differ. Leave `seek`'s early return, but make it check the displayed frame rather than only the timeline time, so a manual seek to the same frame also recovers. A GUI script plays a short loop with a 3D view, pauses just after a wrap (hook the frame-changed signal), and checks the displayed frame number.
  - verify: the GUI script passes 10 of 10 under Xvfb and fails on main; `ctest -R "Playback|OutputScheduler"` green
  - size: M

**Verification gate:** full build and ctest green; the GUI check passes on a remote GUI run.
