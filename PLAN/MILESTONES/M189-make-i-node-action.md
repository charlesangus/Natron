# M189 - Make I-Node Action

The single entry point for I-nodes (user, 2026-10-24): a right-click **Make I-Node** on one node or a selection. If the selection is one node with an exact I-variant in M184 - Variable-Size Filter Kit's registry, the node is replaced by its I-variant with its knob values, connections and name kept. Otherwise the selection is wrapped in a M180 - Loop Block Core LoopBegin/LoopEnd block set to Map blend (M183 - Loop Map Blend), and LoopEnd's driver table (M181 - Loop Drivers And UI) opens empty for the user to pick driven knobs (user, 2026-10-25). A matching **Make Base Node** reverses a conversion.

## Scope

**In:**
- Make I-Node / Make Base Node actions in the node graph context menu and the Edit menu, undoable.
- Exact-variant conversion via the registry's knob map, and loop wrapping with sensible defaults (iterations 8, Map blend, Map input exposed on LoopEnd).
- A warning when wrapping a node that exports a transform matrix (Transform, CornerPin) or a Defocus, suggesting its exact variant if one exists.

**Out:**
- The registry, the loop block, the driver editor: M184, M180, M181.

**Edge cases:**
- A selection with several outputs or external inputs feeding several nodes: wrapping requires one output; otherwise the action is disabled with a tooltip.
- A node whose exact variant is not built yet (its milestone not done): the action wraps it in a loop.
- Expressions and links pointing at a converted node keep working (the node name is kept).
- Make Base Node on an I-variant drops the Map connection after a confirm.

## Phase 189.1: Actions

- [ ] M189.P1.T1 — Convert a node to and from its exact I-variant
  - files: `Engine/NodeConvert.h`, `Engine/NodeConvert.cpp` (new), `Engine/Nodes/IVariantRegistry.cpp`, `Tests/NodeConvert_Test.cpp`
  - approach: An engine operation that creates the target node, copies knobs through the registry's knob map (by name otherwise), rewires inputs and outputs, keeps the name and position, and deletes the original, as one undoable command.
  - verify: gtest converts a Blur with animated size to IBlur and back: values, animation, connections and name survive; undo restores the original.
  - size: L

- [ ] M189.P1.T2 — Wrap a selection in a Map-blend loop
  - files: `Engine/NodeConvert.cpp`, `Tests/NodeConvert_Test.cpp`
  - approach: Insert LoopBegin before the selection's single entry and LoopEnd after its single output, set Map blend and 8 iterations, as one undoable command; refuse multi-output selections.
  - verify: gtest wraps a Blur→Grade chain; the graph is LoopBegin→Blur→Grade→LoopEnd with external links intact; undo restores it.
  - size: M

- [ ] M189.P1.T3 — Add the GUI actions
  - files: `Gui/NodeGraph.cpp` (context menu), `Gui/NodeGraph20.cpp` or the Edit-menu builder, `Tests/gui/m188_make_inode.py`
  - approach: Make I-Node / Make Base Node entries, enabled per the rules above; after wrapping, select LoopEnd and open its panel at the empty driver table; the matrix/Defocus warning as a non-modal message.
  - verify: the Xvfb GUI test converts a Blur, wraps a Grade, checks the warning on a Transform, and undoes each; screenshots captured.
  - size: M

## Decisions

- 2026-10-25 — **Wrapping drives nothing by default (user):** the user picks driven knobs in LoopEnd's driver table.

**Verification gate:** the full build and ctest pass; the Xvfb GUI test passes; the GUI bench rule applies (`Gui/NodeGraph*`); screenshots of each action shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
