# M197 - Python Undo Step API

M114 - Global Undo Correctness Phase 114.5 records Python edits through an Engine-side `UndoRecorderI` with `UndoStepGuard` and `UndoSuppressGuard`, but exposes no Python API. This milestone adds two context managers on `app`: `with app.newUndoStep("label"):` groups everything inside into one undo step, and `with app.noUndoStep():` runs edits without recording them. It also makes edits from PySide widget handlers in Python panels recordable, which Phase 114.5 leaves out because they have no C++ entry point.

Source: user request via the plan inbox (2026-10-24).

## Scope

**In:**
- `App.newUndoStep(label)` and `App.noUndoStep()` returning context managers; both are `app` methods only (user, 2026-10-25).
- Nesting: an inner `newUndoStep` folds into the outer step (one entry, the outer label); `noUndoStep` inside a step suppresses recording for its block only; `newUndoStep` inside `noUndoStep` records nothing.
- Edits made inside a `with app.newUndoStep(...)` block in a PySide widget handler of a Python panel are recorded as that one step, through the Python API hooks Phase 114.5 added.
- Exceptions: the step closes on exit, keeps the edits made before the exception, and the exception propagates (user, 2026-10-25).
- With no recorder (background or command line) or off the main thread: both managers enter and exit as silent no-ops (user, 2026-10-25).
- Reference docs for both methods.

**Out:**
- A module-level alias in `NatronEngine`: not wanted (user, 2026-10-25).
- Rollback of a failed block.
- Recording raw Qt widget state or anything not made through the Natron Python API; roto/tracker item structure stays unrecorded, as M114 - Global Undo Correctness decided.
- Changing what Phase 114.5 already brackets (Script Editor, menu commands, button callbacks): those keep their own steps; a user `with` inside them nests into them.

**Edge cases:**
- An empty step is dropped (the history's existing rule).
- `__exit__` runs on the thread that entered; entering on a non-main thread is a no-op, so a mismatched exit never closes another thread's step.
- A `with` left open by an exception in the caller still closes through `__exit__`; a generator-based manager abandoned without exit is not supported.
- Compound-entry replay skips Python callbacks (Phase 114.5 rule); steps opened by user code during replay are no-ops.

**Assumptions:**
- Phase 114.5 landed `UndoRecorderI` (`openStep`/`closeStep`, suppression depth) and the Python API recording hooks as designed; the freshness check at promotion confirms the actual names.

## Phase 197.1: Bindings

- [ ] M197.P1.T1 — Context-manager types and `App` methods
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/typesystem_engine.xml`
  - approach: Add two small wrapper classes (`UndoStepScope`, `NoUndoScope`) holding the label and an enter/exit flag, with `__enter__`/`__exit__` registered in the typesystem (inject-code or `add-function`, whichever the file already uses for Python protocol methods). `__enter__` opens a step on the app's recorder via `getUndoRecorder()` (null, off the main thread, or background means a no-op); `__exit__` closes it exactly once and returns false so exceptions propagate. `noUndoStep` takes an `UndoSuppressGuard` equivalent the same way. `App.newUndoStep(label)` and `App.noUndoStep()` return the scopes.
  - verify: Builds; shiboken regenerates without warnings; T2 covers behaviour.
  - size: M

- [ ] M197.P1.T2 — Engine and GUI tests for the managers
  - files: `Tests/UndoHistory_Test.cpp` or a new `Tests/PythonUndoStep_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Drive the Python API from a GUI-app fixture with a recorder installed: a `with` over three knob sets is one history entry; nesting yields one entry; `noUndoStep` records nothing and `newUndoStep` inside it records nothing; an exception inside the block still leaves one entry with the edits and re-raises; an empty block leaves no entry; with no recorder installed the same script runs and records nothing. Use existing Phase 114.5 test fixtures; read them before writing.
  - verify: New cases pass in ctest.
  - size: M

## Phase 197.2: Panels and docs

- [ ] M197.P2.T1 — Verify and document PySide-handler recording
  - files: `Gui/PythonPanels.cpp` (only if a gap shows), `Tests/python/` (new script test), `Documentation/source/PythonReference/NatronEngine/App.rst`
  - approach: Add a Python-panel script test whose widget handler wraps knob edits in `app.newUndoStep(...)`, invoked as a signal handler, and assert one entry. Fix any gap in how handler-invoked Python reaches the recorder. Document both methods with an example, the no-op behaviour without a recorder, and the exception rule.
  - verify: The script test passes under the Xvfb GUI runner (see the Xvfb recipe in project memory) with the entry appearing in the Undo History panel and undoing in one step; the docs build.
  - size: M

**Verification gate:** ctest green including the new cases; the Xvfb GUI script test shows a handler-driven `with app.newUndoStep("X")` as a single undo entry that undoes and redoes in one step; the same script run with `NatronRenderer` completes without error and records nothing.

## Decisions

- 2026-10-25 — **Names and location: `app.newUndoStep(label)` / `app.noUndoStep()`, `App` methods only** (user). `app` is already in every script and panel scope; no module-level alias.
- 2026-10-25 — **Exceptions close the step, keep the partial edits, and re-raise** (user). No rollback machinery; the user can undo the step.
- 2026-10-25 — **No recorder, background or off-main-thread: silent no-ops** (user), so one script runs everywhere.
- 2026-10-25 — **Widget handlers use the same mechanism** (user): a step opened by the manager captures Python API edits made in the handler; no new C++ entry point.
