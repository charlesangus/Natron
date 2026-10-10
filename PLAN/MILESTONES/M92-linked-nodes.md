# M92 - Linked Nodes

Replace node cloning with **Linked Nodes**. Today a clone is a one-way master/slave binding: every knob of the child follows the parent's knob, greyed out, and edits flow parent → child only (`KnobHolder::slaveAllKnobs`, `Engine/Knob.cpp:6209-6275`; `KnobHelper::slaveToInternal`, `Knob.cpp:3693-3764`). A Linked Node is symmetric instead: each linked knob belongs to a **link group** that holds one shared value per dimension — static value, animation curve and expression. An edit on any member, original or linked copy, changes every member still linked on that knob. Every linked knob gets a small button to break its link; an unlinked knob is fully independent. Clicking the button again (or the right-click menu) re-links it, after a warning that the knob's local values will be discarded. "Clone"/"Declone" disappear from the UI and the code; this is a hard cut with no compatibility path for old clones.

Out of scope, left unchanged: the per-knob "Copy Link → Paste Link" / drag-to-link one-way links (`KnobGui.cpp:555,613`, `KnobWidgetDnD.cpp:450`), group alias links, and the internal `slaveTo` users (MultiInstancePanel, Roto, Tracker). They keep using `slaveTo`; only the node-level clone path is removed.

## Design (applies to every task)

