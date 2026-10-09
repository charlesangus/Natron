# RFVSwitch and moving variables between nodes and the project

Decided 2026-10-08 with the user, extending `2026-10-08-reverse-flow-variables-design.md`.

- **RFVSwitch** passes through the input whose pattern line first matches an RFV's value. One multi-line string knob holds the patterns, line i for input i; a line starting `re:` is a regex, any other line is a glob; both match the whole value; first match wins. The pattern syntax is my choice (a line prefix, not a per-line type control) to keep the GUI to one knob; change it if a per-line selector is wanted. Non-string values are compared as `str(value)`. No match is an error by default, or the first input by choice.
- Because the switch chooses its input at request time under the path's context, its variable name is declared to the read-set through a node hook, so its context key and cache entries include the variable's value.
- **Set node drop-down** adds an already-declared variable (project or another Set node) as a new, unlinked copy on this node. **Right-click "Add to project variables"** copies a Set-node variable into the project and keeps the node's copy, so the node continues to override the project.
