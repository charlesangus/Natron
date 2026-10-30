# Input topology edits

How a connection edit reaches the hash, and what is refreshed alongside it. Citations follow
[keys.md](keys.md): `file:line` on this branch, paths under `Engine/` unless they start with
`Tests/` or `Gui/`.

## Caches

There is no topology cache of its own. A topology edit acts on these:

| State | Where | Notes |
| --- | --- | --- |
| Real inputs and outputs | `_imp->inputs`, `_imp->outputs` | Read by `getInput` (NodeInputs.cpp:236 → `getInputInternal(false, ...)` :242), which feeds `getHashInputs` (Node.cpp:1133) and every render. |
| GUI inputs and outputs | `_imp->guiInputs`, `_imp->guiOutputs` | Edits made while the node renders are queued here (`getGuiInput` NodeInputs.cpp:268). |
| Group connections | `NodeGroup` `inputs`/`guiInputs` | Swapped by `NodeGroup::dequeueConnexions` (NodeGroup.cpp:1397). |
| Inspector/viewer active inputs | `InspectorNode::_activeInputs` | Folded into a viewer's content hash (Node.cpp:976-980). |
| Effective data kind | `Node::invalidateEffectiveOutputDataKindCache` Node.cpp:2377 | Cleared on every input change; see actions-and-metadata.md. |
| Input-related data (clip preferences) | `markAllInputRelatedDataDirty` Node.cpp:8418 | Set dirty, then refreshed from `endInputEdition` (NodeInputs.cpp:1597). |

## Key

The hash folds the connected inputs by slot after redirection (keys.md, step 5). Redirection
(`applyNodeRedirectionsUpstream` NodeInputs.cpp:78) looks through a Group to its output's input, a
precomp to `PrecompNode::getOutputNode()` (PrecompNode.cpp:147; the read node when Pre-Render is
on), and a GroupInput to the group's real input. Because those nodes are looked through, no hash
folds them in. `getHashDependents` (Node.cpp:1074) therefore also lists the outputs of every node
for which `isSkippedByHashInputs` (Node.cpp:212) is true. Otherwise a dirty walk could stop at them
while they are dirty.

## Invalidated by

Every edit path follows the same shape. It aborts the viewers below
(`abortViewerRendersForEdit`, EffectInstance.cpp:6297), then takes the node's render-start lock
(`Node::Implementation::lockRenderStarts`, Node.cpp:1484: `nodeIsDequeuingMutex`, the lock under
which `setNodeIsRenderingInternal` raises the render count). Under that lock it reads whether a
render holds the node, and either writes the real inputs (and the GUI copies) or writes only the
GUI copies and sets `actionsQueued`, then marks the hash (`markHashDirty()`, which takes only the
mark queue's mutex). No render can start between that check, the write and the mark (fixed in
M260; before, the mark came after `onInputChanged`, outside the lock). Outside the lock it updates
the input's outputs, calls `onInputChanged` (real path only), which so reads the new hash, then
`endInputEdition(true)`. Nothing is emitted or called back while the lock is held.

