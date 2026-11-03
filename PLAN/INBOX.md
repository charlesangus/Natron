## Pending

### 2026-11-03T18:00:00Z — bug
- refs: (new stub)
- Expression evaluation leaks into the Python globals: `Knob::setExpression` runs a setup script with `frame=0` and `view=0` (Engine/Knob.cpp:3240) in the same globals as user/Script Editor code, so any user script variable or helper named `frame` or `view` is silently overwritten when an expression is set. Found by M118's GUI check (helper `frame()` became the int 0). Expressions should evaluate in their own namespace. 21 GUI scripts define such helpers. Proposed stub: "Expression Globals Isolation".

