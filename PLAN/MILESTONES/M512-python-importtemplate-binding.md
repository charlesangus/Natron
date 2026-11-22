# M512 - Python importTemplate Binding

This milestone joins Feature F33 - Graph Diff And Merge and branches off `feature/f33-graph-diff-and-merge`, where `Project::importTemplate` lives (it is not on main yet). M103 - Graph Diff And Merge added `Project::importTemplate(filePath, group, error)` (`Engine/Project.h:198`, `Engine/ProjectTemplateImport.cpp:159`) and deferred the Python binding. Python has no `Project` object: `App` is the project handle (`loadProject` and `saveProject` are in `Engine/PyAppInstance.h:394-415`). So the binding is `app.importTemplate(filename, group=None)`, which returns the created top-level nodes as `Effect`s and raises `RuntimeError` with the loader's message on failure.

## Scope

**In:**
- `App::importTemplate` in `Engine/PyAppInstance.h`/`.cpp`.
- Its `Engine/typesystem_engine.xml` entry with error handling.
- A Python gtest.

**Out:**
- The Import Template… menu, Update from Template and undo for an import (M104 - Merge Resolution UI).
- Any change to `Project::importTemplate`'s behaviour.

**Edge cases:**
- A missing or unreadable file raises `RuntimeError` with the loader's message, and no nodes are created.
- `group=None` imports into the app's root.
- A `Group` that is not in this app raises `ValueError`.
- An import from Python is not an undo step. The docstring says so, and M104 owns the undo.

**Assumptions:**
- The import's provenance (template entry, `origin`) is the same as from C++, because the binding only forwards.

## Phase 512.1: Binding

- [ ] M512.P1.T1 — Wrapper and typesystem entry that raise on failure
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/typesystem_engine.xml`
  - approach: add `std::list<Effect*> importTemplate(const QString& filename, Group* group = 0)`. Resolve `group` to a `NodeCollectionPtr` the way `App::createNode` does, call `Project::importTemplate`, and wrap the results the way `Group::getChildren` does. Mirror that entry's ownership in the typesystem. Shiboken swallows the exception when a define-ownership binding returns null, so add `inject-code class="target" position="beginning"`. It calls an internal variant that returns the error, and on failure does `PyErr_SetString(PyExc_RuntimeError, ...)` and returns 0 early, before any conversion (pattern: `ExprUtils.noise`, typesystem_engine.xml:250-283). Write the docstring in the header, including the undo note.
  - verify: the phase build generates and compiles the wrapper; P1.T2 passes
  - size: M
- [ ] M512.P1.T2 — Python test for importTemplate
  - files: `Tests/Serialization/PyTemplateImport_Test.cpp` (new; harness as in `Tests/PyLinks_Test.cpp:79`, template fixture as in `Tests/Serialization/TemplateImport_Test.cpp:251`)
  - approach: from `interpretPythonScript`: import into the root and get the created `Effect`s; import into a `Group` and check they are its children; import twice and get distinct names and two template entries. A missing path raises `RuntimeError` whose message names the file, and the node count is unchanged. Bundled gtest is old: no `GTEST_SKIP`, `INSTANTIATE_TEST_CASE_P` only, no `ASSERT_TRUE(ptr)`.
  - verify: `ctest -R PyTemplateImport` passes at the phase build
  - size: M

**Verification gate:** `ctest -R "PyTemplateImport|TemplateImport"` green on the F33 branch; full debug PR CI green.