| Function | Real or queued | `onInputChanged` | `computeHash()` | Notes |
| --- | --- | --- | --- | --- |
| `Node::connectInput` NodeInputs.cpp:801 | Decided under the render-start lock (:833). The real path writes both `inputs` and `guiInputs`; the queued path writes `guiInputs` and sets `actionsQueued` (:849). | after the hash mark | under the lock, after the write | `abortViewerRendersForEdit` (:829). `recheckInvalidExpressions` after the write. |
| `Node::replaceInputInternal` NodeInputs.cpp:885 | Decided under the render-start lock (:921) when the caller passes `queueWhileRendering`; `replaceInput` (:977) does. The destroy path passes false and always writes the real inputs. | plus `invalidateDetachedInputDataKind` | under the lock | Viewer abort is in `replaceInput` (:984). |
| `Node::switchInput0And1` NodeInputs.cpp:989 | Decided under the render-start lock (:1038) | both slots | under the lock | Swaps the first non-mask input with the next one. |
| `Node::disconnectInput(int)` NodeInputs.cpp:1125 | Decided under the render-start lock (:1144) | plus detached data kind | under the lock | No viewer abort while the node is being destroyed (:1132-1140). |
| `Node::disconnectInputInternal` NodeInputs.cpp:1194 | Decided under the render-start lock (:1206) when the caller passes `queueWhileRendering`; `disconnectInput(Node*)` (:1266) does. | after the mark | under the lock | |
| `Node::onInputChanged` NodeInputs.cpp:1623 | Real inputs only (main thread) | — | Not called | Order: data-kind cache cleared, mask enabledness, Inspector active inputs, frame args installed, `effect->onInputChanged` (:1679), render clones cleared. Then it forwards to a group's GroupInput outputs (:1709), a GroupOutput's container outputs (:1724) and a precomp's outputs (:1737). |
| `Node::endInputEdition` NodeInputs.cpp:1592 | — | — | — | At recursion depth 0, if anything changed: `refreshInputRelatedDataAfterInputChange` (unless a node tree is being created), `refreshDynamicProperties`, then `renderCurrentFrame` on the viewers below. |
| `Node::dequeueActions` Node.cpp:7983 | Applies the queue | Per changed slot, inside begin/end edition | Input swaps marked under the lock; `computeHash()` after, only if values were dequeued | Returns at once while a render holds the node, aborting the viewer renders below when something is queued. Otherwise sets `nodeIsDequeuing`, dequeues knob values and group connections, then copies `guiInputs` over `inputs` and `guiOutputs` over `outputs` under the render-start lock, unless a render started meanwhile on the main thread; see step 6 below. Calls `refreshIdentityState()` after the hash mark. |
| `Node::setNodeIsRenderingInternal` Node.cpp:7882 | — | — | — | Raises `nodeIsRendering` under `nodeIsDequeuingMutex`, waiting first, off the main thread, while the node is dequeuing; then recurses through `getInput` (real inputs). Set by `RenderingFlagSetter` (ViewerInstance.cpp:1248, OutputSchedulerThread.cpp:2531, :2699, RotoPaint.cpp:854). |
| `Node::unsetNodeIsRendering` Node.cpp:7943 | — | — | — | When the count reaches 0 emits `mustDequeueActions`, in every mode. That is connected to `dequeueActions` with the default connection type (Node.cpp:227), so from a render thread it is queued to the main thread. |

`InspectorNode::connectInput` (NodeInputs.cpp:1797) and `InspectorNode::setActiveInputAndRefresh`
add their own `computeHash()` calls; `setActiveInputAndRefresh` makes it after `onInputChanged`
has changed the active inputs (fixed in M260). `InspectorNode::setInputA`/`setInputB`
(:1985, :2002) call `computeHash()` when the selection changes (:1996, :2013); the viewer's
`setInputA`/`setInputB` (ViewerInstance.cpp:4305, :4320) go through them.

### The queued path, step by step

1. A render sets `nodeIsRendering` on the nodes it reaches (Node.cpp:7882). Its frame args hold
   each node's hash, computed from the real inputs.
2. The user connects input `i` on node N. Under the render-start lock the edit sees the render, so
   only `guiInputs[i]` changes and `actionsQueued` is set; the new input's `guiOutputs` follow
   (`connectOutput` NodeInputs.cpp:1107). Viewer renders below are aborted with auto-restart
   (`abortViewerRendersForEdit`), which keeps the oldest render (keys.md K1). Other renders, such
   as writers, continue.
3. `computeHash()` marks N. The recompute reads the real inputs, which haven't changed, so the hash
   value is the same. `recomputeHashIfDirty` calls `onNodeHashChanged` only on a change, so the
   ActionsCache isn't touched. Renders still running are consistent: old inputs under the old hash.
