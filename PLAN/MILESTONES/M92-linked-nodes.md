# M92 - Linked Nodes

Replace node cloning and every user-facing one-way knob link with **two-way links**. Today a clone, a "Paste Link", a drag-to-link and a group alias are all the same one-way master/slave binding: the slave dimension follows its master, greyed out, and edits flow master → slave only (`KnobHelper::slaveToInternal`, `Engine/Knob.cpp:3693-3764`; clones via `KnobHolder::slaveAllKnobs`, `Knob.cpp:6209-6275`). The new model is symmetric: each linked knob dimension belongs to a **link group** that holds one shared value — static value, animation curve and expression. An edit on any member changes every member still linked.

- **Linked Nodes** replace clones: "Create Linked Node" makes a copy of a node with every knob linked to the original. Each linked knob gets a small blue circle button, filled when linked and empty when unlinked. Clicking it unlinks the knob; clicking it again re-links it, after a warning that the knob's local values will be discarded. The old cloning behaviour and the word "Clone" disappear.
- **Copy Link → Paste Link and drag-to-link** create the same two-way links, with the same circle button to break and re-link them.
- **Group aliases** ("Create alias on group", Python `Param.setAsAlias`) become two-way too, but are **permanent**: no circle button and no unlink action, since unlinking would leave a dead knob on the Group.
- The unused expression-based "Link to..." dialog (`Gui/LinkToKnobDialog.*`) is deleted.

This is a hard cut: no code reads old clones from project files, and none converts old one-way links to the new ones.

Out of scope, left unchanged: the **internal** `slaveTo` users that wire a node's own knobs together and that the user never creates or sees as links — MultiInstancePanel (`Gui/MultiInstancePanel.cpp:1214,1544`), Roto (`Engine/RotoContext.cpp:1225`, `Engine/RotoDrawableItem.cpp:1512`), Tracker (`Engine/TrackerContext*.cpp`) and the Write node's colourspace binding (`Engine/WriteNode.cpp:814`). The `slaveTo` mechanism stays in the engine for them only.

## Design (applies to every task)

