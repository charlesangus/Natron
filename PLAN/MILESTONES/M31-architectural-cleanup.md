# Milestone 31: Architectural cleanup

**Deferred — do not start or elaborate without an explicit user go-ahead.**
A parking place for structural debts the deep/3D work has exposed but that
are refactors in their own right: each would widen a feature milestone's diff
well past its scope, none blocks a feature, and each is worth doing on its own
once the feature that revealed it has shipped. Add tasks here as they surface
rather than folding them into the milestone that found them.

## Phase 31.1: Render-root ownership

- [ ] M31.P1.T1 — Move the `RenderEngine` from `OutputEffectInstance` to `Node` by composition
  - files: `Engine/OutputEffectInstance.h`/`.cpp` (deleted), `Engine/Node.h`/`.cpp`, `Engine/OutputSchedulerThread.h`/`.cpp`, `Engine/AppInstance.h`/`.cpp`, `Engine/BlockingBackgroundRender.h`/`.cpp`, `Engine/ProcessHandler.h`/`.cpp`, `Engine/TimeLine.h`/`.cpp`, `Engine/EffectInstance.cpp`, `Engine/NodeGroup.h`/`.cpp`, `Engine/PrecompNode.cpp`, `Engine/PyAppInstance.cpp`, `Engine/NodeMain.cpp`, `Engine/NodeInputs.cpp`, `Engine/OfxEffectInstance.h`, `Engine/NoOpBase.h`, `Engine/DiskCacheNode.h`, `Engine/ViewerInstance.h`/`.cpp`, `Engine/WriteNode.cpp`, `Engine/Nodes/NativeEffectBase.h`, `Gui/Gui.h`, `Gui/Gui40.cpp`, `Gui/GuiAppInstance.h`/`.cpp`, `Gui/ProgressPanel.cpp`, `Gui/ProgressTaskInfo.cpp`, `Gui/NodeGui.cpp`, `Gui/ViewerTab.cpp`
  - approach: `OutputEffectInstance` is not "a writer"; it is "an effect that can own a `RenderEngine`" — one `RenderEnginePtr`, a render-sequence request queue, `renderFullSequence()`/`renderCurrentFrame()`, and `isOutput()`. It ended up as the base of nearly every effect (`OfxEffectInstance`, `NodeGroup`, `NoOpBase`, `DiskCacheNode`, `ViewerInstance`, and `NativeEffectBase` after M18.P3.T8a) because OFX plugins are hosted by one class whose writer-ness is only known per instance from the plugin context, and C++ picks a base per class — so every OFX Blur carries the engine slot and gates it on an `isOutput()` override. The base class therefore carries no information; `EffectInstance::isOutput()`/`isWriter()` are the real discriminators, and every consumer already does `dynamic_cast<OutputEffectInstance*>` + `isOutputNode()` before touching the engine. Replace inheritance with composition: `Node` owns an optional `RenderEnginePtr`, created in `Node::load()` iff `effect->isOutput()` (the moment `OutputEffectInstance::initializeData()` does it today), with `createRenderEngine()` becoming a virtual on `EffectInstance` that `ViewerInstance` overrides for its `ViewerRenderEngine` exactly as now. `renderFullSequence()`, `renderCurrentFrame()`, `isDoingSequentialRender()`, `isSequentialRenderBeingAborted()` and the request queue move to `Node` (or a small `RenderRoot` object `Node` owns), and `RenderEngine`/`OutputSchedulerThread` hold a `NodeWPtr` instead of an `OutputEffectInstanceWPtr`. The ~28 files above are the current reference set (`OutputSchedulerThread.cpp` 19 mentions, `AppInstance.cpp` 10, `Node.cpp` 9, the rest one to six each); `AppInstance`'s writer-queue bookkeeping (`RenderQueueItem`, `startNextQueuedRender`, `removeRenderFromQueue`) and the Gui progress panel key on `OutputEffectInstance*` and should key on `NodePtr`. Every effect class that derives `OutputEffectInstance` reverts to `EffectInstance`; their `isOutput()` overrides stay, unchanged. `NativeEffectBase` keeps the `NativePluginDescription::isWriter` flag from M18.P3.T8a and drops the base again — nothing in T8a/T8b conflicts with this. Do it as one mechanical change (the compiler finds every site), not incrementally: a half-moved engine with two owners is worse than either endpoint.
  - verify: whole ctest suite green, including M18's `renderFullSequence()`-driven `DeepWrite` test and the CLI/Python entry-point tests from M18.P3.T8c; `grep -r OutputEffectInstance Engine Gui` returns nothing; a non-output node (`Dot`, `Blur`, `DeepRecolor`) has no engine (`Node::getRenderEngine()` null) and the Viewer, a Write, and a `DeepWrite` each have one; GUI smoke under Xvfb (`build/deeprepro/run-gui.sh`): playback, a Write render with progress/abort, and node deletion mid-render all behave as before.
  - size: L

