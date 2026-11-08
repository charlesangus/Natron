# M340 - Viewer Gain Gamma On Rendered Pixels

The 3D viewport applies the Viewer's gain and gamma to its background as well as to what it renders. The user would rather they touch rendered items only (user, 2026-11-02, 2026-11-05). Moved from M234 - Camera Projection Nodes' dropped task M234.P6.T2.

## Scope

**In:**
- With gain or gamma set, geometry changes and the 3D viewport's background and grid don't.

**Out:**
- The 2D viewer's gain and gamma, and USDRender's output: neither has this background.

**Edge cases:**
- Transparent rendered pixels: gain and gamma act on what was rendered, and the background shows through unchanged, composited after.
- A viewer render delegate (M254 - Progressive Viewer Render): its image gets gain and gamma the same way, and the background stays out.

## Phase 340.1: Keep the background out

- [ ] M340.P1.T1 — Measure the options and choose
  - files: `Gui/SceneViewport.cpp` (`viewerGainTransform`, the colour config around line 370), `## Decisions` here
  - approach: compare drawing the background after Hydra's colour correction task with keeping it out of the corrected AOV and compositing afterwards. Measure frame time on the 100k-cube file against today's. A second full-screen pass that slows the viewport needs the user's call. Record the numbers and the choice.
  - verify: numbers in `## Decisions`, one approach chosen.
  - size: M

- [ ] M340.P1.T2 — Apply gain and gamma to rendered pixels only
  - files: `Gui/SceneViewport.{h,cpp}`, `Tests/gui/` (a new script, or the 3D viewer script that covers gain)
  - approach: implement the choice from M340.P1.T1. Dragging gain and gamma keeps reusing the bake, without re-rendering the scene.
  - verify: a GUI script at gain 2: a background pixel is unchanged and a card's pixel is doubled; gamma likewise; dragging gain causes no scene resync.
  - size: M

**Verification gate:** release gate green; the GUI script passes under Xvfb; viewport frame time recorded and not worse than today's by more than the user accepts.

## Decisions

- 2026-11-07 — P1.T1 choice: Storm clears to transparent and the window is cleared to the ungraded background, so Hydra's existing premultiplied over composites graded geometry onto it. No extra pass. The transform of black is subtracted from the clear so empty and fully covered pixels are exact; on a log view with a strong gamma the background can shift slightly. Frame-time comparison is a bench step (heavy_scene_bench with BENCH_VIEWER_GAIN) for the GPU host session. Delegate viewers (Cycles, MoonRay) stay out of scope, for the display-transform milestone the user asked for.
