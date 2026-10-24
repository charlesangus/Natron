## Pending

### 2026-10-24T19:30 — info
- refs: none
- `Tests/gui/m55_node_graph.py` section `splice-vertical` fails 6 checks on main (seen on `M80-final.AppImage`, main plus M80, and on M134's branch): the hover highlight isn't drawn, the splice connections aren't made, and redo doesn't re-apply them. Found while gating M134 - Output Picker Pixel Aspect, which doesn't touch the node graph. Needs an owner: likely a regression from a node-graph change, or a script assumption that no longer holds.
