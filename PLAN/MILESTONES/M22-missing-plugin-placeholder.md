# Milestone 22: Lossless project round-trip with missing plugins

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

- [ ] M22.P1.T1 — `MissingPluginNode` placeholder effect
  - files: `Engine/Nodes/MissingPluginNode.h`, `Engine/Nodes/MissingPluginNode.cpp`, `Engine/EffectInstance.h` (plugin id constant)
  - approach: an `EffectInstance` holding a `NodeSerializationPtr` verbatim plus the inputs it needs to keep connections alive: `getNInputs()`/`getInputLabel()` come from the stored `_inputs` map, so every edge has a slot to land in. Declares an error via `setPersistentMessage(eMessageTypeError, ...)` naming the missing plugin id and version, and refuses to render rather than passing through. Follow `ReadNode`'s precedent for a container that survives without the thing it wraps (`ReadNode.cpp:446-563`, `:602-624`). It must **not** be registrable from the node menu — it is only ever constructed by the loader.
  - verify: unit test — constructing one from a hand-built `NodeSerialization` exposes the right input count and labels and reports an error state; it does not appear in the plugin menu.
  - size: M

- [ ] M22.P1.T2 — Create the placeholder instead of skipping the node
  - files: `Engine/NodeGroupSerialization.cpp` (the `continue` at ~:262-270)
  - approach: where the loader logs "does not exist in the loaded plug-ins" and skips, construct a `MissingPluginNode` holding `*it` and register it in `createdNodes` under its serialized script name, so the existing connection-restoration pass below (~:320-395) wires it like any other node. Keep the error-log entry; it is how the user finds out. Also promote the edge-restore failure at ~:386-392 from `qDebug()` to the error log — a dropped connection is not console-only news.
  - verify: integration test — a project referencing an unavailable plugin loads with the placeholder present, its script name intact, and every edge into and out of it still connected.
  - size: M

- [ ] M22.P1.T3 — Re-emit the stored serialization verbatim on save
  - files: `Engine/NodeGroupSerialization.cpp` (`NodeCollectionSerialization::initialize()`), `Engine/NodeSerialization.cpp`/`.h`
  - approach: `initialize()` builds a fresh `NodeSerialization` from each live node; for a placeholder it must instead emit the stored one unchanged, so plugin id, version, knob values, user pages, planes and children all survive exactly. The placeholder contributes nothing of its own — no marker, no substituted id. Watch the connection data specifically: `_inputs` must reflect the graph as it stands (the user may have rewired around the placeholder), not blindly echo the loaded map.
  - verify: round-trip test — load a project with a missing plugin, save it, and diff the node's serialized form against the original; they must be equivalent. Then load again with the plugin available and confirm a normal node with its knob values restored.
  - size: L

- [ ] M22.P1.T4 — Stop the load-time silent save from committing losses
  - files: `Engine/Project.cpp` (~:261-266), `Engine/ProjectPrivate.cpp`
  - approach: `mustSave` (set by the PyPlug version prompt) triggers `saveProject()` immediately after load. With M22.P1.T3 in place a placeholder round-trips safely, so this is no longer destructive — but the auto-save should still not fire while any node is in a load-error state, since the user has not seen the error log yet and has consented to nothing. Gate it, and say plainly in the log if an auto-save was skipped for that reason.
  - verify: test — a project with both a PyPlug version bump and a missing plugin does not silently rewrite itself on load.
  - size: S

## Phase 22.2: Make it legible

- [ ] M22.P2.T1 — Placeholder appearance in the node graph
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: the persistent error message from M22.P1.T1 should already surface through the existing error indicator; confirm that and make the node read as a placeholder rather than a broken ordinary node — it should be obvious at a glance which nodes are standing in for something absent. Reuse the existing error affordances; do not add a new indicator vocabulary.
  - verify: manual GUI check (no GUI test harness exists in this repo) plus a reviewer checklist in the PR.
  - size: S

**Verification gate:** full build + ctest green; a project with a missing plugin loads with the node and all its edges intact and the node errored; save/reload with the plugin still missing is content-identical for that node; installing the plugin and reloading yields the original graph with knob values restored and no trace of the placeholder; no silent auto-save occurs while a load error is outstanding.
