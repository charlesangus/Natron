## Pending

### 2026-10-24T23:30:00Z — info
- refs: none
User request: a Python API for undo steps, as context managers — e.g. `with app.newUndoStep("label"):` groups everything inside into one undo step (nesting folds into the outer step), and `with app.noUndoStep():` runs edits without recording them. Builds on M114 Phase 114.5 (Python edits recorded through the Engine-side UndoRecorderI with UndoStepGuard/UndoSuppressGuard), which exposes no Python API. It also covers edits made from PySide widget handlers in Python panels, which 114.5 leaves unrecorded because they have no C++ entry point. Decide names, where they live (app vs module-level), behaviour with no recorder (background/command line: no-ops), and exception handling (the step still closes; whether a failed block is rolled back).
