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

Every edit path follows the same shape. It decides real-or-queued from `isNodeRendering()`, aborts
the viewers below, writes the inputs, calls `onInputChanged` (real path only), then `computeHash()`
(Node.cpp:1917, which just calls `markHashDirty()`), then `endInputEdition(true)`.

| Function | Real or queued | `onInputChanged` | `computeHash()` | Notes |
| --- | --- | --- | --- | --- |
| `Node::connectInput` NodeInputs.cpp:801 | Queued if `isNodeRendering()` (:829). The real path writes both `inputs` and `guiInputs` (:845-847). | :865, before the hash mark | :868, after `onInputChanged` | `abortViewerRendersForEdit` (:830). `recheckInvalidExpressions` (:852). |
| `Node::replaceInputInternal` NodeInputs.cpp:883 | Caller's `useGuiInputs`; `replaceInput` (:970) uses `isNodeRendering()` (:977). | :951, plus `invalidateDetachedInputDataKind` (:952) | :955 | Viewer abort is in `replaceInput` (:978), not in the internal function. |
| `Node::switchInput0And1` NodeInputs.cpp:983 | `isNodeRendering()` (:1028) | :1054-1055 | :1057 | Swaps the first non-mask input with the next one. |
| `Node::disconnectInput(int)` NodeInputs.cpp:1117 | `isNodeRendering()` (:1122) | :1172, plus detached data kind (:1173) | :1175 | No viewer abort while the node is being destroyed (:1124-1133). |
| `Node::disconnectInputInternal` NodeInputs.cpp:1189 | Caller's `useGuiValues`; `disconnectInput(Node*)` (:1257) uses `isNodeRendering()` (:1262). | :1236-1237 | :1239 | |
| `Node::onInputChanged` NodeInputs.cpp:1615 | Real inputs only (main thread) | — | Not called | Order: data-kind cache cleared (:1625), mask enabledness (:1632), Inspector active inputs (:1636), frame args installed (:1656), `effect->onInputChanged` (:1671), render clones cleared (:1675). Then it forwards to a group's GroupInput outputs (:1696-1701), a GroupOutput's container outputs (:1716) and a precomp's outputs (:1729). |
| `Node::endInputEdition` NodeInputs.cpp:1584 | — | — | — | At recursion depth 0, if anything changed: `refreshInputRelatedDataAfterInputChange` (:1597, unless a node tree is being created), `refreshDynamicProperties`, then `renderCurrentFrame` on the viewers below (:1608). |
| `Node::dequeueActions` Node.cpp:7931 | Applies the queue | Per changed slot, inside begin/end edition | Only if something changed or values were dequeued | Copies `guiInputs` over `inputs` and `guiOutputs` over `outputs`. `NodeGroup::dequeueConnexions` doesn't set `hasChanged` (NodeGroup.cpp:1397). Calls `refreshIdentityState()` after the hash mark. |
| `Node::setNodeIsRenderingInternal` Node.cpp:7836 | — | — | — | Off the main thread it waits while the node is dequeuing, then increments `nodeIsRendering` and recurses through `getInput` (real inputs). Set by `RenderingFlagSetter` (ViewerInstance.cpp:1248, OutputSchedulerThread.cpp:2531, :2699, Node.cpp:4819, RotoPaint.cpp:854). |
| `Node::unsetNodeIsRendering` Node.cpp:7899 | — | — | — | When the count reaches 0, and not in background mode, emits `mustDequeueActions`. That is connected to `dequeueActions` with the default connection type (Node.cpp:227), so from a render thread it is queued to the main thread. |

`InspectorNode::connectInput` (NodeInputs.cpp:1789) and `InspectorNode::setActiveInputAndRefresh`
(:1837) add their own `computeHash()` calls (:1830, :1847).

### The queued path, step by step

1. A render sets `nodeIsRendering` on the nodes it reaches (Node.cpp:7836). Its frame args hold
   each node's hash, computed from the real inputs (ParallelRenderArgs.cpp:2004).
2. The user connects input `i` on node N. `isNodeRendering()` is true, so only `guiInputs[i]` and
   the new input's `guiOutputs` change (NodeInputs.cpp:847, `connectOutput` :1099). Viewer renders
   below are aborted with auto-restart (`abortViewerRendersForEdit` EffectInstance.cpp:6239), which
   keeps the oldest render (keys.md K1). Other renders, such as writers, continue.
3. `computeHash()` marks N. The recompute reads the real inputs, which haven't changed, so the hash
   value is the same. `recomputeHashIfDirty` calls `onNodeHashChanged` only on a change
   (Node.cpp:1592), so the ActionsCache isn't touched. Renders still running are consistent: old
   inputs under the old hash.
4. The last render below N finishes. `unsetNodeIsRendering` emits `mustDequeueActions`
   (Node.cpp:7918). That is queued to the main thread.
5. **Window.** Between the emit and the slot running, a new render may start. `nodeIsDequeuing` is
   still false, so `setNodeIsRenderingInternal` doesn't wait. The render captures N's hash from the
   old real inputs.
6. `dequeueActions` runs on the main thread. It doesn't check `isNodeRendering()`
   (Node.cpp:7931-7975). It swaps `inputs`, calls `onInputChanged` for each changed slot, then
   `computeHash()` and `refreshIdentityState()`.
7. A render from step 5 is still running. Its `getInput(i)` now returns the new input, which has no
   frame args in that render's map (renderRoI then logs "[BUG]" and uses the live hash,
   EffectInstanceRenderRoI.cpp:349-358). Its own outputs are stored under N's old hash with the new
   topology. See T1.

