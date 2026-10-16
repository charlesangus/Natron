# M22 - Missing Plugin Placeholder

Full title: Lossless project round-trip with missing plugins

Loading a project whose node references an unavailable plugin currently destroys
that node. It is skipped at load, every edge into and out of it is discarded, and
because save walks the live graph, the next save writes a file with the node gone
for good — knob values, plugin id, version and connections included. A PyPlug
version prompt elsewhere in the same project can trigger an immediate silent save
right after load, so the loss can reach disk without the user ever choosing Save.

This milestone makes a missing plugin non-destructive: the node survives as a
placeholder carrying its own serialization verbatim, keeps its connections, shows
as errored, and is written back out byte-equivalent. Install the plugin, reload,
and the graph is exactly what it always was.

**The invariant:** a load/save round trip with a plugin missing is content-identical
for that node. The placeholder leaves no trace of itself in the file, so a later
load with the plugin present is indistinguishable from never having lost it.

## Phase 22.1: Preserve the node

- [x] M22.P1.T1 — `MissingPluginNode` placeholder effect
  - files: `Engine/Nodes/MissingPluginNode.h`/`.cpp` (new), `Engine/EffectInstance.h` (`PLUGINID_NATRON_MISSING_PLUGIN` beside `PLUGINID_NATRON_READ`), `Engine/AppManager.cpp` (`loadBuiltinNodePlugins`), `Engine/CMakeLists.txt`
  - approach: derive from `NativeEffectBase`; register with `registerBuiltInPlugin<MissingPluginNode>("", false, true)` (`internalUseOnly`, like `RotoSmear`), not a reader or writer so the node picker excludes it. Holds the `Serialization::NodeSerializationPtr` passed through `kCreateNodeArgsPropNodeSerialization`; input count and labels come from its `inputs` keys (override `getNInputs()`/`getInputLabel()` if the description is static). Calls `setPersistentMessage(eMessageTypeError, ...)` naming the missing `pluginID` and version; `render` fails rather than passing through.
  - verify: gtest in `Tests/Serialization/` building one from a hand-built `NodeSerialization` (right input labels, error state), plus a `Tests/PluginIdentity_Test.cpp`-style check that the id is registered and internal-only.
  - size: M

- [x] M22.P1.T2 — Create the placeholder instead of skipping the node
  - files: `Engine/NodeGroupSerialization.cpp` (`NodeCollection::restoreFromSerialization`: the `if (!n)` block ~:251-259, the connect pass ~:310-380)
  - approach: when `createNode` fails, retry once with `PLUGINID_NATRON_MISSING_PLUGIN` and the same serialization, keep the "does not exist in the loaded plug-ins" error-log entry and `mustShowErrorsLog`, and register the node in `createdNodes` so the connect pass wires it. Promote the connect-pass `qDebug()` failures to `appPTR->writeToErrorLog_mt_safe` when `createNodes` is true. Never recurse into a placeholder's `children`.
  - verify: a `Tests/Serialization/` test loads a project with a bogus plugin id: the placeholder exists with its script name and every edge in and out is intact.
  - size: M

- [x] M22.P1.T3 — Re-emit the stored serialization verbatim on save
  - files: `Engine/ProjectSerializationConvert.cpp` (`Node::toSerialization`), `Engine/Nodes/MissingPluginNode.h` (getter for the stored serialization)
  - approach: early branch in `Node::toSerialization` for a placeholder: copy the stored serialization, then overwrite only `inputs` (from the live graph, since the user may rewire), `label`, `scriptName` and position/GUI fields from the live node; clear `oldInputs`. `knobsAge`/`cacheID` are session counters. `nodesToSerialization` needs no change.
  - verify: `ProjectXmlEquivalence`-style test (copy or hoist `saveProjectXml`/`loadProjectXml`/`reloadProjectXml`/`withoutSessionCounters`/`findSerializedNode` from `Tests/Serialization/ProjectXmlEquivalence_Test.cpp`): serialize a real built-in node, rename its `pluginID`, load, save, diff the node after `withoutSessionCounters`, rename back and reload to get the normal node with its knob values; plus a rewired-input case.
  - size: L

