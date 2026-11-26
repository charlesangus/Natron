# M617 - Native Geometry Viewer Architecture Spike

From the user's F15 - Native Geometry UAT (2026-11-26): the viewer still flashes during knob drags on native geometry. "It's ok for now, it works, but I'm concerned it's not going to be a viable/sustainable solution." The user asks whether to spike drawing native geometry ourselves (our own GL viewer), forking Storm and driving it outside Hydra, or similar, instead of going native geometry → USD → Hydra → Storm.

## Scope

**In:**
- Measure and explain the current flashing: where a knob drag's frame goes (cook, GeoUsdConvert, layer edit, Hydra resync, Storm draw), what produces the grey/flash frames, and how much is fixable within the current path.
- Evaluate the options with trade-offs (cost to build and maintain, performance on heavy geometry, feature parity: selection/picking, handles, display modes, stale cue, delegates other than Storm, and the hardware range from modest to many-core):
  1. Fix within the current path (incremental conversion, no full resyncs, double-buffered display).
  2. A native geometry GL/Vulkan drawer in the viewer (our own draw path for GeoDetail), keeping Hydra for USD scenes and for non-Storm delegates.
  3. Storm (HdSt) driven directly from a native-geometry scene index without the USD layer round trip.
  4. A forked Storm outside Hydra.
- A recommendation the user decides on.

**Out:**
- Building any of the options beyond throwaway prototypes.

## Phase 617.1: Spike

- [x] M617.P1.T1 — Measure the drag path and write the options report
  - files: the native geometry viewer path (GeoViewerLayer, GeoUsdConvert, GeoSdfFileFormat, SceneViewport, the scene index chain), docs in .local/docs (USD/Hydra), Gaffer and the reference DCC's viewer architecture where documented
  - approach: instrument a GeoTransform drag on light and heavy geometry (timings per stage, frames shown grey or stale); prototype the cheapest fix inside the current path if one is obvious; write `build/m617-viewer-architecture.md` with findings, the options table with trade-offs and costs, and a recommendation
  - verify: the report goes to the user with the measurements
  - size: L

**Verification gate:** the user picks a direction; follow-up milestones are filed.

## Decisions
- 2026-11-26 (PM, opus consultant) — Report `build/m617-viewer-architecture.md` sent to the user. Recommendation: keep Hydra/Storm. Measure the flashes first: per-stage timers and a presented-frame flash detector. Fix them in the current path (option 1); the likely sources are a failed cook clearing the scene, prims re-admitted over several fill frames, placeholder paints, and the stale cue toggling per step. Prototype a native points-only scene index (option 3, Blender's approach) only if heavy-geometry timings show the Sdf diff plus USD change processing above ~40% of a drag step. No own GL drawer, no Storm fork. **Needs: user decision.**
- 2026-11-26 — **User:** "file all that on the board, I will discuss on the side, more research is needed." Report kept at `PLAN/DESIGN/2026-11-26-native-geometry-viewer-architecture.md`. Staged plan filed as M618 - Native Geometry Viewer Flash Measurement, M619 - Native Geometry Viewer Flash Fixes and M620 - Native Geometry Points Scene Index Prototype (conditional), all unclaimed pending the user's side discussion. M617 done.
- 2026-11-26 — **User:** go straight to a full native scene index. Further research (Houdini, Blender, Omniverse, Gaffer, Katana; flash root cause; staging dependency inventory) is in `PLAN/DESIGN/2026-11-26-m617-research/`. Filed F41 - Native Geometry Scene Index (M622-M626) and M627 - Native Geometry Viewer Cook Fixes; M618, M619 and M620 cancelled.