4. The last render below N finishes. `unsetNodeIsRendering` emits `mustDequeueActions`, queued to
   the main thread.
5. A render may start before the slot runs. It sees the old real inputs and N's old hash.
6. `dequeueActions` runs on the main thread. If a render holds N it returns, aborting the viewer
   renders below because `actionsQueued` is set; that render's `unsetNodeIsRendering` emits again.
   Otherwise it sets `nodeIsDequeuing`, so render threads wait, and dequeues knob values; the
   holder stops dequeuing values as soon as `canSetValue()` turns false. A value callback can
   start a render on the main thread itself (a nested event loop), which does not wait. So the
   input swap re-checks under the render-start lock and, if such a render holds N, leaves the
   inputs queued with `actionsQueued` set until it ends. Otherwise it swaps `inputs` and marks the
   hash under that lock, calls `onInputChanged` for each changed slot, `computeHash()` if values
   were dequeued, then `refreshIdentityState()`, and clears `nodeIsDequeuing`.
7. A render that starts after the swap sees the new inputs under the new hash (T8, fixed in
   M260).

## Not invalidated by

- **An edit that is only queued** (step 3). By design the hash keeps the real topology until the
  dequeue.
- **An empty slot.** Optional inputs (Map, Mask) and other unconnected slots aren't hashed
  (Node.cpp:1158-1161). A mask's enabled knob is set from connectedness by
  `refreshMaskEnabledNess` (Node.cpp:8222, from NodeInputs.cpp:1632). That is a knob write, so
  it marks.
- **Members refreshed from input state that stay outside the hash.** None of them feeds pixels:
  - IBlur, IWarp and IErodeDilate map warnings: persistent messages (IBlur.cpp:964, IWarp.cpp:1172,
    IErodeDilate.cpp:1783).
  - Switch and Dissolve display range (Switch.cpp:100-121, Dissolve.cpp:180).
  - Raw* knob visibility and warnings: RawDemosaic.cpp:633, RawHighlights.cpp:989,
    RawPrepare.cpp:354, RawInputColor.cpp:776.
  - USDRender camera menu (USDRender.cpp:150).
  - TrackerNode marker texture (TrackerNode.cpp:693).
  - ViewerInstance active-input data kind (ViewerInstance.cpp:4150).
- **Input-derived values written through knobs.** These are hashed and do mark:
  - CornerPin and ICornerPin from/to (CornerPin.cpp:780, ICornerPin.cpp:140)
  - Transform and ITransform center (Transform.cpp:579, ITransform.cpp:183)
  - RawTemperature presets and spot defaults (RawTemperature.cpp:540, :414)
  - RawColorCalibration options (RawColorCalibration.cpp:1179, :1129). `populateChoices`
    invalidates the knob hash (KnobTypes.cpp:859, :888), and choice options are hashed
    (`appendExtraContentHash` KnobTypes.cpp:765).
- **Overrides the plan listed that don't exist.** STMap and IDistort have no `onInputChanged`
  override on this branch. The ones that exist are listed above, plus RotoPaint.cpp:717 (roto
  context input) and OfxEffectInstance.cpp:1798 (`clipInstanceChangedAction`).

## Covered by tests

- Seeded stress (`Tests/CacheStress.h`): `CacheStressTest.STMapWithAToggledMap`, `MergeSwitchAndDissolveWithGrowingInputs` and `GradeBlurTransformInsideAGroup` connect, disconnect, replace and swap inputs and compare every cached step with a render from empty caches; `CacheStressConcurrentTest.InputEditsDuringARender` edits inputs while a gated render holds the node, then dequeues and revisits; `CacheStressConcurrentTest.DequeueUnderARestartedRender` starts a render between the dequeue request and the dequeue; `CacheStressTest.ViewerABSwitch` runs the A/B sequence of T2.

