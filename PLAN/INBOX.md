## Pending

### 2026-10-24T19:40:00-04:00 — info
- refs: none

Python bindings that return `StatusEnum` and can set a Python error from C++ (e.g. via `checkIsEditable`/`checkHolderIsEditable` on a placeholder node for an uninstalled plug-in), such as `ParametricParam.addControlPoint`/`setNthControlPoint` and other param setters, surface the error as `SystemError: ... returned a result with an exception set` instead of the intended exception, because the generated wrapper keeps converting the return value. M96 fixed this for its two new weighted calls with typesystem inject-code (`if (PyErr_Occurred()) return {};`). A sweep over the other `StatusEnum` methods that can raise is a follow-up, not owned by any milestone yet.