## Not invalidated by

- **An edit that is only queued** (step 3). By design the hash keeps the real topology until the
  dequeue.
- **Viewer A/B selection.** `ViewerInstance::setInputA`/`setInputB` (ViewerInstance.cpp:4305,
  :4320) call `InspectorNode::setInputA`/`setInputB` (NodeInputs.cpp:1977, :1988). Those write
  `_activeInputs` and mark nothing. The callers are `Gui/ViewerTab30.cpp:1072`, `:1091`,
  `:1122-1123`, `:1141`, `:1159` and `Gui/PyGuiApp.cpp:786`, `:811`; they then call
  `abortViewersAndRefresh` (`Gui/ViewerTab10.cpp:368`), which marks nothing either. See T2.
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

| Test | What it pins |
| --- | --- |
| `Tests/HashInvariants_Test.cpp:504` | Connect and disconnect change the hash and restore it. |
| `Tests/HashInvariants_Test.cpp:523`, `Tests/NodeContentHash_Test.cpp:216` | Input swap. |
| `Tests/NodeContentHash_Test.cpp:197` | Connecting and disconnecting. |
| `Tests/LazyHash_Test.cpp:364` | Reconnection above a split rehashes only the branch that is read. |
| `Tests/LazyHash_Test.cpp:659` | GroupInput rewire reaches downstream. |
| `Tests/LazyHash_Test.cpp:612` | Group disable reaches inner nodes. |

Not found: a test of the queued path (`dequeueActions`, `RenderingFlagSetter`), viewer A/B
switching, precomp Pre-Render toggling, or `onInputChanged` work stored under the pre-edit hash.

## Suspected holes

- **T1. A dequeue can swap inputs under a render.** Steps 5–7 above. `dequeueActions`
  (Node.cpp:7931) doesn't re-check `isNodeRendering()`, and the wait in `setNodeIsRenderingInternal`
  (Node.cpp:7848) covers only a dequeue that has already started. A render started in the window
  stores new-topology pixels under the old hash. **Fixed by M202** for its gated stores: the
  dequeue's `computeHash()` moves the mark count and the recomputed hash, so
  `CleanHashSnapshot::allowsStoreUnder` refuses or retracts. On this branch, nothing guards it.
- **T2. Viewer A/B selection doesn't mark the viewer.** The viewer's content hash folds its
  active inputs (Node.cpp:976-980), but `InspectorNode::setInputA`/`setInputB`
  (NodeInputs.cpp:1977-1995) leave the hash as it was. The viewer's `FrameKey` also names the input
  rendered (`inputToRenderName`, ViewerInstance.cpp:1378), so a plain switch looks up under another
  name and is safe. A stale hit needs a sequence. Example:
  1. A=B=X. Switch B to Y: no mark. Y's v1 is cached under (Y, H(X,X)).
  2. Edit Y, so the viewer rehashes to H(X,Y'). Switch B back to X: no mark.
  3. Edit and revert X, so the viewer rehashes to H(X,X).
  4. Switch B to Y: no mark. The lookup (Y, H(X,X)) serves Y's v1, which is stale.

  Not fixed by M202: the viewer isn't dirty, so its snapshot agrees. Also cross-referenced in
  viewer-and-timeline.md (P2.T1). A Phase 242.3 stress candidate.
- **T3. `setActiveInputAndRefresh` marks before it changes.** `InspectorNode::setActiveInputAndRefresh`
  (NodeInputs.cpp:1837) calls `computeHash()` (:1847) before `onInputChanged` updates
  `_activeInputs` (:1850 → :1636 → `refreshActiveInputs` :1865). A render thread that reads the
  viewer hash in between recomputes it from the old active inputs and clears the mark, and nothing
  marks it again. Narrow window (main thread to render thread). Not fixed by M202, for the same
  reason as T2.
- **T4. `onInputChanged` runs under the pre-edit hash.** On the real path, `connectInput` (:865 then
  :868), `replaceInputInternal` (:951/:955), `switchInput0And1` (:1054/:1057), the disconnects
  (:1172/:1175, :1236/:1239) and `dequeueActions` all call `onInputChanged` before `computeHash()`.
  `Node::onInputChanged` installs frame args (NodeInputs.cpp:1656). `buildArgsMap` reads N's hash,
  which isn't marked yet, so it is the pre-edit value, while `getInput` already returns the new
  input. Whatever `effect->onInputChanged` (:1671) stores in N's own ActionsCache or metadata cache
  is keyed under N's old hash with the new topology. `onNodeHashChanged` keeps entries of earlier
  hashes (keys.md), so reverting the edit could serve them.

  Native overrides query only the input's own answers (`input->getRenderHash()`, e.g.
  Transform.cpp:674, CornerPin.cpp:805). An input is keyed on its own hash, so those are correct.
  The exposure is OFX `clipInstanceChangedAction` (OfxEffectInstance.cpp:1798) and anything that
  asks N itself. Not fixed by M202: N isn't dirty, so the snapshot agrees.
- **T5. Group connection changes in the queue don't count as a change.**
  `NodeGroup::dequeueConnexions` (NodeGroup.cpp:1397) swaps the group's GroupInput/Output lists
  without setting `hasChanged` in `dequeueActions`. Whether a GroupInput or GroupOutput added
  during a render marks the group some other way (node creation) is not verified here.
- **T6. Background renders never dequeue.** `unsetNodeIsRendering` doesn't emit in background mode
  (Node.cpp:7914), so an input edit queued during a background render (from Python) is never
  applied. This is a correctness question, not cache staleness. The hash stays consistent with the
  real inputs.