- **Link group.** A shared-state object per knob dimension set: every member knob reads and writes the group's values/curves/expression instead of its own. Members are symmetric — there is no master. A change through any member refreshes and re-evaluates every member's holder (the equivalent of today's `refreshListenersAfterValueChange` listener refresh, `Knob.cpp:4020-4075`), with a guard against re-entrant refresh. Linked knobs are **editable** (never greyed out).
- **Linking scope.** "Create Linked Node" links the same knob set the old clone linked (plugin and user knobs matched by name; buttons, pages, groups and separators excluded). Inputs are not linked. Refused for viewers and multi-instance nodes, as before. Creating a Linked Node from a node that is already linked joins the existing groups, so all members stay in sync.
- **Unlink** (per knob): the knob leaves its group taking a private copy of the group's current state (values, keyframes, expression); nothing visible changes at that moment. **Re-link**: the knob discards its local state and rejoins its group, adopting the group's state. Both are undoable. The GUI warns before re-link; the Python API does not.
- **Group lifetime.** A node is a "Linked Node" while at least one of its knobs shares a group with another node. Deleting a member leaves the others linked to each other; a group reduced to one member dissolves (that knob becomes ordinary). "Unlink All" on a node unlinks every knob of that node.
- **Hard cut.** No code reads the old clone format (`NodeSerialization`'s `MasterNode`). Old clones cannot exist in projects this fork loads.

## Phase 92.1: Engine link groups

- [ ] M92.P1.T1 — Add shared-state knob link groups with per-knob unlink and re-link
  - files: new `Engine/KnobLinkGroup.h`, `Engine/KnobLinkGroup.cpp`, `Engine/Knob.h`, `Engine/Knob.cpp`, `Engine/KnobImpl.h` (+ the Engine source list in CMake), new `Tests/KnobLinkGroup_Test.cpp`
  - approach: Implement the link group from **Design**. Route the value/curve/expression accessors of a linked dimension in `Knob<T>` (`KnobImpl.h`, e.g. `getValue` at `:716`) through the group's storage rather than the knob's own, the same way a slaved dimension currently reads its master — but symmetrically, and with writes from any member going to the group. On change, notify every member so its holder re-evaluates and its GUI refreshes. API on `KnobI`: `linkTo(KnobIPtr other)` (join or create other's group), `unlinkFromGroup()` (leave with a private copy), `relinkToGroup()` (discard local, rejoin — the knob remembers its group after unlinking so it can re-link), `isLinked()`, `getLinkGroup()`, `getLinkedKnobs()`. Keep `slaveTo` untouched. A group holds members by weak pointer; a group with one live member dissolves.
  - verify: `Tests/KnobLinkGroup_Test.cpp` (engine ctest): two and three knobs on separate holders, linked: a value set on any member reads back on all; a keyframe added on one appears on all; an expression set on one evaluates on all; unlink keeps the current value and then edits stop propagating both ways; re-link adopts the group's value; destroying one member leaves the other two linked; a two-member group dissolves when one is destroyed.
  - size: L
- [ ] M92.P1.T2 — Node-level Linked Node API replacing the clone master/slave path
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/Knob.h`, `Engine/Knob.cpp`, new `Tests/LinkedNode_Test.cpp`
  - approach: Add `Node::linkAllKnobsTo(NodePtr other)` (links the knob set from **Design** via P1.T1's `linkTo`), `Node::unlinkAllKnobs()`, `Node::isLinkedNode()`, `Node::getLinkedNodes()` (the other nodes sharing at least one live group with this one), and a `linkedNodesChanged` signal the GUI can listen to (emitted when membership changes, including after a member is deleted). Refuse viewers and multi-instance nodes. Do **not** remove the old `slaveAllKnobs`/`masterNode` machinery yet — P2/P3 still call it until they are converted; P4.T1 deletes it.
  - verify: `Tests/LinkedNode_Test.cpp`: create two nodes of the same plugin, link them; edit a knob on the copy → the original changes, and vice versa; a third node linked to the copy stays in sync with both; `unlinkAllKnobs` on one node leaves the other two linked; deleting a node updates `getLinkedNodes()` on the rest; linking a viewer is refused.
  - size: M

## Phase 92.2: Node graph

- [ ] M92.P2.T1 — Stop right-click Duplicate and Paste from creating clones
  - files: `Gui/NodeGraph35.cpp`
  - approach: In `NodeGraph::showMenu`, the Duplicate (`:500-502`) and Paste (`:508-510`) branches both call `cloneSelectedNodes(scenePos)`. Point them at the duplicate and paste handlers the keyboard shortcuts already use.
  - verify: In the GUI, right-click Duplicate and right-click Paste produce independent nodes (editing the copy leaves the original unchanged, and the copy is not drawn as linked).
  - size: S
- [ ] M92.P2.T2 — Replace the Clone/Declone actions with Create Linked Node / Unlink Node
  - files: `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGraph40.cpp`, `Gui/NodeGraphPrivate10.cpp`, `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: Rename the shortcut IDs and labels (`kShortcutIDActionGraphClone`/`Declone`, `ActionShortcuts.h:455-459`) to "Create Linked Node" / "Unlink Node", keeping Alt+K / Alt+Shift+K. Rewrite `cloneSelectedNodes` (`NodeGraph40.cpp:233-338`) as `createLinkedNodes`: paste a copy of each selected node, then call P1.T2's `linkAllKnobsTo` (replacing `pasteNode`'s `slaveAllKnobs` call, `NodeGraphPrivate10.cpp:259-263`). Allow linking a node that is already linked (it joins its groups); keep refusing viewers and multi-instance nodes. Rewrite `decloneSelectedNodes` and `DecloneMultipleNodesCommand` as `unlinkSelectedNodes` / `UnlinkMultipleNodesCommand`, whose undo re-links each knob to the group it left. No user-visible string may still say "clone" for this feature.
  - verify: In the GUI: Alt+K and the right-click menu create a Linked Node; edits propagate both ways; Alt+Shift+K unlinks; Ctrl+Z / Ctrl+Shift+Z undo and redo both; `grep -rn -i 'declone\|clone node' Gui/` finds nothing.
  - size: M
- [ ] M92.P2.T3 — Draw Linked Nodes in the graph
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/DockablePanel.cpp`
  - approach: Replace the clone visuals: remove `_clonedColor` / `isDrawnAsClone()` (`NodeGui.cpp:429,1908,1916`) and the clone exception that hides the user-colour border (`:1948`, accepted as a stopgap in M24 - Node Graph Category Colour's UAT note 9), so a Linked Node keeps its category body and user border. Replace the single master `LinkArrow` (`onAllKnobsSlaved`, `:2705-2745`) with dashed link lines between every pair of linked nodes in the same group view (no arrowhead, as the link has no direction), rebuilt on P1.T2's `linkedNodesChanged`. Add a small link glyph to the node so a Linked Node is recognisable when its partners are off-screen or in another group. In `DockablePanel.cpp:1616`, drop the clone-specific disabling of "Set key on all parameters".
  - verify: Xvfb screenshots: two Linked Nodes with a dashed line and glyph, one of them with a user colour showing its border; after Unlink Node the line and glyph disappear; deleting one of three linked nodes leaves a line between the remaining two.
  - size: M

## Phase 92.3: Knob link button

- [ ] M92.P3.T1 — Per-knob link button with unlink, re-link and warning
  - files: `Gui/KnobGui.h`, `Gui/KnobGui.cpp`, `Gui/KnobGui10.cpp`, `Gui/KnobGui20.cpp`, `Gui/KnobUndoCommand.h`/`.cpp`
  - approach: On every knob row of a knob that belongs to a link group (linked or unlinked-but-re-linkable), add a small button at the row's end: a blue circle, **filled when linked, empty when unlinked**, with a tooltip naming the linked nodes. Clicking a filled circle unlinks (P1.T1 `unlinkFromGroup`). Clicking an empty circle shows a confirmation dialog — "Re-linking will discard this knob's local values and use the linked value" — and re-links on accept. Add matching "Unlink from Linked Nodes" / "Re-link to Linked Nodes" items to the knob right-click menu (`KnobGui.cpp:733-785`), alongside the existing "Unlink from Node.knob" item that still serves Paste Link. Both actions are undo commands. Linked knobs must not be greyed out: make sure the slave-greying paths (`KnobGuiValue.cpp:1073-1076`, `KnobGui20.cpp:773`) apply only to `slaveTo` links. The button is absent on knobs of non-linked nodes.
  - verify: In the GUI on a Linked Node pair: the circles are filled, the knobs editable; clicking one empties it and the knob then edits independently both ways; clicking it again shows the warning, and accepting restores the shared value; cancelling leaves it unlinked; undo/redo restore each step; knobs on an ordinary node show no circle. Xvfb screenshot of a panel with filled and empty circles.
  - size: L

## Phase 92.4: Persistence, scripting and removal

- [ ] M92.P4.T1 — Save and load link groups, and remove the clone code
  - files: `Engine/NodeSerialization.h`, `Engine/NodeSerialization.cpp`, `Engine/KnobSerialization.h`, `Engine/KnobSerialization.cpp`, `Engine/NodeGroupSerialization.cpp`, `Engine/Knob.h`/`.cpp`, `Engine/Node.h`/`.cpp`, `Tests/LinkedNode_Test.cpp`
  - approach: Serialize each linked knob's group membership — a project-unique group id plus a linked/unlinked flag — and the unlinked knob's own values as usual. On load, rebuild groups after all nodes exist (alongside where `restoreLinks` runs), mapping node names the same way `findMaster` does for pasted/imported nodes, so copy-pasting a set of linked nodes links the pasted copies to each other and not to the originals. Remove the `MasterNode` field (`NodeSerialization.h:259,295,381`) and the clone restore path (`NodeGroupSerialization.cpp:336-348`) with **no** legacy reader. Delete `KnobHolder::slaveAllKnobs` / `unslaveAllKnobs`, the holder's `isSlave` flag, `Node::onAllKnobsSlaved` / `getMasterNode` / `masterNode`, and the `allKnobsSlaved` signal, plus any caller left over from P2. Per-knob `MasterNodeName`/`MasterKnobName` serialization stays — it serves Paste Link.
  - verify: Extend `LinkedNode_Test.cpp`: save a project with three linked nodes, one knob unlinked on one node with a distinct value; reload; the two-way edits still propagate, the unlinked knob keeps its value and re-links on request; copy-paste of two linked nodes yields a pair linked to each other only. `grep -rn 'slaveAllKnobs\|getMasterNode\|_masterNodeName' Engine Gui` finds nothing. Full ctest green.
  - size: L
- [ ] M92.P4.T2 — Python API for Linked Nodes
  - files: `Engine/PyNode.h`/`.cpp`, `Engine/PyParameter.h`/`.cpp`, the Shiboken typesystem and generated binding sources per the repo's existing regeneration process, `Documentation/` Python reference pages for those classes
  - approach: Add `Effect.createLinkedNode()` (returns the new linked node), `Effect.unlinkAllParams()`, `Effect.isLinkedNode()`, `Effect.getLinkedNodes()`, `Param.isLinked()`, `Param.unlink()`, `Param.relink()` (no warning). Leave `Param.slaveTo` / `unslave` as they are (Paste Link). Document them, and replace any "clone" wording for this feature in the user docs with "Linked Node".
  - verify: A Python test (in the repo's existing Python test setup) creates a linked node, sets a value through the copy and reads it on the original, unlinks a param and checks independence, re-links and checks the shared value returns.
  - size: M

**Verification gate:** full ctest green (including `KnobLinkGroup_Test` and `LinkedNode_Test`); `format` and `lint-ci` green; Xvfb screenshots of a Linked Node pair in the graph and its panel with filled and empty link circles; manual GUI checklist — Alt+K creates a Linked Node, edits propagate both ways and to siblings, unlink/re-link with warning, Unlink Node, undo/redo of each, save/reload round trip, right-click Duplicate/Paste produce independent copies; no user-visible "Clone"/"Declone" for this feature remains.

## Decisions

- 2026-10-10 — **Added (user):** clones replaced by symmetric Linked Nodes. Edits on any member propagate to all members still linked (siblings included); each knob can be unlinked and later re-linked with a warning that local values are discarded; the button is a small blue circle, filled when linked, empty when unlinked; links share everything — value, animation and expression. "Clone" is renamed "Linked Node" and the old cloning behaviour is dropped.
- 2026-10-10 — **Hard cut, no legacy clone loading (user):** projects with old clones cannot exist in this fork, so there is no reader or converter for the old `MasterNode` clone format.
- 2026-10-10 — **Shared-state groups, not two-way slaving:** `slaveToInternal` refuses a link back to its own master and allows only one master per dimension, so symmetric links cannot be built from mutual `slaveTo`. A shared value store per group gives symmetry and sibling sync for free. Per-knob Paste Link and the internal `slaveTo` users are out of scope and keep the one-way mechanism.
- 2026-10-10 — **Deps M24:** M24 - Node Graph Category Colour is reworking the same `NodeGui` body/border drawing (including the clone colour exception), so this milestone starts after it lands.