| Test | What it pins |
| --- | --- |
| `Tests/HashInvariants_Test.cpp:504` | Connect and disconnect change the hash and restore it. |
| `Tests/HashInvariants_Test.cpp:523`, `Tests/NodeContentHash_Test.cpp:216` | Input swap. |
| `Tests/NodeContentHash_Test.cpp:197` | Connecting and disconnecting. |
| `Tests/LazyHash_Test.cpp:364` | Reconnection above a split rehashes only the branch that is read. |
| `Tests/LazyHash_Test.cpp:659` | GroupInput rewire reaches downstream. |
| `Tests/LazyHash_Test.cpp:612` | Group disable reaches inner nodes. |
| `QueuedInputEditTest.NotAppliedUnderARenderThatStartedAfterTheRequest`, `AppliedWhenTheLastRenderEnds` (Tests/QueuedInputEdit_Test.cpp) | A queued edit waits for every render holding the node, then applies. |
| `QueuedInputEditTest.InputsStayQueuedUnderARenderStartedByAValueCallback` | A render started on the main thread by a value callback during the dequeue keeps the inputs it found; the knob value already dequeued stands. |
| `QueuedInputEditTest.ARenderCannotStartBetweenTheCheckAndTheApply` | A render that tries to start while a direct edit is between its check and its write (held there by `Node::setInputEditHookForTesting`) waits, then sees the new input. |

| `QueuedInputEditTest.ARenderStartingAfterTheWriteReadsTheNewHash` | A render waiting to start while an edit writes the input reads the new hash (T8). |
| `CacheStressConcurrentTest.OnInputChangedUnderTheNewHash` | A region of definition stored from `onInputChanged` is keyed under the new hash: connect, revert and connect again render as from empty caches (T4). |
| `CacheStressConcurrentTest.ViewerABSwitchDuringRender` | A hash read at the moment an A/B switch marks the viewer leaves it under the hash of the input shown (T3). |

Not found: a test of precomp Pre-Render toggling.

## Suspected holes

- **T3. `setActiveInputAndRefresh` marks before it changes. Fixed in M260.**
  `InspectorNode::setActiveInputAndRefresh` called `computeHash()` before `onInputChanged` updated
  `_activeInputs`. A render thread that read the viewer hash in between recomputed it from the old
  active inputs and cleared the mark, and nothing marked it again. It now marks after
  `onInputChanged`. `Node::onInputChanged` still runs the viewer effect's `onInputChanged` under
  the hash from before the switch, as T4 described for the other edits.
- **T4. `onInputChanged` runs under the pre-edit hash. Fixed in M260** for the input edits and
  the dequeue, which now mark under the render-start lock before `onInputChanged` runs. The
  analysis below describes the code before the fix. On the real path, `connectInput` (:867 then
  :870), `replaceInputInternal` (:958/:962), `switchInput0And1` (:1062/:1065), the disconnects
  (:1177/:1180, :1245/:1248) and `dequeueActions` all call `onInputChanged` before `computeHash()`.
  `Node::onInputChanged` installs frame args. `buildArgsMap` reads N's hash, which isn't marked yet,
  so it is the pre-edit value, while `getInput` already returns the new input. Whatever
  `effect->onInputChanged` (:1679) stores in N's own ActionsCache or metadata cache is keyed under
  N's old hash with the new topology. `onNodeHashChanged` keeps entries of earlier hashes
  (keys.md), so reverting the edit could serve them.

  Native overrides query only the input's own answers (`input->getRenderHash()`, e.g.
  Transform.cpp:674, CornerPin.cpp:805). An input is keyed on its own hash, so those are correct.
  The exposure is OFX `clipInstanceChangedAction` (OfxEffectInstance.cpp:1798) and anything that
  asks N itself. Not fixed by M202: N isn't dirty, so the snapshot agrees.
