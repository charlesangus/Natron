# M390 - Python API Reference

Generate the Python API reference from the bindings, and merge in hand-written descriptions kept in the repo. A coverage check fails CI when a bound class or method has no description. Every current gap is filled in this milestone. Reference-style pages for scripting (callbacks, startup scripts, expressions, the node menu, PyPlugs and environment variables) are ported from the old docs where still true.

Context:
- **Bindings:**
  - `Engine/typesystem_engine.xml` generates `NatronEngine`, and `Gui/typesystem_natronGui.xml` generates `NatronGui`. `Shiboken/typesystem_widgets.xml` is also present.
  - The bindings are built with Shiboken6/PySide6 6.8 (`CMakeLists.txt:108-110`). Shiboken6 exposes `__signature__` on bound functions through its signature module. Confirm this on the build before relying on it.
  - Nothing introspects the modules today. `Tests/Native/EffectMetadataPython_Test.cpp:167` is the only test that imports `NatronEngine`.
- **Running a script:** `NatronRenderer` takes `-t`/`--interpreter` and `-b`/`--background` (`Engine/CLArgs.cpp:849,858`). Check which form runs a script file and exits. Whether `NatronGui` imports headless is unverified. If it doesn't, run the GUI binary with `QT_QPA_PLATFORM=offscreen` for that module.
- **Old hand text:** 66 RST pages at `origin/docs:Documentation/source/devel/PythonReference/` (`NatronEngine/*.rst`, `NatronGui/*.rst`), using `.. class::`, `.. method::` and `.. attribute::` directives. They were updated on the docs branch for layers (2026-09-19) and for `nodesMenu()` (2026-10-22).
  - The scripting pages are in `origin/docs:Documentation/source/devel/`: `callbacks`, `startupscripts`, `paramExpressions`, `nodesMenu`, `groups`, `userCommands`, `natronpath`, plus `_environment.rst` at the source root.
  - Treat all of it as evidence, never as requirements (`DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`): every claim is checked against the code before it is kept.
- **Names:** may land before F17 - OpenNucleon Flag Day. The descriptions are keyed by class name without the module prefix, so the module rename only changes generated headings. The rename script rewrites the prose.

## Scope

**In:**
- `tools/docs/gen-python-ref.py`, which:
  - introspects every public class, method, property and enum in both modules;
  - writes `python/api/<Module>/<Class>.md` pages with signatures;
  - merges descriptions from `docs/user/python/descriptions/<Class>.md` (a class intro, then one `## <member>` section per member);
  - has a `--check` mode.
- The coverage check runs as a ctest (user, 2026-11-10). It fails on a bound class or public member without a description, and on a description section naming a member that no longer exists.
- Porting the old RST descriptions, and writing every missing one now (user, 2026-11-10).
- Reference-style scripting pages under `docs/user/content/python/` (user, 2026-11-10): callbacks and their arguments, `init.py`/`initGui.py` and plug-in search paths, parameter expressions, `nodesMenu()`, PyPlugs and groups, user commands, and environment variables. No tutorials.
- The `tools/docs/build.sh` hook.

**Out:**
- Python tutorials and how-to guides (user, 2026-11-10: reference only for 1.0).
- Adding docstrings to the bindings themselves. Descriptions live in `docs/user/python/descriptions/`.
- Serving the pages in the app: M391 - In-App Docs Bundle.

**Edge cases:**
- **Overloads.** List every overload's signature under one member section; one description covers them all.
- **Inherited members** appear on the base class page only, and subclasses link to it. The check does not require a subclass to re-describe them.
- **Private and Shiboken-internal names** (leading `_`, `Shiboken.Object` machinery) are excluded from both the pages and the check.
- **Enums.** One section per enum. Values are listed by the generator; a value needs a description only where the old docs had one or its meaning isn't obvious from its name.
- **Missing `__signature__`.** Fall back to the typesystem XML's function declarations for that member, and say so in the page.

**Assumptions:**
- `NatronGui` either imports in `NatronRenderer` or works with the GUI binary under `QT_QPA_PLATFORM=offscreen`. Whichever works is recorded in `## Decisions` by P1.T1.

## Phase 390.1: Generator And Check

- [ ] M390.P1.T1 — Write the Python reference generator
  - files: `tools/docs/gen-python-ref.py`, `tools/docs/build.sh` (the Python hook)
  - approach:
    - The script runs inside the app's interpreter. It walks `NatronEngine` and `NatronGui`, collects classes, their bases, public methods with `__signature__` (falling back to the typesystem XML), properties and enums. It writes one Markdown page per class under `python/api/<Module>/`, plus module index pages and `.pages` nav, merging the class intro and the `## <member>` sections from `docs/user/python/descriptions/<Class>.md` when they exist.
    - The hook runs it through the binary given to `build.sh --binary`, using the invocation P1.T1 establishes (`-t <script>` or the GUI binary offscreen for `NatronGui`).
    - Record the working invocation in `## Decisions`.
  - verify: `tools/docs/build.sh --binary <fast build>/NatronRenderer --out build/docs/site` passes `--strict`. The `App`, `Effect` and `PyTabWidget` pages list their known methods with signatures. Undescribed members show a visible "No description yet" marker.
  - size: L

