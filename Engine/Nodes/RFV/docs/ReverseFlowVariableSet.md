# ReverseFlowVariableSet

Tolerance: none recorded.

## Sharing variables with the project

### Add to project variables

Right-click a variable on a Set node and choose "Add to project variables" to copy it into the project's variables. The copy keeps the variable's type, dimensions, value, animation, label and hint. The whole action is one undo step.

- Both copies stay active and editable. They are not linked: changing one does not change the other.
- The node's copy keeps overriding the project's value upstream of that Set node.
- The menu item is disabled, with a tooltip giving the reason, when the project already declares a variable with that name.

### Global Variables tab

The Set node's "Global Variables" tab has an "Existing variable" drop-down. It lists the variables declared on the project or on other Set nodes, project first. Names already on this node are left out.

Choosing an entry adds a variable to this node with the source's type, dimensions, label, hint and defaults, and the source's current value. The new variable has no animation and no link to the source. Adding it is undoable.
