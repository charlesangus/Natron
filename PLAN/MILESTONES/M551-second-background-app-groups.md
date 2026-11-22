# M551 - Second Background App Groups

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Found by M512 - Python importTemplate Binding (2026-11-22). Creating a Group in any background app after the first crashes, in release builds too:
- `AppInstance::getAppIDString()` (`Engine/AppInstance.cpp:2805`) returns `"app"` for every background instance, and each new instance re-runs `app = app1`. So the second app's nodes are declared on the first app (`app.<name> = app.getNode(...)` gives `None`).
- The Group's Output child then fails `declarePythonFields()`, `createNode(Output)` returns null, and `AppInstance::createNodeInternal` (~line 1690) calls `output->setLabel("Output")` before its `assert(output)`, so it dereferences null.

Acceptance sketch:
- No null dereference: a failed initial-node creation is an error the caller sees, not a crash.
- A second background app's nodes are reachable from Python under that app's own variable (`app2` …), with expressions and node declarations resolving in the right app. Decide with the user if this changes naming visible to scripts.
- gtest: create a Group in a second background app and reach its Output from Python.
