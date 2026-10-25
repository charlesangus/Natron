# M198 - RenderEngine On Node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Move the `RenderEngine` out of the `OutputEffectInstance` base class, which nearly every effect derives today, and into `Node` by composition. A node gets an engine iff its effect `isOutput()`. Moved from M31 - Architectural Cleanup (cancelled 2026-10-25). 42 files under `Engine/` and `Gui/` mention `OutputEffectInstance` as of 2026-10-25.

Blocked on: M177 - Renders Isolated From Edits landing, then the user's go-ahead on whether the composition move is still worth it on top of M177's render-ownership changes.

Acceptance sketch:
- `grep -r OutputEffectInstance Engine Gui` returns nothing.
- A non-output node has no engine; the Viewer, a Write and a `DeepWrite` each have one.
- Playback, a Write render with progress and abort, and node deletion mid-render behave as before under Xvfb.

Carried from M31.P1.T1 — Move the `RenderEngine` from `OutputEffectInstance` to `Node` by composition:

  - files: `Engine/OutputEffectInstance.h`/`.cpp` (deleted), `Engine/Node.h`/`.cpp`, `Engine/OutputSchedulerThread.h`/`.cpp`, `Engine/AppInstance.h`/`.cpp`, `Engine/BlockingBackgroundRender.h`/`.cpp`, `Engine/ProcessHandler.h`/`.cpp`, `Engine/TimeLine.h`/`.cpp`, `Engine/EffectInstance.cpp`, `Engine/NodeGroup.h`/`.cpp`, `Engine/PrecompNode.cpp`, `Engine/PyAppInstance.cpp`, `Engine/NodeMain.cpp`, `Engine/NodeInputs.cpp`, `Engine/OfxEffectInstance.h`, `Engine/NoOpBase.h`, `Engine/DiskCacheNode.h`, `Engine/ViewerInstance.h`/`.cpp`, `Engine/WriteNode.cpp`, `Engine/Nodes/NativeEffectBase.h`, `Gui/Gui.h`, `Gui/Gui40.cpp`, `Gui/GuiAppInstance.h`/`.cpp`, `Gui/ProgressPanel.cpp`, `Gui/ProgressTaskInfo.cpp`, `Gui/NodeGui.cpp`, `Gui/ViewerTab.cpp`
  - approach: `OutputEffectInstance` is not "a writer"; it is "an effect that can own a `RenderEngine`" — one `RenderEnginePtr`, a render-sequence request queue, `renderFullSequence()`/`renderCurrentFrame()`, and `isOutput()`. It ended up as the base of nearly every effect (`OfxEffectInstance`, `NodeGroup`, `NoOpBase`, `DiskCacheNode`, `ViewerInstance`, and `NativeEffectBase` after M18.P3.T8a) because OFX plugins are hosted by one class whose writer-ness is only known per instance from the plugin context, and C++ picks a base per class — so every OFX Blur carries the engine slot and gates it on an `isOutput()` override. The base class therefore carries no information; `EffectInstance::isOutput()`/`isWriter()` are the real discriminators, and every consumer already does `dynamic_cast<OutputEffectInstance*>` + `isOutputNode()` before touching the engine. Replace inheritance with composition: `Node` owns an optional `RenderEnginePtr`, created in `Node::load()` iff `effect->isOutput()` (the moment `OutputEffectInstance::initializeData()` does it today), with `createRenderEngine()` becoming a virtual on `EffectInstance` that `ViewerInstance` overrides for its `ViewerRenderEngine` exactly as now. `renderFullSequence()`, `renderCurrentFrame()`, `isDoingSequentialRender()`, `isSequentialRenderBeingAborted()` and the request queue move to `Node` (or a small `RenderRoot` object `Node` owns), and `RenderEngine`/`OutputSchedulerThread` hold a `NodeWPtr` instead of an `OutputEffectInstanceWPtr`. The ~28 files above are the current reference set (`OutputSchedulerThread.cpp` 19 mentions, `AppInstance.cpp` 10, `Node.cpp` 9, the rest one to six each); `AppInstance`'s writer-queue bookkeeping (`RenderQueueItem`, `startNextQueuedRender`, `removeRenderFromQueue`) and the Gui progress panel key on `OutputEffectInstance*` and should key on `NodePtr`. Every effect class that derives `OutputEffectInstance` reverts to `EffectInstance`; their `isOutput()` overrides stay, unchanged. `NativeEffectBase` keeps the `NativePluginDescription::isWriter` flag from M18.P3.T8a and drops the base again — nothing in T8a/T8b conflicts with this. Do it as one mechanical change (the compiler finds every site), not incrementally: a half-moved engine with two owners is worse than either endpoint.
  - verify: whole ctest suite green, including M18's `renderFullSequence()`-driven `DeepWrite` test and the CLI/Python entry-point tests from M18.P3.T8c; `grep -r OutputEffectInstance Engine Gui` returns nothing; a non-output node (`Dot`, `Blur`, `DeepRecolor`) has no engine (`Node::getRenderEngine()` null) and the Viewer, a Write, and a `DeepWrite` each have one; GUI smoke under Xvfb (`build/deeprepro/run-gui.sh`): playback, a Write render with progress/abort, and node deletion mid-render all behave as before.
  - size: L

## Scope

**Out:**
- Who may edit during a render, which M177 - Renders Isolated From Edits owns.
- Scheduler feeder threads, which M199 - Remove Legacy Scheduler owns.

**Assumptions:**
- The carried brief's file list predates M177 and will need a fresh count at elaboration.

## Decisions

- 2026-10-25 — **Split out of M31 and sequenced after M177:** the user chose to re-decide once M177 lands, since M177 reworks who owns a running render.