- [x] M22.P1.T4 — Stop the load-time silent save from committing losses
  - files: `Engine/Project.cpp` (`loadProject`, ~:268-272), `Engine/ProjectPrivate.cpp` (~:214-220, where `mustSave` is set)
  - approach: in the `mustSave` branch after `loadProjectInternal`, skip `saveProject` when any node in `getNodes_recursive` is a `MissingPluginNode`, and log the skip to the error log.
  - verify: a test with a PyPlug-bump project plus a missing plugin leaves the file on disk unchanged (reuse `Tests/PyPlugInstantiate_Test.cpp`'s fixture if it can load a project; otherwise test the gate directly).
  - size: M

## Phase 22.2: Make it legible

- [ ] M22.P2.T1 — Placeholder appearance in the node graph
  - files: `Gui/NodeGui.cpp` (`onPersistentMessageChanged`, the `_persistentMessage` item ~:823-832, layout ~:1220), `Gui/NodeGui.h`
  - approach: confirm the error text/colour/tooltip already arrive via `persistentMessageChanged`, then give a `MissingPluginNode` a distinct placeholder look (dashed or desaturated outline, or a "missing" label) using the existing error colour; no new indicator vocabulary.
  - verify: Xvfb screenshot (recipe `build/deeprepro/run-gui.sh`) shared with the user before sign-off, plus a PR checklist.
  - size: M

**Verification gate:** full build + ctest green; a project with a missing plugin loads with the node and all its edges intact and the node errored; save/reload with the plugin still missing is content-identical for that node; installing the plugin and reloading yields the original graph with knob values restored and no trace of the placeholder; no silent auto-save occurs while a load error is outstanding.

## Decisions

- 2026-10-15 — **Freshness check at claim**: every task's files were stale after M93 - Serialization Boundary (no `NodeSerialization.cpp`, no `NodeCollectionSerialization::initialize()`; save is `Node::toSerialization` in `ProjectSerializationConvert.cpp`, the boundary type is `Serialization::NodeSerialization`). Intent unchanged; all five briefs rewritten in place. P1.T4 and P2.T1 re-sized S→M.
- 2026-10-15 — **Claimed with known small overlap** (user): `Engine/EffectInstance.h` (one plugin-id define) and the `Project.cpp` load region overlap M100 - Content Node Hashing; resolved at rebase rather than waiting.
- 2026-10-15 — **The placeholder must name the missing plug-in** (user): a dashed outline alone doesn't tell the user what the node was. The node shows a second line with the plug-in's short name and version, the tooltip and error carry the full id, and the panel gets a non-persistent info knob. Part of P2.T1.
- 2026-10-15 — **P1 trade-offs** (P1.T3): user knobs are restored on the placeholder so expressions resolve, but edits to them are discarded on save; pre-label-format projects (inputs by index) lose a placeholder's connections after a re-save (no backward compat); copy/paste of a placeholder is untested.
- 2026-10-15 — **Placeholder knobs are read-only** (user): the save writes the stored serialization verbatim, so edits would be silently lost. Every knob on a placeholder (restored user knobs, its info knob, the standard node knobs) is disabled; reading and expressions still work, renaming and rewiring stay allowed. Supersedes the "edits are discarded" trade-off above.
- 2026-10-16 — **Phase 22.1 green** at 04114d053 (fast run 38833721805): 2252/2252 in legacy and taskgraph, all eight placeholder tests pass. Code: P1.T1 f5cb5042c, P1.T2 c5ccf7892 (+ 2b355f909 compile fix, 04114d053: `getPluginBinaryFromOldID` returns null rather than throwing, so the missing check never fired), P1.T3 84672be3f, P1.T4 136ea9827 (`hasMissingPluginNode` is tested directly; the skip itself only by reading). P2.T1 (2aa03bc4e, a93318759, 0f094e770, d77341eab) is built and green; its screenshot is pending.
- 2026-10-16 — **Gate green; PR #63 opened** at 5d6b10989 (fast run 38847194125, 2250/2250 in both schedulers; Xvfb check passes, with all mutating panel actions greyed). Codex review running. P2.T1 stays open until the user signs off the screenshots.
- 2026-10-16 — **Review round 1** (Codex, PR #63, 5 findings, fixed in 6f15ad86d): a missing PyPlug now becomes a placeholder instead of an editable Group; any failed creation goes through the shared `AppInstance::createNodeOrMissingPluginPlaceholder` (canonical-id retry first), which paste and duplicate use too; cloning a placeholder is refused; Python writes to a placeholder raise `ValueError`; narrative comments removed. Follow-ups from the user: fix the inverted "Load PyPlugs from .py" preference here, and suppress the per-knob "Could not find a parameter" log noise on placeholders.