- **Link group.** A shared-state object whose members are `(knob, dimension)` pairs. Each member reads and writes the group's value/curve/expression instead of its own. Members are symmetric, with no master. Members can come from different dimensions, so one dimension can be linked to another (Paste Link onto a single dimension, and EdgeBlur's two `size` dimensions both linked to one `blurSize` dimension). Two knobs can link only if their types are compatible, using the same rule `slaveTo` uses today. A change through any member refreshes and re-evaluates every member's holder, the equivalent of today's listener refresh in `refreshListenersAfterValueChange` (`Knob.cpp:4020-4075`), with a guard against re-entrant refresh. Linked knobs are **editable**, never greyed out.
- **Permanent groups.** A group created by an alias is flagged permanent. Its members have no circle button and no unlink menu item, and refuse `unlink` from Python. Removing the alias knob from the Group (deleting the user knob) dissolves the group, and the inner knob keeps its value as an ordinary knob.
- **Linking scope for Linked Nodes.** "Create Linked Node" links the same knob set the old clone linked: plugin and user knobs matched by name, excluding buttons, pages, groups and separators. Inputs are not linked. It is refused for viewers and multi-instance nodes, as before. Creating a Linked Node from a node that is already linked joins the existing groups, so every member stays in sync.
- **Unlink** (per knob, every dimension of that knob that is in a non-permanent group): the knob leaves its groups and takes a private copy of their current state (values, keyframes, expression), so nothing visible changes at that moment. **Re-link**: the knob discards its local state and rejoins the groups it left, adopting their state. Both can be undone. The GUI warns before re-linking; the Python API does not.
- **Group lifetime.** A node is a "Linked Node" while at least one of its knobs shares a group with a knob on another node created via Create Linked Node. Paste-Link groups do not make a node a Linked Node. Deleting a member leaves the others linked to each other. A group reduced to one member dissolves, and that knob becomes ordinary. "Unlink Node" unlinks every non-permanent knob link on that node.
- **Hard cut.** No code reads the old clone format (`NodeSerialization`'s `MasterNode`) or turns saved one-way user links into link groups.

## Phase 92.1: Engine link groups

- [ ] M92.P1.T1 — Add shared-state knob link groups with per-knob unlink, re-link and permanent groups
  - files: new `Engine/KnobLinkGroup.h`, `Engine/KnobLinkGroup.cpp`, `Engine/Knob.h`, `Engine/Knob.cpp`, `Engine/KnobImpl.h` (+ the Engine source list in CMake), new `Tests/KnobLinkGroup_Test.cpp`
  - approach: Implement the link group from **Design**. In `Knob<T>` (`KnobImpl.h`, e.g. `getValue` at `:716`), make the value, curve and expression accessors of a linked dimension use the group's storage instead of the knob's own. This mirrors how a slaved dimension reads its master today, but symmetrically, and writes from any member go to the group. On a change, notify every member so its holder re-evaluates and its GUI refreshes. Add this API to `KnobI`:
    - `linkTo(int thisDim, KnobIPtr other, int otherDim)` joins the other member's group, or creates one if it has none. `thisDim = -1` links all matching dimensions one-to-one.
    - `linkAsAlias(KnobIPtr other)` does the same and flags the group permanent.
    - `unlinkFromGroups()` leaves every non-permanent group with a private copy of its state.
    - `relinkToGroups()` discards local state and rejoins. After unlinking, the knob remembers its groups so it can re-link.
    - Queries: `isLinked(dim)`, `isLinkPermanent(dim)`, `hasRelinkableGroups()`, `getLinkGroup(dim)`, `getLinkedKnobs()`.
    Check type compatibility as `slaveTo` does. Leave `slaveTo` untouched. A group holds its members by weak pointer, and a group with one live member dissolves.
  - verify: `Tests/KnobLinkGroup_Test.cpp` (engine ctest). Link two and three knobs on separate holders, then check:
    - a value set on any member reads back on all of them;
    - a keyframe added on one appears on all;
    - an expression set on one evaluates on all;
    - a cross-dimension link (dim 1 of A ↔ dim 0 of B) propagates both ways;
    - unlinking keeps the current value, and edits then stop propagating in both directions;
    - re-linking adopts the group's value;
    - unlinking a permanent group is refused;
    - destroying one member leaves the other two linked;
    - a two-member group dissolves when one member is destroyed;
    - linking incompatible types is refused.
  - size: L
- [ ] M92.P1.T2 — Node-level Linked Node API replacing the clone master/slave path
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/Knob.h`, `Engine/Knob.cpp`, new `Tests/LinkedNode_Test.cpp`
  - approach: Add these to `Node`:
    - `linkAllKnobsTo(NodePtr other)` links the knob set from **Design** via P1.T1's `linkTo`, and marks the groups as node-link groups.
    - `unlinkAllKnobs()`.
    - `isLinkedNode()`.
    - `getLinkedNodes()` returns the other nodes sharing at least one live node-link group with this one.
    - `getKnobLinkedNodes()` returns the nodes sharing any group with this one, used by the graph's knob-link lines.
    - A `linksChanged` signal that fires whenever membership changes, including after a member is deleted.
    Refuse viewers and multi-instance nodes. Do **not** remove the old `slaveAllKnobs`/`masterNode` code yet: P2 and P3 still call it until they are converted, and P4.T1 deletes it.
  - verify: `Tests/LinkedNode_Test.cpp`. Create two nodes of the same plugin and link them, then check:
    - editing a knob on the copy changes the original, and vice versa;
    - a third node linked to the copy stays in sync with both;
    - `unlinkAllKnobs` on one node leaves the other two linked;
    - deleting a node updates `getLinkedNodes()` on the rest;
    - linking a viewer is refused.
  - size: M

## Phase 92.2: Node graph

- [ ] M92.P2.T1 — Stop right-click Duplicate and Paste from creating clones
  - files: `Gui/NodeGraph35.cpp`
  - approach: In `NodeGraph::showMenu`, the Duplicate (`:500-502`) and Paste (`:508-510`) branches both call `cloneSelectedNodes(scenePos)`. Point them at the duplicate and paste handlers that the keyboard shortcuts already use.
  - verify: In the GUI, right-click Duplicate and right-click Paste produce independent nodes: editing the copy leaves the original unchanged, and the copy is not drawn as linked.
  - size: S
- [ ] M92.P2.T2 — Replace the Clone/Declone actions with Create Linked Node / Unlink Node
  - files: `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGraph40.cpp`, `Gui/NodeGraphPrivate10.cpp`, `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach:
    - Rename the shortcut IDs and labels (`kShortcutIDActionGraphClone`/`Declone`, `ActionShortcuts.h:455-459`) to "Create Linked Node" / "Unlink Node", keeping Alt+K and Alt+Shift+K.
    - Rewrite `cloneSelectedNodes` (`NodeGraph40.cpp:233-338`) as `createLinkedNodes`: paste a copy of each selected node, then call P1.T2's `linkAllKnobsTo`. This replaces `pasteNode`'s `slaveAllKnobs` call (`NodeGraphPrivate10.cpp:259-263`). Allow linking a node that is already linked, so it joins its groups. Keep refusing viewers and multi-instance nodes.
    - Rewrite `decloneSelectedNodes` and `DecloneMultipleNodesCommand` as `unlinkSelectedNodes` / `UnlinkMultipleNodesCommand`. Undo re-links each knob to the groups it left.
    - No user-visible string may still say "clone" for this feature.
  - verify: In the GUI:
    - Alt+K and the right-click menu create a Linked Node;
    - edits propagate both ways;
    - Alt+Shift+K unlinks;
    - Ctrl+Z and Ctrl+Shift+Z undo and redo both actions;
    - `grep -rn -i 'declone\|clone node' Gui/` finds nothing.
  - size: M
- [ ] M92.P2.T3 — Draw Linked Nodes and knob links in the graph
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/DockablePanel.cpp`
  - approach:
    - Remove the clone visuals: `_clonedColor` / `isDrawnAsClone()` (`NodeGui.cpp:429,1908,1916`), and the clone exception that hides the user-colour border (`:1948`). That exception was accepted as a stopgap in M24 - Node Graph Category Colour's UAT note 9. A Linked Node keeps its category body and its user border.
    - Replace the single master `LinkArrow` (`onAllKnobsSlaved`, `:2705-2745`) with a dashed line between each pair of Linked Nodes in the same group view. Use no arrowhead, since the link has no direction.
    - Turn the per-knob link arrows (`_knobsLinks` / `onKnobsLinksChanged`, `:2814`) into undirected lines driven by `getKnobLinkedNodes()`.
    - Rebuild both kinds of line on P1.T2's `linksChanged`.
    - Add a small link glyph to a Linked Node so it is recognisable when its partners are off-screen or inside another group.
    - In `DockablePanel.cpp:1616`, drop the clone-specific disabling of "Set key on all parameters".
  - verify: Xvfb screenshots:
    - two Linked Nodes with a dashed line and the glyph, one of them with a user colour showing its border;
    - after Unlink Node, the line and glyph disappear;
    - deleting one of three Linked Nodes leaves a line between the remaining two;
    - two ordinary nodes with one Paste-Linked knob show the knob-link line and no glyph.
  - size: M

- [ ] M92.P2.T4 — Separate link and expression lines, with colour and visibility preferences
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/Settings.h`, `Engine/Settings.cpp`, `Gui/NodeGui.cpp`
  - approach:
    - **Tell the lines apart.** Today `Node::KnobLink` (`Node.h:1193`) has no kind: `nodeLinks` mixes expression dependencies (added through `onKnobSlaved(..., true)` at `Knob.cpp:3016,4312`) with slave links. All of them are drawn in one hard-coded green (`NodeGui.cpp:2889-2892`). Add a kind to each entry, either **expression** or **link** (a P1.T1 link group), and build the graph's knob-link lines from both kinds. Internal `slaveTo` links (Roto, Tracker and so on) draw no lines.
    - **Preferences.** On the Node Graph preferences page, next to M24 - Node Graph Category Colour's edge-kind colours, add:
      - "Link line colour". Default: a blue matching the P3.T1 circle.
      - "Expression line colour". Default: today's green (143,201,103).
      - "Show link lines" (default on).
      - "Show expression lines" (default on).
    - **What follows the link settings.** The P2.T3 dashed lines between Linked Nodes are link lines, so they take the link colour and obey "Show link lines". The Linked Node glyph is always shown.
    - **Live updates.** Changing any of these preferences repaints open graphs without a restart.
  - verify:
    - Xvfb screenshots of a graph containing a Linked Node pair, a Paste Link and an expression that references another node, with the two line colours visibly different.
    - With "Show expression lines" off, only the link lines remain. With "Show link lines" off, only the expression lines remain.
    - Changing a colour preference repaints immediately.
    - An expression added or removed through the GUI adds or removes its line.
  - size: M
- [ ] M92.P2.T5 — Hotkey to show or hide all link lines
  - files: `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGui.cpp`
  - approach: The existing "Show Expressions Links" shortcut (`kShortcutIDActionGraphShowExpressions`, Shift+E, `ActionShortcuts.h:419-420`, `GuiApplicationManager10.cpp:928`) toggles `NodeGraph::toggleKnobLinksVisible` (`NodeGraph35.cpp:292`), which affects the current session only. Rename it to "Show Link Lines", keeping Shift+E and the right-click menu's checkable entry (`NodeGraph35.cpp:452-456`). Make it a master switch over both kinds: when off, no expression or link lines are drawn; when on, each kind follows its P2.T4 "Show … lines" preference. It stays a per-session toggle, defaulting to on, and does not change the preferences.
  - verify: In the GUI, with both preferences on, Shift+E hides every expression line and link line, including the dashed lines between Linked Nodes, and pressing it again restores them. With "Show expression lines" off, Shift+E toggles only the link lines. The right-click menu entry's check state follows the hotkey. The shortcut appears as "Show Link Lines" in the shortcut editor.
  - size: S

## Phase 92.3: Knob links in the GUI

- [ ] M92.P3.T1 — Per-knob link button with unlink, re-link and warning
  - files: `Gui/KnobGui.h`, `Gui/KnobGui.cpp`, `Gui/KnobGui10.cpp`, `Gui/KnobGui20.cpp`, `Gui/KnobUndoCommand.h`/`.cpp`
  - approach:
    - **The button.** Add a small button at the end of each knob row whose knob is in a non-permanent group, or has a group it can rejoin. It is a blue circle, **filled when linked and empty when unlinked**, with a tooltip naming the linked node(s) and knob(s). This covers Linked Node links and Paste Link links alike.
    - **Clicking it.** Clicking a filled circle unlinks (`unlinkFromGroups`). Clicking an empty circle shows a confirmation dialog, "Re-linking will discard this knob's local values and use the linked value", and re-links if accepted.
    - **Right-click menu.** Replace the old "Unlink from Node.knob[.dim]" item in the knob right-click menu (`KnobGui.cpp:733-785`) with "Unlink" / "Re-link (discards local values)". Show neither item on permanent (alias) links.
    - **Undo.** Both actions are undo commands.
    - **Not greyed out.** Linked knobs must not be greyed out: the greying code (`KnobGuiValue.cpp:1073-1076`, `KnobGui20.cpp:773`) applies only to the remaining internal `slaveTo` links.
    - **Where it doesn't appear.** Knobs with no group show no circle, and neither do alias knobs.
  - verify: In the GUI, on a Linked Node pair:
    - the circles are filled and the knobs are editable;
    - clicking one empties it, and the knob then edits independently in both directions;
    - clicking it again shows the warning; accepting restores the shared value, and cancelling leaves it unlinked;
    - undo and redo restore each step;
    - knobs on an ordinary node show no circle.
    Also take an Xvfb screenshot of a panel with filled and empty circles.
  - size: L
- [ ] M92.P3.T2 — Make Copy Link → Paste Link and drag-to-link create two-way links
  - files: `Gui/KnobGui.cpp`, `Gui/KnobUndoCommand.cpp`, `Gui/KnobUndoCommand.h`, `Gui/KnobWidgetDnD.cpp`
  - approach: Paste Link (`KnobGui.cpp:555,613`; `PasteUndoCommand` with `eKnobClipBoardTypeCopyLink`, `KnobUndoCommand.cpp:207-222`) and drag-and-drop linking (`KnobWidgetDnD.cpp:450`) call P1.T1's `linkTo` instead of `slaveTo`. This keeps today's dimension mapping: all dimensions, or one dimension onto another. The link starts from the source knob's value, so the knob that was pasted onto takes the copied knob's state. Undo unlinks and restores the target's previous state. The pasted knob then shows P3.T1's circle button.
  - verify: In the GUI:
    - Copy Link on a Blur's size, then Paste Link on another Blur's size: editing either one changes both;
    - the circle on the pasted knob unlinks and re-links it;
    - pasting onto a single dimension links only that dimension;
    - drag-to-link behaves the same as Paste Link;
    - undo restores the target's old value and the link is gone.
  - size: M
- [ ] M92.P3.T3 — Make group aliases two-way and permanent
  - files: `Engine/Knob.cpp`, `Engine/Knob.h`, `Gui/KnobGui.cpp`, `Gui/KnobGui10.cpp`, `Engine/PyParameter.cpp`
  - approach:
    - Re-implement `setKnobAsAliasOfThis` / `getAliasMaster` on P1.T1's `linkAsAlias`. "Create alias on group" (`KnobGui10.cpp:46`, menu at `KnobGui.cpp:797`) and Python `Param.setAsAlias` (`PyParameter.cpp:441`) then give a two-way, permanent link: editing the Group's knob or the inner knob changes both, and neither is greyed out.
    - Remove the "Remove Alias link" menu item (`KnobGui.cpp:773`). Deleting the user knob from the Group is the only way to end an alias.
    - The bundled PyPlugs (`Gui/Resources/PyPlugs/*.py`) use `setAsAlias` and must keep working unchanged.
  - verify: In the GUI:
    - load the bundled Glow PyPlug; editing an aliased knob on the Group changes the inner node's knob, and editing the inner knob changes the Group's knob;
    - alias knobs show no circle and no unlink item;
    - deleting the Group's user knob leaves the inner knob as an ordinary knob with its value.
    `Tests/KnobLinkGroup_Test.cpp` also gets an alias case: two-way propagation, and unlink refused.
  - size: M
- [ ] M92.P3.T4 — Delete the unused Link to... dialog
  - files: `Gui/LinkToKnobDialog.h`, `Gui/LinkToKnobDialog.cpp`, `Gui/KnobGui20.cpp`, `Gui/KnobGui.h`, the Gui CMake source list
  - approach: Delete `LinkToKnobDialog` and the expression-based `KnobGui::linkTo` / `onLinkToActionTriggered` (`KnobGui20.cpp:283-370`), which no menu action reaches.
  - verify: `grep -rn 'LinkToKnobDialog\|onLinkToActionTriggered' Gui/` finds nothing, and the build is clean.
  - size: S

## Phase 92.4: Persistence, scripting and removal

- [ ] M92.P4.T1 — Save and load link groups, and remove the clone code
  - files: `Engine/NodeSerialization.h`, `Engine/NodeSerialization.cpp`, `Engine/KnobSerialization.h`, `Engine/KnobSerialization.cpp`, `Engine/NodeGroupSerialization.cpp`, `Engine/Knob.h`/`.cpp`, `Engine/Node.h`/`.cpp`, `Tests/LinkedNode_Test.cpp`
  - approach:
    - **What is saved.** For each knob dimension in a link group, save a project-unique group id, whether it is currently linked, and the group's node-link and permanent flags. An unlinked knob's own values are saved as usual.
    - **Load and paste.** On load, rebuild the groups after all nodes exist, alongside where `restoreLinks` runs (`Knob.cpp:159-307`). Map node names the same way `findMaster` does for pasted or imported nodes, so that copy-pasting a set of linked nodes links the pasted copies to each other and not to the originals.
    - **Old fields.** Remove the `MasterNode` field (`NodeSerialization.h:259,295,381`) and the clone restore path (`NodeGroupSerialization.cpp:336-348`), with **no** legacy reader. Also remove the per-knob `MasterNodeName` / `MasterKnobName` / `MasterDimension` fields (`KnobSerialization.cpp:105-135`) and their `restoreLinks` handling, including the alias case where `dimension == -1`, unless an internal `slaveTo` user still needs them to persist. If one does, keep only that use and record why in `## Decisions`.
    - **Dead code.** Delete `KnobHolder::slaveAllKnobs` / `unslaveAllKnobs`, the holder's `isSlave` flag, `Node::onAllKnobsSlaved` / `getMasterNode` / `masterNode`, the `allKnobsSlaved` signal, and any caller left over from P2.
  - verify: Extend `LinkedNode_Test.cpp`. Save a project containing three Linked Nodes, with one knob unlinked on one node and given a distinct value, plus one Paste Link between two other nodes, then reload it and check:
    - two-way edits still propagate;
    - the unlinked knob keeps its value and re-links on request;
    - the Paste Link survives;
    - copy-pasting two linked nodes gives a pair linked only to each other.
    Also: `grep -rn 'slaveAllKnobs\|getMasterNode\|_masterNodeName' Engine Gui` finds nothing, and the full ctest is green.
  - size: L
- [ ] M92.P4.T2 — Python API and PyPlug export for two-way links
  - files: `Engine/PyNode.h`/`.cpp`, `Engine/PyParameter.h`/`.cpp`, `Engine/NodeGroup.cpp`, `Gui/Resources/PyPlugs/EdgeBlur.py`, the Shiboken typesystem and generated binding sources per the repo's existing regeneration process, `Documentation/` Python reference pages for those classes
  - approach:
    - **Node functions.** Add `Effect.createLinkedNode()` (returns the new Linked Node), `Effect.unlinkAllParams()`, `Effect.isLinkedNode()` and `Effect.getLinkedNodes()`.
    - **Param functions.** Replace `Param.slaveTo` / `Param.unslave` (`PyParameter.cpp:348-373`) with `Param.linkTo(other, thisDimension=-1, otherDimension=-1)`, `Param.unlink()`, `Param.relink()` (no warning) and `Param.isLinked(dimension=-1)`. `Param.unlink()` raises on an alias link. `Param.setAsAlias` keeps its name, now with P3.T3's semantics.
    - **PyPlug export.** Make it write `param.linkTo(...)` instead of `param.slaveTo(...)` (`exportKnobLinks`, `NodeGroup.cpp:~2704`). Write alias links as `setAsAlias`.
    - **Bundled PyPlugs.** Update `EdgeBlur.py:419-420`, the only bundled PyPlug that calls `slaveTo`, to `linkTo`.
    - **Docs.** Document the new functions, and replace "clone" wording for this feature in the user docs with "Linked Node".
  - verify: A Python test, using the repo's existing Python test setup:
    - create a Linked Node, set a value through the copy and read it on the original;
    - `linkTo` two params on separate nodes and check that edits go both ways;
    - unlink a param and check it is independent;
    - re-link it and check the shared value returns;
    - `unlink` on an alias raises.
    Also export a Group containing a Paste Link as a PyPlug and reload it, and the link is intact. Loading EdgeBlur produces no Python errors.
  - size: M

**Verification gate:**
- The full ctest is green, including `KnobLinkGroup_Test`, `LinkedNode_Test` and the Python test.
- `format` and `lint-ci` are green.
- Xvfb screenshots: a pair of Linked Nodes in the graph, and their panel with filled and empty link circles.
- Manual GUI checklist:
  - Alt+K creates a Linked Node.
  - Edits propagate both ways and to siblings.
  - Unlink and re-link work, with the warning on re-link.
  - Unlink Node works.
  - Paste Link and drag-to-link are two-way and can be broken and re-linked.
  - Group aliases are two-way and cannot be broken.
  - Each of these actions can be undone and redone.
  - A save and reload keeps every link.
  - Right-click Duplicate and Paste produce independent copies.
  - Link lines and expression lines use their own preference colours, and each kind can be hidden in preferences.
  - Shift+E ("Show Link Lines") hides and shows both kinds of line.
  - No user-visible "Clone", "Declone" or "Link to..." remains.

## Decisions

- 2026-10-10 — **Added (user):** clones are replaced by symmetric Linked Nodes:
  - Edits on any member propagate to every member still linked, siblings included.
  - Each knob can be unlinked, and later re-linked with a warning that its local values are discarded.
  - The button is a small blue circle, filled when linked and empty when unlinked.
  - Links share everything: value, animation and expression.
  - "Clone" is renamed "Linked Node", and the old cloning behaviour is dropped.
- 2026-10-10 — **Hard cut, no legacy clone loading (user):** projects with old clones cannot exist in this fork, so there is no reader or converter for the old `MasterNode` clone format.
- 2026-10-10 — **Shared-state groups, not two-way slaving:** `slaveToInternal` refuses a link back to its own master and allows only one master per dimension, so symmetric links can't be built from two `slaveTo` calls. A shared value store per group gives both symmetry and sibling sync with no extra code.
- 2026-10-10 — **Paste Link and drag-to-link become two-way (user):** they use the same link groups and circle button as Linked Nodes. Only the internal `slaveTo` users (MultiInstance, Roto, Tracker, Write's colourspace binding) keep the one-way mechanism.
- 2026-10-10 — **Aliases are two-way but permanent (user):** breaking an alias would leave a knob on the Group that does nothing, so alias links have no circle button and no unlink action. Deleting the Group's knob is the only way to end one.
- 2026-10-10 — **Delete Link to... (user):** the unused expression-based `LinkToKnobDialog` is removed rather than revived. Copy Link → Paste Link and drag-to-link are the ways to link.
- 2026-10-10 — **Line preferences and hotkey (user):**
  - Link lines and expression lines get separate colour preferences and separate "show" preferences.
  - The existing Shift+E session toggle becomes "Show Link Lines", a master switch over both kinds. Each kind still obeys its own preference when the switch is on.
  - The dashed lines between Linked Nodes count as link lines. The Linked Node glyph is always shown.
- 2026-10-10 — **Deps M24:** M24 - Node Graph Category Colour is reworking the same `NodeGui` body and border drawing, including the clone colour exception, so this milestone starts after it lands.