- [ ] M390.P1.T2 — Add the coverage check as a ctest
  - files: `tools/docs/gen-python-ref.py` (`--check`), `Tests/CMakeLists.txt` (an `add_test` running the binary with the script)
  - approach: `--check` writes nothing. It prints one line per class or public member with no description, and per description section naming a member that doesn't exist, and exits 1 if there are any. Register it as a ctest named `PythonReferenceCoverage`. Don't turn the test on until P2.T4 has filled the gaps: add it with a CTest `DISABLED` property, and P2.T4 removes the property.
  - verify: Run against a scratch descriptions dir: a missing description and a stale section are each reported and give exit 1, and a complete set gives exit 0. The ctest appears as disabled in `ctest -N` on the remote build.
  - size: M

## Phase 390.2: Descriptions

- [ ] M390.P2.T1 — Port the old RST descriptions
  - files: `tools/docs/rst-to-descriptions.py` (one-off, deleted at the end of the task), `docs/user/python/descriptions/*.md`
  - approach: Convert each `origin/docs:Documentation/source/devel/PythonReference/**/*.rst` page into the descriptions format, mapping its class text to the intro and each `.. method::`/`.. attribute::` to a `## <member>` section. Drop any section whose member is no longer bound. Check each kept description against the binding's C++ source (`Engine/Py*.{h,cpp}`, `Gui/Py*.{h,cpp}`) and fix what is wrong. Delete the converter after use; the descriptions are the source from now on.
  - verify: `gen-python-ref.py --check` reports no stale sections. Spot-check five ported descriptions against the code, and list them in the PR body.
  - size: M

- [ ] M390.P2.T2 — Describe the remaining NatronEngine parameter classes
  - files: `docs/user/python/descriptions/*Param.md` and the `*Tuple.md` files
  - approach: For every parameter class and member `--check` still reports, write the description from the C++ binding source. Say what it does, its argument meanings and units, the return value, and the dimension and view behaviour. Use a two-line example only where the call order is not obvious.
  - verify: `--check` reports no parameter or tuple class.
  - size: M

- [ ] M390.P2.T3 — Describe the remaining NatronEngine core classes
  - files: `docs/user/python/descriptions/` (`App`, `AppSettings`, `Effect`, `Group`, `Roto`, `ItemBase` and the other non-parameter NatronEngine classes `--check` reports)
  - approach: As in P2.T2, from the C++ binding source.
  - verify: `--check` reports no NatronEngine class.
  - size: M

- [ ] M390.P2.T4 — Describe the NatronGui classes and turn the check on
  - files: `docs/user/python/descriptions/` (the NatronGui classes), `Tests/CMakeLists.txt`
  - approach: Write the remaining NatronGui descriptions as in P2.T2. Then remove the `DISABLED` property from `PythonReferenceCoverage`.
  - verify: `--check` exits 0, and the enabled `PythonReferenceCoverage` ctest passes in the remote `fast` build with `--tests`.
  - size: M

## Phase 390.3: Scripting Reference

- [ ] M390.P3.T1 — Write the callbacks, startup scripts and environment pages
  - files: `docs/user/content/python/{index,callbacks,startup-scripts,environment}.md`, `docs/user/content/python/.pages`
  - approach:
    - **Callbacks:** each callback the app runs (parameter changed, input changed, node created and deleted, before and after render, frame render, project load and save, and any others), with its exact keyword arguments, taken from the C++ call sites. Grep for where the callback knobs' scripts are run.
    - **Startup scripts:** the `init.py`/`initGui.py` search order and when each runs (`initGui.py` runs after the first window, per the 2026-10-22 docs change), plus the plug-in path environment variables.
    - **Environment:** every environment variable the app reads, found by grepping `getenv`/`qgetenv`/`QProcessEnvironment` in the first-party code.
    - Port from the old pages only what the code confirms.
  - verify: `build.sh` passes `--strict`. Every callback and environment variable listed is cited to a `file:line` in the PR body. A scratch `init.py` that prints from a node-created callback shows the documented arguments when run with `NatronRenderer`.
  - size: M

- [ ] M390.P3.T2 — Write the expressions, node menu, PyPlugs and user commands pages
  - files: `docs/user/content/python/{expressions,nodes-menu,pyplugs,user-commands}.md`
  - approach:
    - **Expressions:** the variables available in a Python expression (`thisNode`, `thisParam`, `frame`, `dimension`, `view` and so on, taken from the code that sets up the expression scope).
    - **Node menu:** `nodesMenu()` and adding entries.
    - **PyPlugs:** exporting a group as a PyPlug, its file layout, and where it is searched for.
    - **User commands:** `addMenuCommand` and shortcuts.
    - Port from the old pages only what the code confirms.
  - verify: `build.sh` passes `--strict`. Each expression variable is cited to the code in the PR body. A scratch PyPlug exported from the GUI under Xvfb loads from the documented path.
  - size: M

**Verification gate:**
- The remote `fast` build and full ctest pass, with `PythonReferenceCoverage` enabled and green.
- `tools/docs/build.sh --binary …` passes `--strict` with the API and scripting pages.
- The PR body lists the spot-checked ports and the code citations from Phase 390.3.

## Decisions

- 2026-11-10 — The reference is generated from the bindings, with hand descriptions merged in, and a CI check fails on any undescribed or stale member: generation keeps signatures true, and the check keeps the prose from drifting (user).
- 2026-11-10 — Every current gap is filled in this milestone, so the check is strict from merge (user).
- 2026-11-10 — The scripting reference pages are included, written as reference rather than tutorials: users can't write Python for the app without them (user).
- 2026-11-10 — Descriptions live in `docs/user/python/descriptions/` rather than as binding docstrings: no rebuild to fix prose, and the RST port maps onto it directly (planner).
