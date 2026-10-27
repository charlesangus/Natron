# RFVSet

Tolerance: none recorded.

## Variables and Global Variables

The Set node's Variables tab has two groups, and every parameter in either one is a variable of the node:

- **Variables** holds the variables only this node declares, meaning the project has no variable of that name.
- **Global Variables** holds the node's variables whose names the project also declares, after the "Existing variable" drop-down.

The grouping follows the project's variables. A node variable moves to Global Variables when the project gains a variable of the same name, by promotion, from the project's settings or from a script. It moves back when the project loses that variable, including when a promotion is undone. When a project loads, each node re-sorts its variables against the project's, whatever grouping was saved.

## Sharing variables with the project

### Add to project variables

Right-click a variable on a Set node and choose "Add to project variables" to copy it into the project's variables. The copy keeps the variable's type, dimensions, value, animation, label and hint. The whole action is one undo step. The node's variable moves to Global Variables.

- Both copies stay active and editable. They are not linked: changing one does not change the other.
- The node's copy keeps overriding the project's value upstream of that Set node.
- The menu item is disabled, with a tooltip giving the reason, when the project already declares a variable with that name.

### Adding an existing variable

The "Existing variable" drop-down at the top of the Global Variables group lists the variables declared on the project or on other Set nodes, project first. Names already on this node are left out.

Choosing an entry adds a variable to this node with the source's type, dimensions, label, hint and defaults, and the source's current value. The new variable has no animation and no link to the source. A project variable's copy goes in Global Variables, and another node's goes in Variables unless the project declares the same name. Adding it is one undo step.

The drop-down is part of the node, not a parameter: it is not saved, cannot be edited or removed from the parameter manager, and is not a variable.

## From Python

A variable starts as an ordinary user parameter. Create it with any of the usual creators, such as `createDoubleParam`, `createInt2DParam`, `createColorParam` or `createStringParam`, and then pass it to `addRFVKnob`:

```python
gain = app.Set1.createDoubleParam("gain", "Gain")
gain.setValue(0.5)
app.Set1.addRFVKnob(gain)

shot = app.createStringParam("shot", "Shot")
app.addRFVKnob(shot)
print(app.shot.getValue())
```

- On a Set node, the parameter moves into the node's Variables group, or into Global Variables when the project already declares the name.
- On the app object, the parameter becomes a project variable and is reachable as `app.<name>`.
- The variable's name is the parameter's script name.
- The User page the creator put the parameter on is removed once it is empty.

`addRFVKnob` raises `ValueError` and moves nothing in any of these cases:

- the parameter belongs to another node or to the project, or the node has no variables group;
- the parameter is not a user parameter;
- it holds no value (a group, page, button or separator);
- it is already a variable;
- its name is a Python keyword or another parameter already has it;
- for a project variable, the name is already an attribute of the app object, such as a method or a top-level node.

A refused parameter stays where its creator put it. A project parameter that is not a variable is not reachable as `app.<name>`.