## Phase 31.2: Per-node memory (speculative, from M62.P5.T1; take only if needed)

M62.P5.T1 (2026-10-03, massif on a 300-node OFX Grade chain, release build at `ab2b06c90`) measured ~0.65 MB of heap per idle node: 518 KB useful + 135 KB malloc overhead. 75–80% is OpenFX-specific. User decision 2026-10-03: none of these fixes go into M62; they are parked here as speculative and only worth doing if memory per node still matters once core nodes are native (M67). Numbers are per node.

- [ ] M31.P2.T1 — Copy-on-write instance property sets in the OpenFX host library (speculative)
  - files: `libs/OpenFX` fork (`HostSupport` `Property::Set`, `Param::Instance`, `ClipInstance`, `ImageEffect::Instance`)
  - approach: instances deep-copy their descriptor's `Property::Set` (250 KB param instances, 31 KB clips, 25 KB effect instance, 16 KB interact descriptors; `Property::Set` copy-construction alone is 286 KB). Make the instance set an overlay over the shared descriptor set: lookups check the overlay then the descriptor; a write materialises only that property. Care with properties the host mutates per instance (enabled, secret, values).
  - verify: massif per-node heap on the same chain drops by ≥150 KB; full ctest and the OFX plugin tests green.
  - size: L
- [ ] M31.P2.T2 — Build interact descriptors lazily so NatronRenderer never allocates them (speculative)
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/OfxOverlayInteract.*`
  - approach: `Interact::Descriptor` property sets are built per instance even with no GUI (16 KB). Create them on first overlay use.
  - verify: massif shows no `Interact::Descriptor` allocations in a renderer run; GUI overlays still appear under Xvfb.
  - size: M
- [ ] M31.P2.T3 — Connect OFX dynamic-property signals lazily (speculative)
  - files: `Engine/OfxParamInstance.cpp` (`OfxParamToKnob::connectDynamicProperties`)
  - approach: per-param `QObject::connect` calls cost 29 KB per node; connect on first change or share one connection per knob group. Needs a GUI check that enabled/secret/label mirroring still updates.
  - verify: massif delta ≥20 KB per node; Xvfb check of a plugin toggling a param's secret/enabled state.
  - size: M
- [ ] M31.P2.T4 — Lazy per-knob `Curve` and deferred GUI-only default pages (speculative)
  - files: `Engine/Knob.cpp`, `Engine/Node.cpp` (`createNodePage`, info page)
  - approach: a `Curve` per knob (6 KB) and GUI-only pages (20 KB) are allocated for every node; defer them. Touches serialization and Python's expectation of which knobs exist.
  - verify: project save/load and Python knob listing unchanged; massif delta.
  - size: M
- [ ] M31.P2.T5 — Unify OFX param and knob state (structural)
  - files: `Engine/Knob*.h/.cpp`, `Engine/OfxParamInstance.*`, `libs/OpenFX` HostSupport
  - approach: params and knobs hold the same parameter twice (OFX `Param::Instance` 250 KB + Natron knob 117 KB). One object owning descriptor-backed state would remove most of it; architecture-level, crosses serialization and GUI. Likely moot for nodes rewritten natively in M67.
  - verify: to be elaborated.
  - size: L

## Phase 31.3: Render-engine hand-offs from M63 (speculative)

- [ ] M31.P3.T1 — Per-render context object replacing per-effect TLS frame args
  - files: `Engine/TLSHolder*`, `Engine/ParallelRenderArgs.*`, `Engine/FrameRenderContext.*`, `Engine/EffectInstance*`
  - approach: M63 materialises `EffectTLSData` from a per-frame context on demand and inherits it along spawner chains; the end state is tasks reading a `FrameRenderContext` directly (args map, request, abort info) with no per-effect thread-local frame args at all. Also fix `FrameView_compare_less` (`ParallelRenderArgs.h`: returns true for `lhs.view > rhs.view`, not a strict weak ordering; multi-view only).
  - verify: whole ctest both scheduler modes; `GraphScalingTLS` and `FrameRenderContext` suites rewritten to the new model.
  - size: L
- [ ] M31.P3.T2 — Collapse `RenderThreadTask` frame feeders into the scheduler and delete the dead FFA `processFrame` image path
  - files: `Engine/OutputSchedulerThread.*`, `Engine/RenderScheduler.*`
  - approach: writers' frames are fed by `RenderThreadTask` QThreads that only block on a future in task-graph mode; the scheduler could own frame admission itself. `processFrame` (`OutputSchedulerThread.cpp:~2432`) is dead for FFA renders.
  - verify: whole ctest both modes; the writer abort tests; bench range renders no slower.
  - size: L

**Verification gate:** every task's verify holds; whole ctest suite green; AppImage packaged and a manual GUI session (playback, render-to-disk with abort, Python `app.render()`) reports no regression.
