# Promoting a Set-node variable: undoable, both copies active

Decided 2026-10-25 with the user. This amends the promote point in `2026-10-08-rfvswitch-design.md`. That point was the planner's reading of the request and was never put to the user. The user has now confirmed the copy-and-keep design, with two changes.

- **Undoable.** "Add to project variables" is one undo step. Undo removes the project's copy, and redo restores it with the same value and animation. This makes promote an exception to M70 - RFV Declaration And Storage's rule that project-variable creation is not recorded in the undo history. Creating a project variable from Python (`app.createRFV*Param`) is still not recorded.
- **Both copies stay active.** After a promote, neither copy is disabled, made read-only or greyed out, and both stay editable. The project's copy is the base value. The node's copy keeps overriding it upstream of that Set node. The two copies are not linked. Only the menu item itself is disabled afterwards, with a tooltip, because a second promote would clash with the name that now exists in the project.