- **T5. Group connection changes in the queue don't count as a change.**
  `NodeGroup::dequeueConnexions` swaps the group's GroupInput/Output lists without setting
  `hasChanged` in `dequeueActions`. Whether a GroupInput or GroupOutput added during a render marks
  the group some other way (node creation) is not verified here. Nothing queues those lists:
  `notifyNodeActivated` writes both copies at once, and `notifyNodeDeactivated` too, except that
  removing a GroupInput returns before it copies `inputs` to `guiInputs`, so the next dequeue
  would put the removed GroupInput back. Open for a decision (M260).
- **T8. A render can start between an input write and the hash mark. Fixed in M260:** the mark is
  made under the render-start lock, right after the write. Before the fix, the render-start lock covered
  the check and the write, not the `computeHash()` that follows outside it. A render that starts in
  between reaches the new inputs while N's hash may still be clean at its pre-edit value, the same
  window T4 describes for `onInputChanged`. Narrow (main thread to render thread); M202's snapshot
  closes it for gated stores, because the mark moves the count before the store.

## Fixed (historical)

The analysis below describes the code before each fix.

- **T1. A dequeue could swap inputs under a render.** Before the fix, between the emit of
  `mustDequeueActions` and the slot running, `nodeIsDequeuing` was still false, so a new render
  didn't wait and captured N's hash from the old real inputs. `dequeueActions` then swapped `inputs`
  without checking `isNodeRendering()`; the render's `getInput(i)` returned the new input, which had
  no frame args in that render's map (renderRoI logged "[BUG]" and used the live hash), and its
  outputs were stored under N's old hash with the new topology. **Fixed by M202** for its gated
  stores (the dequeue's `computeHash()` moves the mark count and the recomputed hash, so
  `CleanHashSnapshot::allowsStoreUnder` refuses or retracts). **Fixed in M242** for queued edits:
  `dequeueActions` returns without applying while the node is rendering, the count goes up under the
  dequeuing lock, and the last render to end emits the dequeue again. So that an edit during
  playback doesn't wait for playback to stop, a deferred dequeue with something queued aborts the
  viewer renders below the node. A render started on the main thread from a value callback inside
  the dequeue did not wait for it either and could see the inputs swapped under it; the swap now
  re-checks under the render-start lock and leaves the inputs queued.
- **T2. Viewer A/B selection didn't mark the viewer.** The viewer's content hash folds its active
  inputs, but `InspectorNode::setInputA`/`setInputB` left the hash as it was, and the callers
  (`Gui/ViewerTab30.cpp`, `Gui/PyGuiApp.cpp`, then `abortViewersAndRefresh`) marked nothing either.
  The `FrameKey` names the input rendered, so a plain switch was safe; a stale hit needed a
  sequence:
  1. A=B=X. Switch B to Y: no mark. Y's v1 is cached under (Y, H(X,X)).
  2. Edit Y, so the viewer rehashes to H(X,Y'). Switch B back to X: no mark.
  3. Edit and revert X, so the viewer rehashes to H(X,X).
  4. Switch B to Y: no mark. The lookup (Y, H(X,X)) serves Y's v1, which is stale.
  **Fixed in M242:** `setInputA`/`setInputB` recompute the hash when the selection changes.
- **T6. Background renders never dequeued.** `unsetNodeIsRendering` didn't emit in background mode,
  so an input edit queued during a background render (from Python) was never applied. **Fixed in
  M242:** the last render to end emits in every mode. The stress tests still call
  `Node::dequeueActions` themselves, so they don't wait on the event loop.
- **T7. A direct edit raced a render that started after the check.** An input edit applied at once
  when `isNodeRendering()` was false, and nothing stopped a render from starting between that check
  and the write of the inputs; the render then read the new inputs under the old hash. **Fixed in
  M242:** every edit decides and writes under the render-start lock, the lock under which a render
  raises the count, so a render either holds the node before the check (the edit queues) or starts
  after the write.
