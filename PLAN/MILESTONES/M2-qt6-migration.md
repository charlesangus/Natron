# Milestone 2: Land the Qt6 migration

> **Resumed 2026-08-31 at `M2.P3.T1c`.** M8 shipped, restoring a merge path.
> This milestone's 54 commits were rebased off `main` onto
> `milestone/m2-qt6-migration`; `.github/workflows/ci.yml` is now M8's version
> verbatim, which strikes `M2.P3.T3` (see `## Decisions`). Remaining work:
> `T1a` → `T1` → `T2`.
>
> <details><summary>Earlier parking notes (historical)</summary>
>
> **Re-parked at `blocked` 2026-08-30, after `M2.P3.T1b`.** M8 (branching model
> and CI/CD rebuild) runs first: `M2.P3.T1b`'s job rename broke `RB-2.6`'s
> branch protection, whose required check is a job display name, so M2 has no
> merge path until that is fixed. M2 resumes at `M2.P3.T1c` on `main`.
> `M2.P3.T3` (strip the `ci(temp)` diagnostics) is superseded by `M8.P2.T2`,
> which rewrites the workflow wholesale — strike it when M8 lands.
>
> **Unblocked 2026-08-30 — M7 shipped; resumed at `M2.P3.T1a`.** The local
> loop is live: `tools/ci/local/build.sh` then
> `tools/ci/local/test.sh smoke --gdb`, no push required. A one-file rebuild is
> 13 s and a build failure surfaces in seconds. See this file's `## Decisions`
> for what M7 already found while validating itself — it is a head start, not
> just tooling.
>
> </details>

`~2-3 weeks` · highest risk. The bulk of the effort, but most of it is already
scoped or already written. Three pieces of this exist as open upstream PRs —
pull them in rather than redoing the work.

Because Qt5 isn't being kept, skip upstream's discipline of gating everything
behind `#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)`. Apply replacements
unconditionally and delete the old branch — less code comes out of this
milestone than went in.

## Phase 2.1: Pull in existing Qt6 work

- [x] M2.P1.T1 — Cherry-pick `lockewerks`'s PRs #1084 & #1085
  - files: whatever the two PRs touch (per upstream diff)
  - approach: *(ready now)* "Replace the Qt6-removed APIs that have Qt5
    equivalents" and "Build against C++20" — both open, mergeable, dated 25
    Aug 2026. Written directly off the maintainer guide's Qt6 audit.
  - verify: both PRs cherry-pick cleanly (or with trivial conflict
    resolution) and the project still builds.
  - size: M

- [x] M2.P1.T2a — Cherry-pick PR #1019's 3 still-relevant commits
  - files: whatever `61e4b7762`, `666ead66b`, `af9d78e22` touch (45 + 6 + 1
    files; see decision below for the full analysis)
  - approach: *(re-scoped after consultant analysis — was "biggest
    unknown")* PR #1019 (`origin/gui-sbk6`, already present locally, no
    fetch needed) actually has 10 commits; 6 already landed upstream in
    `RB-2.6` under different SHAs. Of the 4 remaining: `728ce2a90`
    (QRegExp) is fully superseded by #1084 (M2.P1.T1) — skip it. Cherry-pick
    the other 3 in order: `git cherry-pick -X ours 61e4b7762` (verified:
    resolves clean, all 49 conflicts are "already fixed in HEAD, same or
    better" — mostly duplicate `enterEvent` hunks #1084/upstream already
    landed), then `git cherry-pick 666ead66b af9d78e22` (both verified
    clean, no conflicts).
  - verify: `git cherry-pick` sequence completes; `git grep` confirms the
    wanted hunks survived (e.g. `ViewerGL.cpp`'s `e->position()`
    conversions, `TableModelView.cpp`'s Qt6 fixes) and no unwanted reverts
    of already-landed work.
  - size: M

- [x] M2.P1.T2b — De-ifdef the cherry-picked commit, delete dead compat shim
  - files: the ~45 files touched by `61e4b7762`
  - approach: the cherry-pick imports ~73 `#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)`
    guards — this milestone's own policy (top of this file) is to apply
    replacements unconditionally, not gate them, since Qt5 isn't kept.
    Collapse every guard introduced by this cherry-pick to just its Qt6
    branch. Also delete `Gui/QGLWidgetCompat.h` (a `#error`-on-Qt≥5.4
    compat shim for Qt4-era `QGLWidget`, imported by the cherry-pick,
    referenced by nothing).
  - verify: `git grep -c "QT_VERSION_CHECK(6,0,0)"` returns 0 (or only the
    handful of guards that predate this cherry-pick, if any turn out to be
    load-bearing for a reason worth keeping — investigate any survivors);
    `Gui/QGLWidgetCompat.h` is gone and nothing references it.
  - size: S

- [x] M2.P1.T2c — Gap-fill: `Gui/CustomParamInteract.cpp`'s `QMouseEvent` sites
  - files: `Gui/CustomParamInteract.cpp`
  - approach: PR #1019 fixes `QMouseEvent::x()/y()/localPos()` (removed in
    Qt6, replaced by `position()`) in 11 other files but misses this one —
    6 live sites use the removed API. Same mechanical fix, applied here.
  - verify: `git grep -n "\.x()\|\.y()\|localPos()" Gui/CustomParamInteract.cpp`
    shows no `QMouseEvent`-derived calls to the removed accessors.
  - size: S

- [x] M2.P1.T2d — Fix `libs/qhttpserver`'s Qt6 moc build failure
  - files: `libs/qhttpserver/src/qhttprequest.h`
  - approach: real CI run (triggered manually via `workflow_dispatch` to
    check compile progress ahead of Phase 2.3) surfaced a genuine bug in
    this vendored library, not caught by any prior audit since it's
    third-party code: `Q_PROPERTY(QString method READ method)` declares
    the property type as `QString`, but `method()` returns the
    `HttpMethod` enum — always latently wrong, but Qt5's looser `QChar`
    constructor overloads silently accepted the enum-to-QChar-to-QString
    conversion moc generates for the property setter/getter dispatch,
    where Qt6's stricter (some now-deleted) `QChar` constructors make it
    ambiguous. A correct `const QString methodString() const` getter
    already exists and is unused elsewhere for exactly this purpose (see
    `qhttprequest.cpp`) — fix is `READ method` → `READ methodString`.
  - verify: `moc_qhttprequest.cpp` (regenerated by CMake) compiles; a real
    CI build progresses past `libs/qhttpserver` without this error.
  - size: S

- [x] M2.P1.T2e — Fix missing `QEnterEvent` include in `Global/QtCompat.h`
  - files: `Global/QtCompat.h`
  - approach: real CI (second manual run, after T2d's fix) got past
    `libs/qhttpserver` and into `Engine/AppInstance.cpp`, then failed with
    `'QEnterEvent' does not name a type`. The de-ifdef pass (M2.P1.T2b)
    collapsed `QtCompat.h`'s 3-way Qt5/Qt6 typedef to
    `typedef QEnterEvent QEnterEvent;`, but the header never includes the
    real Qt6 `QEnterEvent` class itself — it happened to compile in Gui
    files that transitively pull it in via other Qt/widget headers, but
    not in Engine files that don't. Add `#include <QEnterEvent>`.
  - verify: `Engine/AppInstance.cpp` compiles past this point in a real CI
    run.
  - size: S

- [x] M2.P1.T2f — Remove the now-pointless `QtCompat::QEnterEvent` alias entirely
  - files: `Global/QtCompat.h`, and the ~45 `Gui/*.cpp`/`Gui/*.h` files using
    `QtCompat::QEnterEvent`
  - approach: T2e's fix was incomplete — real CI's next run failed with
    `QEnterEvent: No such file or directory` in `Engine/AppInstance.cpp`:
    the `Engine` CMake target doesn't have Qt6Gui's include dir wired up,
    so `#include <QEnterEvent>` in the shared `QtCompat.h` (included by
    both Engine and Gui files, for the unrelated `removeFileExtension`
    utility) breaks any Engine file that transitively includes it. The
    `QtCompat::QEnterEvent` alias only ever existed to bridge Qt5's
    `QEvent`-based enter events and Qt6's real `QEnterEvent` class — now
    that Qt5 is gone, the indirection has no purpose. Per this milestone's
    own policy, delete it rather than patch around it: remove the typedef
    and the `#include <QEnterEvent>` from `QtCompat.h`, then replace every
    `QtCompat::QEnterEvent` with plain `QEnterEvent` in the ~45 Gui files
    that use it (all confirmed Gui-only via `git grep`; none are Engine
    files). Each of those files should already transitively include
    `<QEnterEvent>` via other Qt/widget headers it uses (they all handle
    `QMouseEvent`/similar already) — add an explicit
    `#include <QEnterEvent>` to any that don't.
  - verify: `git grep -rn "QtCompat::QEnterEvent"` returns nothing;
    `Engine/AppInstance.cpp` and the Gui files all compile in a real CI
    run.
  - size: M

- [x] M2.P1.T2g — Fix `Engine/Knob.cpp`'s missing `Py_LIMITED_API` undef
  - files: `Engine/Knob.cpp`
  - approach: not a Qt6 issue — a pre-existing, unrelated latent bug that
    real CI's next run exposed simply by compiling further than any
    previous build reached. `Py_LIMITED_API` is defined project-wide
    (compiler flag, likely from Shiboken/PySide6's CMake integration for
    ABI-stable builds), which hides `cpython/pythonrun.h` (and with it the
    `PyRun_String`/`PyRun_SimpleString`/`Py_NoUserSiteDirectory` macros)
    unless a translation unit explicitly `#undef Py_LIMITED_API` before
    `#include <Python.h>`. `Engine/AppInstance.cpp`, `Engine/AppManager.cpp`,
    and `Global/PythonUtils.cpp` already carry this exact workaround
    (`#undef Py_LIMITED_API  // Needed for PyRun_SimpleString, PyRun_String,
    Py_NoUserSiteDirectory`) right before their `#include <Python.h>`.
    `Engine/Knob.cpp` calls `PyRun_String` (in `executeExpression`) but
    never got the same undef — add it, copying the existing pattern
    verbatim for consistency.
  - verify: `Engine/Knob.cpp` compiles in a real CI run; `git grep -c
    "#undef Py_LIMITED_API"` now matches on all 4 files that call these
    Python runtime functions.
  - size: S

- [x] M2.P1.T2h — Fix `Engine/OSGLContext_wayland.cpp`'s X11/Qt include-order conflict
  - files: `Engine/OSGLContext_wayland.cpp`
  - approach: not a Qt6 issue — a pre-existing, never-before-compiled code
    path. `Wayland_FOUND` only started evaluating true once M4's CI began
    installing `wayland-devel` defensively; this file was never actually
    built in any prior CI run. Real CI's first failure here:
    `qtextstream.h must be included before any header file that defines
    Status` — `<EGL/egl.h>` pulls in X11 `Xlib.h`, which `#define`s the
    bare identifier `Status`, and `#include "Engine/AppManager.h"`
    (transitively pulling Qt's `qtextstream.h`) came after it in this
    file. **First attempt** moved the Natron includes (`AppManager.h`,
    `OSGLContext.h`, `GLIncludes.h`) above the EGL/Wayland block — this
    fixed the `Status` clash but broke a different thing: `GLIncludes.h`
    pulls in `glad/glad.h`, which `#define`s `__gl_h_` (the same guard
    real system GL headers use), so moving it before `<EGL/egl.h>` made
    EGL's own internal `<GL/gl.h>`-dependent typedefs
    (`PFNEGLGETDISPLAYPROC` etc.) silently not get declared. **Final fix**:
    revert the Natron-include reorder entirely (restore original
    EGL-before-Natron-includes order) and instead add a single, narrowly
    targeted `#include <QTextStream>` as the very first thing inside the
    `#ifdef __NATRON_WAYLAND__` block, before anything else. **This was
    also insufficient**: real CI's next run showed the identical
    `Status`-pollution symptom cascading into a *different* Qt header,
    `qvariant.h` (reached via a separate include chain off `EGL/egl.h`) —
    pre-including one specific Qt header doesn't prevent `Status` from
    corrupting whichever *other* Qt header gets pulled in next. **Final
    fix**: the standard, robust pattern used throughout the Qt/X11/EGL
    ecosystem for exactly this — `#undef Status` immediately after the
    EGL/Wayland includes, before `Engine/AppManager.h` (or anything else)
    can see the polluted macro. This removes the pollution at its source
    rather than racing to pre-include whichever Qt header happens to need
    protecting.
  - verify: `Engine/OSGLContext_wayland.cpp` compiles in a real CI run.
  - size: S
  - status: attempt 3 (`#undef Status` only) still failed identically —
    turns out X11's `Xlib.h` pollutes several common identifiers as plain
    macros, not just `Status`: the actual break was `#define Bool int`
    colliding with `QVariant`'s deprecated `Type::Bool` enumerator.
    **Attempt 4**: undef the full standard set of X11-macro/Qt-identifier
    collisions (`Bool`, `Status`, `True`, `False`, `None`, `Complex`).
    **Still insufficient**: next real CI run broke on yet another
    collision — X11 also defines `CursorShape` as a macro, breaking
    `QVariant(Qt::CursorShape) = delete;` — confirming the X11 macro list
    is open-ended and chasing it one real-CI-run at a time doesn't
    converge (worse, the resulting parse corruption cascades into
    unrelated-looking errors later in the same file, like the `PFNEGL*PROC`
    "does not name a type" errors reappearing even though the GLAD/EGL
    ordering itself was never touched this time).

    **Final fix (attempt 5)**: stop enumerating X11 macros entirely.
    Move only `#include "Engine/AppManager.h"` (not `OSGLContext.h`, not
    `GLIncludes.h` — those two stay in their original post-EGL position,
    since `OSGLContext.h` is what transitively pulls `GLIncludes.h`'s
    GLAD/`__gl_h_` conflict) to BEFORE the EGL/Wayland include block.
    `AppManager.h` alone pulls enough of QtCore (`QObject`, `QStringList`,
    `QString`, `QProcess`, `QMap` — confirmed to not itself touch
    `GLIncludes.h`/glad) to fully process `qvariant.h`, `qtextstream.h`,
    and everything else Qt declares using an X11-clashing name, before
    X11 ever gets a chance to define any of them. The existing `#undef`
    block stays in place as defense in depth for anything `AppManager.h`
    doesn't happen to reach.

    **This actually worked** — the X11/Qt macro cascade was completely
    gone on the next real CI run. But a *new*, unrelated problem
    surfaced: `PFNEGLGETDISPLAYPROC` and every other core EGL function-
    pointer typedef came back "does not name a type" — likely Qt6's own
    internal EGL platform-integration headers (pulled in transitively by
    `AppManager.h`) pre-define a guard like `EGL_VERSION_1_0` via a
    minimal stub, causing the real `<EGL/egl.h>`'s
    `#ifndef EGL_VERSION_1_0 ... #endif` block (which holds all the
    typedefs) to be skipped when we include it afterward.

    **Ultimately abandoned rather than chased further**: this file
    (`OSGLContext_wayland.cpp`) was never compiled by any prior CI
    configuration — it only started compiling because M4 defensively
    installed `wayland-devel` in the CI container (see M4's decisions),
    not because Wayland support was ever planned, tested, or requested.
    After 11 real CI round-trips on a chain of distinct, increasingly
    obscure bugs in untested code, the user chose to stop installing
    `wayland-devel` in CI instead — restoring `Wayland_FOUND=false` and
    `__NATRON_WAYLAND__` undefined, which makes this whole file compile
    to nothing again, matching every build before M4. All the exploratory
    fix attempts to this file were reverted (`git checkout` to the
    pre-T2h state) rather than left as untested, unverified code. Real
    Wayland desktop support — if ever wanted — is future work requiring
    an actual Wayland test environment, not CI guesswork.

- 2026-08-30 — **Correction**: removing `wayland-devel` from `ci.yml`'s
  `dnf install` line was insufficient — real CI still compiled
  `OSGLContext_wayland.cpp` and failed identically. Root cause: the ASWF
  base image bundles Wayland dev headers in its OS-distro layer
  regardless of what we explicitly install (per `aswf-docker`'s
  `versions.yaml`, `ASWF_WAYLAND_VERSION` is a base-OS package, not part
  of the VFX-platform conan stack we control), so
  `find_package(Wayland COMPONENTS Client Egl)` in the top-level
  `CMakeLists.txt` (line ~118) succeeds unconditionally on this
  container no matter what CI installs. Fixed properly at the source:
  added `option(NATRON_ENABLE_WAYLAND "..." OFF)` and gated the
  `find_package(Wayland ...)` call behind it, so Wayland detection (and
  therefore `OSGLContext_wayland.cpp`) is opt-in and off by default,
  robust to whatever the build environment happens to have installed.
  The `ci.yml` `dnf install` change from the previous decision is
  harmless but unnecessary now — left as-is rather than reverting for no
  functional reason.

- [x] M2.P1.T2i — Fix `NatronRenderer`'s FreeType/HarfBuzz link failure
  - files: `.github/workflows/ci.yml`, `Engine/CMakeLists.txt`
  - approach: not a Qt6 issue — real CI's next run got past compiling the
    *entire* codebase (a first!) and failed at link time instead:
    `libharfbuzz.so.0: undefined reference to 'FT_Get_Color_Glyph_Paint'`
    and four sibling COLRv1 FreeType symbols. Root cause:
    `pkg_check_modules(Cairo REQUIRED IMPORTED_TARGET cairo fontconfig)`
    (Engine/CMakeLists.txt:22) combines cairo+fontconfig into one
    `PkgConfig::Cairo` target, but neither `.pc` file's `Requires:` (as
    opposed to `Requires.private:`, which pkg-config only expands for
    static linking) surfaces `freetype2`/`harfbuzz` for a normal dynamic
    `--libs` query — so nothing on `NatronRenderer`'s link line explicitly
    pulls in a `libfreetype.so` with COLRv1 symbols, and this toolchain
    validates *all* transitive `.so` symbols strictly at link time (unlike
    the old Ubuntu-based CI, which apparently didn't). Fix: add
    `freetype2` explicitly to the `pkg_check_modules(Cairo ...)` call so
    its libs are included on the link line.
  - verify: `NatronRenderer` links successfully in a real CI run.
  - size: S
  - status: first attempt (adding `freetype2` to the `pkg_check_modules(Cairo
    ...)` call) did not fix it — identical error on the next real CI run.
    Likely pkg-config path/ordering subtlety (freetype2.pc resolving to a
    different libfreetype than the one satisfying harfbuzz, or link-order
    placement). **Revised approach**: stop relying on pkg-config's
    `Requires:`/`Requires.private:` semantics for this at all — add
    `find_package(Freetype REQUIRED)` (CMake's own, more reliable
    `FindFreetype` module) and link `Freetype::Freetype` explicitly as a
    PUBLIC dependency of `NatronEngine`, alongside `PkgConfig::Cairo`, so
    every consumer (`NatronRenderer`, `NatronGui`, `Tests`) gets it via the
    same propagation mechanism that already works correctly for
    Boost/Cairo. **Second revised approach**: the package-provided CMake
    configs are under `/usr/local`, so configure CI with
    `-DCMAKE_PREFIX_PATH=/usr/local`; require HarfBuzz's config package
    explicitly (`find_package(harfbuzz 14 CONFIG REQUIRED)`) and link
    `harfbuzz::harfbuzz` alongside `Freetype::Freetype` on `NatronEngine`.
    This makes both libraries direct, propagated dependencies of every
    consumer. **Confirmed fixed**: real CI run 33317644370 (head
    `8ec23929`) built and linked `NatronRenderer` successfully — both debug
    and release configurations. The run's only failure is in "Check for
    test failures" (ctest), unrelated to linking and out of M2's scope
    (M5: Test & correctness baseline).

## Phase 2.2: Mechanical Qt6 API replacements

- [x] M2.P2.T1 — Fix the ~33 `QRegExp` sites (16 files)
  - files: `ScriptTextEdit.cpp`, `Project.cpp`, `CLArgs.cpp`,
    `NodeCreationDialog.cpp`, and the remaining sites inventoried in
    `Documentation/source/maintainers/qt6-migration.rst`
  - approach: verified `QRegularExpression` replacements are already written
    and compile-tested in that doc — this is application work, not
    investigation.
  - verify: `git grep -c QRegExp` across the tree returns 0; project builds.
  - size: M

- [x] M2.P2.T2 — Fix `QDesktopWidget` (6 files) and the one `QVariant::Type` site
  - files: the 6 `QDesktopWidget` sites and the single `QVariant::Type` site,
    per `Documentation/source/maintainers/qt6-migration.rst`
  - approach: replace with `QScreen`/`QGuiApplication::screens()` and
    `QMetaType` respectively — also pre-audited in the same doc.
  - verify: `git grep -c QDesktopWidget` and `git grep -c 'QVariant::Type'`
    across the tree both return 0; project builds.
  - size: S

- [x] M2.P2.T3 — Replace Qt6-removed `QFontMetrics::width()`
  - files: `Gui/TabWidget.cpp`
  - approach: CI run 33316759167 reached `Gui/TabWidget.cpp` and found one
    missed Qt6 removal: replace the `QFontMetrics::width()` call with the
    equivalent `horizontalAdvance()` API, preserving the existing sizing
    calculation.
  - verify: `git grep -n 'QFontMetrics.*width\|\.width(' Gui/TabWidget.cpp`
    contains no use of the removed `QFontMetrics::width`; the CI debug build
    compiles `NatronGui` past `TabWidget.cpp`.
  - size: S

## Phase 2.3: Bindings and validation

- [x] M2.P3.T1 — Regenerate PySide6/Shiboken6 bindings (compile-verified; see M2.P3.T1a)
  - files: `Engine/typesystem_engine.xml`, `Gui/typesystem_natronGui.xml`
    (bindings themselves are generated at build time into
    `${CMAKE_CURRENT_BINARY_DIR}/Qt6` by Shiboken — do not hand-edit
    generated output; fix the typesystem XML / underlying C++ instead)
  - approach: fix the enum/`QFlags` class of runtime bugs (upstream issue
    #854 — flags passed as raw `int` no longer implicitly convert in
    Qt6/PySide6). This is where most *runtime*, as opposed to compile-time,
    surprises will show up. Confirmed still current (§5a spot-check,
    2026-08-30): `Engine/typesystem_engine.xml:793-796` shows
    `render(...)`'s argument-1 `<replace-type modified-type="PyList"/>`
    still commented out with the note that Shiboken infers the conversion
    itself — verify that actually holds under a real build/run rather than
    assuming it's finished.
  - verify: `cmake --build` regenerates and builds the bindings clean; a
    Python console session exercising enum/flags-taking APIs (e.g. node
    property setters) runs without conversion errors.
  - size: L

- [x] M2.P3.T1b — Declare the Python version the CY2027 platform actually ships
  - files: `.github/workflows/ci.yml`, `tools/ci/local/devshell.sh`
  - approach: *(run before `M2.P3.T1a`)* `ci.yml` sets
    `PYTHON_VERSION: '3.10'` and names the job "Test Ubuntu Python 3.10", but
    `aswf/ci-baseqt:2027.0` ships **Python 3.13.14** at `/usr/local` — the
    variable is vestigial from the pre-ASWF CI and nothing consumes it
    (`CMakeLists.txt:60` uses `find_package(Python3 ...)`, which finds 3.13
    regardless). Set it to `3.13` to match the image, rename the job so it
    stops advertising a version it does not run, and update the mirrored
    `-e PYTHON_VERSION=3.10` in `devshell.sh` so local and CI keep agreeing.
    Confirm the image's version from the image itself rather than from the
    VFX Reference Platform document — the image is what we build against, and
    `PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md` records CY2027 as the
    target precisely because that is what the ASWF image line follows.
    This task is **declaration only**; making the code work under 3.13 is
    `M2.P3.T1c`. Keeping them separate means the rename cannot be blamed for a
    behavioural change.
  - verify: `grep -rn PYTHON_VERSION .github/workflows/ci.yml
    tools/ci/local/devshell.sh` shows `3.13` in both; `tools/ci/local/devshell.sh
    bash -lc 'echo $PYTHON_VERSION; python3 --version'` shows the two agreeing
    after a `--recreate`; no build or test behaviour changes.
  - size: S

- [x] M2.P3.T1c — Port embedded-Python startup off the deprecated 3.11 setters
  - files: `Global/PythonUtils.cpp`, `Global/PythonUtils.h`,
    `Engine/AppManager.cpp`
  - approach: *(run before `M2.P3.T1a`; this is the leading suspect for its
    failure)* `Global/PythonUtils.cpp` configures the embedded interpreter with
    `Py_SetPythonHome` (line ~153) and `Py_SetProgramName` (line ~246) before
    `Py_InitializeEx`. Both were deprecated in 3.11 and are slated for removal
    in 3.15; they still compile against 3.13 but are the wrong API now. More
    to the point, the surrounding logic **clears `pythonHome` when the expected
    `lib/python*` layout is not found** (see the `not setting PYTHONHOME`
    branch around line 133) and then never sets it at all — which is the
    classic cause of the `Fatal Python error: Failed to import encodings
    module` seen in this milestone's `## Decisions`. Port initialization to the
    `PyConfig` API (`PyConfig_InitPythonConfig` / `PyConfig_SetString` /
    `Py_InitializeFromConfig` / `PyConfig_Clear`), which is the supported way
    to set `home`, `program_name` and `module_search_paths` from 3.8 onward,
    and let it fall back to the interpreter's own defaults when Natron has no
    bundled Python to point at — in the ASWF image the system interpreter at
    `/usr/local` is the correct one, so overriding it with a nonexistent path
    is worse than not overriding it.
    Preserve the existing behaviour for a bundled/relocatable Natron install,
    which is why the overrides exist; do not simply delete them. Check whether
    `PY_VERSION_HEX` guards are warranted — this fork targets one Python, so
    prefer an unconditional port over version-guarded branches, consistent with
    how this milestone drops `#if QT_VERSION` guards.
  - verify: `tools/ci/local/build.sh` compiles with no deprecation warnings for
    `Py_SetPythonHome`/`Py_SetProgramName`; `tools/ci/local/test.sh smoke`
    no longer reports `Failed to import encodings module` across **at least
    five consecutive runs** (the failure is non-deterministic, so a single pass
    proves nothing); `tools/ci/local/test.sh ctest` is no worse than before.
  - size: L

- [x] M2.P3.T1e — Guard the unchecked `getpwuid()` deref in `Engine/Project.cpp`
  - files: `Engine/Project.cpp`
  - approach: *(run before `M2.P3.T1a`; it currently crashes every local smoke
    run)* `Natron::getUserName()` at line ~109 does
    `return passwd->pw_name;` on the result of `getpwuid(getuid())` without a
    NULL check, and `generateGUIUserName()` (line ~116) reaches it from
    `Project::initializeKnobs()` (line ~1072). `getpwuid` returns NULL for any
    uid with no passwd entry — which is exactly the dev container, running as
    uid 1000 (the same cause as the `id: cannot find name for user ID 1000`
    noise `tools/ci/local/README.md` already documents). CI runs as root and
    has an entry, which is why this never showed up there. Fall back to
    `$USER`/`$LOGNAME`, then to the uid rendered as a string; do not invent a
    name that could collide with a real one. Pre-existing bug, not a Qt6
    regression — found while verifying `M2.P3.T1c`.
  - verify: `tools/ci/local/test.sh smoke` reaches `smoke_test.py` instead of
    segfaulting in `initializeKnobs`; `tools/ci/local/test.sh ctest` runs the
    `Tests` binary to completion rather than dying as `SEGFAULT`.
  - size: S

- [x] ~~M2.P3.T1d — Install `qtpy` so the smoke test can actually run~~
  - **Superseded by `M2.P3.T1g` (2026-08-31).** Landed as `91b5ab304`, then
    reversed: installing qtpy meant adding a `pip install` the ASWF container
    does not carry, which contradicts the standing requirement that the build
    need only that container. The right fix was to remove the dependency, not
    to satisfy it. See `PLAN/DECISIONS/2026-08-31-drop-qtpy.md`. Original
    brief below, for the record.
  - files: `tools/ci/local/fetch-assets.sh` (or whichever shared script is
    the right seam — see below), possibly `tools/ci/local/Dockerfile`
  - approach: *(blocks `M2.P3.T1a`)* `tools/ci/smoke_test.py`'s first
    assertion is `import qtpy; qtpy.API_NAME == 'PySide6'`, but nothing
    installs `qtpy` — `grep -i "qtpy\|pip install" .github/workflows/ci.yml`
    returns nothing and `aswf/ci-baseqt:2027.0` does not ship it. The smoke
    test therefore cannot pass anywhere today, in CI or locally, which means
    `M2.P3.T1a` could never have been green. Install `qtpy>=2.0` (the floor
    for `QT_API=pyside6`) in the same layer CI and the local loop share, so
    the two keep agreeing per
    `PLAN/DECISIONS/2026-08-30-ci-reuses-local-scripts.md`.

    **Corrected premise (2026-08-31, PM):** an earlier draft of this brief
    assumed `tools/ci/local/Dockerfile` was the layer CI and the local loop
    share. It is not. CI runs `container: image: aswf/ci-baseqt:2027.0`
    **directly** (`ci.yml:20-21`); the Dockerfile builds `natron-dev:2027.0`
    for the local loop only and currently adds no layers. Installing `qtpy`
    there would fix the local loop and leave CI broken. The genuinely shared
    layer is `tools/ci/local/*.sh`, which is the whole point of
    `2026-08-30-ci-reuses-local-scripts.md` — CI invokes `fetch-assets.sh`,
    `build.sh` and `test.sh`. Put the install in a shared script so one change
    serves both, and keep it idempotent the way `fetch-assets.sh` already is.

    **Network:** PyPI *is* reachable, despite
    `PLAN/DECISIONS/2026-08-30-sealed-package-network.md` — that decision is
    about **distro** repos. Verified 2026-08-31 from inside the container:
    `pip3 download qtpy` fetched `QtPy-2.4.3-py3-none-any.whl` successfully.
    So `pip3 install` is a viable mechanism here; no vendoring needed.
  - verify: `tools/ci/local/devshell.sh bash -lc 'python3 -c "import qtpy;
    print(qtpy.API_NAME, qtpy.__version__)"'` prints `PySide6` and a version
    >= 2.0; `tools/ci/local/test.sh smoke` gets past the `import qtpy` line;
    and the mechanism is demonstrably one that a real CI run would also get,
    not just the local container.
  - size: M

- [x] M2.P3.T1f — Make the local smoke run survive to completion
  - files: likely `tools/ci/local/test.sh` and/or a Natron cache setting;
    diagnose before choosing
  - approach: *(blocks `M2.P3.T1a`)* `tools/ci/local/test.sh smoke` is killed
    by the OOM killer on roughly two runs in three — measured 2026-08-31 by
    the PM: 1 of 3 reached the smoke script, and 2 died as
    `xvfb-run: line 181: NNN Killed`. This is **not** ordinary host memory
    pressure: at the time of the kills `free -m` showed ~10.8 GB *available*
    of 15.7 GB total and the container has **no** memory limit
    (`docker inspect` → `Memory=0`, cgroup `memory.max` = `max`). Swap, by
    contrast, was pinned at 4094/4095 MB. The shape — plenty of free RAM,
    exhausted swap, sudden kill — points at a single very large allocation
    rather than gradual growth, and every run also prints
    `Error while loading OpenGL: std::bad_alloc`. Natron sizes its caches
    from total system memory, so the leading hypothesis is that the renderer
    requests a cache proportional to RAM and the kernel's overcommit
    heuristic refuses or the OOM killer reaps it. Diagnose it properly
    (measure the peak RSS and the failing allocation) before choosing a fix;
    a plausible fix is bounding the cache explicitly for the test harness,
    but do not assume it.
  - approach note: this is why it must precede `M2.P3.T1a`. T1a exists to
    make the smoke test trustworthy evidence about the bindings, and it
    cannot do that while two runs in three die for an unrelated reason —
    the same "no single run was trustworthy" trap M7 recorded, in a new
    guise. Do not paper over it with retries.
  - verify: 10 consecutive `tools/ci/local/test.sh smoke` runs all reach
    `smoke_test.py` (they may still *fail* inside it — that is T1a's scope);
    none is killed. Report the peak RSS observed.
  - size: M

- [x] M2.P3.T1g — Drop `qtpy`; import PySide6 directly
  - files: `Engine/AppManager.cpp`, `tools/ci/smoke_test.py`,
    `INSTALL_LINUX.md`, and the `M2.P3.T1d` plumbing in
    `tools/ci/local/fetch-assets.sh`, `tools/ci/local/devshell.sh`,
    `tools/ci/local/Dockerfile`, `.github/workflows/ci.yml`
  - approach: *(supersedes `M2.P3.T1d`; see
    `PLAN/DECISIONS/2026-08-31-drop-qtpy.md` for the rationale and the
    accepted user-facing cost)* qtpy is an abstraction over PyQt4/5/PySide/
    PySide2 adopted upstream (#687) for cross-Qt portability this fork does
    not want — it targets one Qt and one Python, and has already deleted Qt5
    outright. It is also absent from `aswf/ci-baseqt:2027.0`, so keeping it
    means a `pip install` in the build, which the container-as-is
    requirement forbids.
    Three parts: (1) replace Natron's own startup imports at
    `AppManager.cpp:1806-1828` — `import qtpy` / `from qtpy import QtCore`
    and the non-background `from qtpy import QtGui` — with the PySide6
    equivalents, keeping the existing soft-failure behaviour (log to stderr
    + `writeToErrorLog_mt_safe`, do not abort) unless there is a reason to
    harden it, and say so if you think there is. (2) remove
    `qputenv("QT_API", "pyside6")` at `AppManager.cpp:2924` with its comment
    block and the back-reference at :2985 — it exists only so qtpy resolves
    to PySide6 and has no other reader; confirm that by grep before removing.
    (3) revert `M2.P3.T1d`'s install plumbing in full (commit `91b5ab304`),
    so no `pip install` runs in CI or the local loop and `QTPY_PIN`
    disappears.
    Then update `tools/ci/smoke_test.py`: `check_qtpy_resolves_to_pyside6()`
    must go, but do not simply delete the coverage — it was the only runtime
    assertion about the binding layer. Replace it with a direct PySide6
    check that would still catch a broken bindings build (e.g. importing
    `PySide6.QtCore` inside the embedded interpreter and exercising an enum
    or `QFlags` value), and update the module docstring, which explains the
    qtpy/`QT_API` relationship at length.
    Also update `INSTALL_LINUX.md` (lines ~23, ~110, ~224). **Leave
    `CHANGELOG.md` alone** — it records what past releases did and is not to
    be rewritten.
  - approach note: verified 2026-08-31 that the replacement needs nothing
    new — the image ships PySide6 6.8.3 with `QtCore`/`QtGui`, plus
    `shiboken6`, at `/usr/local/lib/python3.13/site-packages`.
  - verify: `git grep -in qtpy` returns hits only in `CHANGELOG.md` and
    `PLAN/`; `git grep -n QT_API` returns nothing outside `PLAN/`;
    `tools/ci/local/fetch-assets.sh` does no `pip install` and a **fresh**
    `aswf/ci-baseqt:2027.0` container (no prior qtpy install) runs
    `tools/ci/local/test.sh smoke` reaching `smoke_test.py`; Natron's startup
    no longer prints `Failed to import qtpy.QtCore`.
  - size: M

- [x] M2.P3.T1a — Add a CI Python smoke-test step and use it to verify the enum/QFlags binding fixes
  - files: `.github/workflows/ci.yml`, a new small Python script under (e.g.)
    `tools/ci/smoke_test.py`
  - approach: M2.P3.T1's `PySequence`/`QT_API` fixes are compile-verified only
    — `NatronEngine`/`NatronGui` build as static libs (no importable `.so`),
    and no existing CI step runs any Python through the app. Add a CI step
    that runs the built `NatronRenderer -t <script.py>` (background mode;
    plain `-t` alone drops to an interactive prompt and would hang CI) or
    equivalent, executing a short script that: (1) asserts
    `import qtpy; qtpy.API_NAME == 'PySide6'` (would have caught the T1
    QT_API-ordering bug directly), (2) calls `app.render([(writeNode, 1,
    1)])` (exercises the `PySequence` fix), (3) calls
    `natron.addMenuCommand(g, f, QtCore.Qt.Key.Key_L,
    QtCore.Qt.KeyboardModifier.ShiftModifier)` or another QFlags-taking
    bound API (exercises the one real QFlags case in the bound surface,
    `PyGuiApplication::addMenuCommand`). Confirm the CI image's qtpy is
    >=2.0 (required for `QT_API=pyside6`) before assuming this will pass.
  - **Revised 2026-08-31 after `M2.P3.T1g`.** Two things changed under this
    task. First, the CI *step* no longer needs writing from scratch — M8
    rewrote `ci.yml` to invoke `tools/ci/local/test.sh`, so wiring the smoke
    test in means adding a `test.sh smoke` step alongside the existing
    `test.sh ctest debug`, not hand-rolling an inline block. Second, and more
    important: `check_pyside6_bindings()` is an **environment tripwire, not a
    regression guard**, and must not be counted as binding coverage. It
    cannot fail in this image — `Qt.AlignmentFlag` is an `int` subclass under
    any released PySide6, and a missing PySide6 aborts Natron earlier anyway,
    when `NatronEngine`'s module init calls
    `Shiboken::Module::import("PySide6.QtCore")`. The old qtpy check *was* a
    real guard (it asserted a Natron-controlled behaviour: `QT_API` being set
    before `Py_Initialize()`); that sensitivity is gone and this task now
    owns replacing it.
    The coverage that actually exercises Natron's own bindings is the
    `app.createReader`/`createWriter`/`render` path already in the script —
    those go through `SbkPySide6_QtCoreTypeConverters`. Getting them running
    is the first job here, starting with the current
    `NameError: name 'app' is not defined`: the script's docstring asserts
    Natron pre-declares `app` for directly-executed scripts, and that is not
    holding — establish what invocation actually pre-declares it
    (`NatronRenderer <script>` vs `-t`, background vs interpreter mode; the
    original brief above assumed `-t`, and `test.sh` currently passes the
    script positionally) before changing the script.
    Note the one real QFlags-taking bound API,
    `PyGuiApplication::addMenuCommand(Qt::KeyboardModifiers)`, is GUI-only
    and stays uncovered in a background CI run — say so explicitly rather
    than implying #854 is guarded when it is not.
  - verify: the new CI step runs and passes on a real CI build, and fails
    loudly (non-zero exit) if reverted/broken — confirm by temporarily
    reverting one of the three fixes locally and observing the step catch it,
    then restore. A check that cannot be made to fail does not count.
  - size: M

- [x] M2.P3.T2 — Validate the GUI end-to-end
  - files: none (manual QA pass across the running application)
  - approach: file dialogs, node graph, viewer, curve editor/dope sheet,
    roto/tracker overlays, Python console. There's no automated UI test
    suite, so this is a manual pass — mirror the checklist upstream used for
    the Qt4→Qt5 migration (referenced in issue #827).
  - verify: each area on the checklist opens and operates without visual
    corruption or crashes on Qt6.
  - size: M

- [x] ~~M2.P3.T3 — Strip the `ci(temp)` diagnostics from the CI workflow~~
  - **Superseded by `M8.P2.T2` and satisfied by the 2026-08-31 rebase.** M8
    rewrote `ci.yml` wholesale; the rebase took M8's version verbatim at every
    conflict, so the three surviving `ci(temp)` commits went empty and were
    dropped. Verified: `grep -c "M2.P3.T1a diagnostic" .github/workflows/ci.yml`
    and `grep -c "::notice::"` both return 0. Original brief below, for the record.
  - files: `.github/workflows/ci.yml`
  - approach: the temporary annotation-encoding block in the Python
    smoke-test step, the `::notice::` outcome-reporting step, and the other
    scaffolding commented as `M2.P3.T1a diagnostic` exist only because CI
    logs were unreadable and iteration was slow. Once M7's local loop
    reproduces the failure, they are dead weight — remove them and restore
    the workflow to its clean design. Leave the `continue-on-error`
    aggregation pattern intact; that is deliberate, not diagnostic.
  - verify: `grep -c "M2.P3.T1a diagnostic" .github/workflows/ci.yml` returns
    0; the workflow still fails the job when any of the three test steps fail.
  - size: S

**Verification gate:** the ctest suite (M5) passes, the Python bindings load
and operate without enum/QFlags errors, the manual GUI checklist above passes
end-to-end on Qt6.8.x, and `.github/workflows/ci.yml` carries no temporary
diagnostics while still gating on test failure.

## Decisions

- 2026-08-30 — park M2 for M7 (local incremental builds): the CI
  round-trip made `M2.P3.T1a` uneconomic to debug — five consecutive
  `ci(temp)` commits existed only to surface a stack trace. Building the
  local loop first is cheaper than continuing to pay that cost for every
  remaining task in this milestone.

- 2026-08-30 — T2i follow-up: making HarfBuzz and FreeType public
  dependencies was not enough to control final link order. On Linux/CMake
  3.24+, promote the ordered `harfbuzz::harfbuzz`, `Freetype::Freetype`
  pair to direct interface dependencies of `NatronEngine`, so the ASWF
  `/usr/local` FreeType wins before an indirect system library with the
  same SONAME.

- 2026-08-30 — T2i follow-up: the ASWF HarfBuzz config exports its target
  but provides no usable version metadata, so `find_package(harfbuzz 14
  CONFIG REQUIRED)` stopped CI configuration before binding generation.
  Retain `CONFIG REQUIRED` but remove the version constraint; the explicit
  public `harfbuzz::harfbuzz` and `Freetype::Freetype` links remain intact.

- 2026-08-30 — T2i's second link-failure attempt makes HarfBuzz explicit as
  well as FreeType and sets CI's CMake prefix to `/usr/local`, where the
  image exposes their package configs. This removes reliance on pkg-config
  transitive dependency expansion and CMake's default package-search path.

- 2026-08-30 — Cherry-picked PR #1084's 4 commits cleanly (one trivial
  line-shift auto-merge in `Engine/CLArgs.cpp`, verified byte-identical to
  upstream's actual code change). This single PR's QRegExp→
  QRegularExpression, QDesktopWidget→QScreen, and QVariant::Type→userType()
  work turned out to BE Phase 2.2's entire scope — checked off M2.P2.T1 and
  M2.P2.T2 as well rather than re-doing already-landed work. Remaining
  `QDesktopWidget` grep hits (5) are all comments/dead code, not live
  usage.
- 2026-08-30 — PR #1085 ("Build against C++20") is fully redundant here:
  its only content is `CMAKE_CXX_STANDARD` (already 20, from M1) and
  `global.pri` edits (that file no longer exists, qmake removed in M0).
  Not cherry-picked; nothing to take.
- 2026-08-30 — M2.P3.T1 (bindings): fixed the enum/QFlags class of #854 bugs.
  `App.render`/`GuiApp.renderBlocking` argument 1's `<replace-type>` was
  commented out on the (wrong) claim that Shiboken infers the list-of-tuples
  conversion itself; restored as `modified-type="PySequence"` (not the
  original `PyList`, which isn't a Shiboken6 built-in type — that's why it
  failed to compile). Precedent: `PathParam::setTable`
  (`Engine/typesystem_engine.xml:1434`) uses the identical pattern. Also
  fixed `QT_API` resolution: `Engine/AppManager.cpp` set
  `qputenv("QT_API", "pyside2")` (now `"pyside6"`) *after* `Py_Initialize()`
  — CPython snapshots the environment into `os.environ` at interpreter
  startup, so the env var was invisible to qtpy and the whole fix was a
  silent no-op. Independent review caught this and moved the `qputenv` call
  to before `initializePython3()` (`Engine/AppManager.cpp:2910-2923`).
  Both fixes are confirmed to generate and compile clean under real CI (run
  33323834024 vs. baseline 33317644370, byte-identical step outcomes aside
  from these changes). Neither is runtime-verified — no CI step exercises
  Python against the bindings today (`NatronEngine`/`NatronGui` are static
  libs, no importable `.so`) — hence new task M2.P3.T1a to close that gap
  before M2.P3.T1 is checked off. Also noted for M6/future packaging work:
  a Qt6 install bundle must ship `qtpy>=2.0` and strip any `PySide2` from
  site-packages — no Qt6 packaging script exists yet
  (`tools/jenkins/build-Linux-installer.sh`'s PySide2 strip is gated on the
  Qt4/`USE_QT5` branch only).

- 2026-08-30 — Consultant analysis of PR #1019 replaced M2.P1.T2 (see
  M2.P1.T2a/b/c above) after finding: (1) 6 of its 10 commits already
  landed upstream under different SHAs — the plan's "biggest unknown /
  20-months-stale" framing was based on GitHub's raw diff count, not
  actual remaining delta; (2) of the 4 remaining commits, 3 apply with
  zero-or-mechanical conflicts (verified in a scratch worktree, not just
  estimated) and 1 (`728ce2a90`, QRegExp) is superseded by #1084;
  (3) `Documentation/source/maintainers/qt6-migration.rst`'s "Concrete
  work items (audited)" list is incomplete — it covers QRegExp,
  QDesktopWidget, setMargin, QVariant::Type, bindings, module includes,
  foreach, but misses a second wave (QMouseEvent::x/y, QMutexLocker
  template, QtConcurrent::run overload, QStyleOption::init,
  QAbstractItemView::viewOptions, QVariant::canConvert, globalStrut,
  QTabletEvent enums, QKeySequence operators, QButtonGroup signal) that
  PR #1019 happens to fix already (folded into M2.P1.T2a) except for
  `CustomParamInteract.cpp` (M2.P1.T2c). M6's existing
  `qt6-migration.rst` doc-currency task already covers correcting this
  list once the migration is actually done — no separate task added.

- 2026-08-30 — **handed over from M7, which found four things while validating
  its own tooling against this branch.** Start here rather than re-deriving
  them.
  1. **A CMake generate failure, already fixed** (commit `621cd48c7` on this
     branch). `find_package(harfbuzz CONFIG REQUIRED)` sat in
     `Engine/CMakeLists.txt` while `harfbuzz::harfbuzz` was on `NatronEngine`'s
     PUBLIC link interface; imported targets are directory-scoped, so
     `Renderer`/`Gui`/`Tests`/`App` — siblings of `Engine`, not descendants —
     could not resolve it. Moved to the top-level `CMakeLists.txt`, matching how
     Qt6/Shiboken6/PySide6/X11 are already handled. The `GLOBAL` keyword would
     also work but needs CMake 3.24+, above this project's 3.16.7 minimum. Link
     order and the `INTERFACE_LINK_LIBRARIES_DIRECT` promotion from
     `FREETYPE_HARFBUZZ_FINDINGS.md` are untouched. **With this, the full debug
     build completes** — `NatronRenderer`, `Natron` and `Tests` all link.
  2. **`M2.P3.T1a`'s smoke test fails two different ways, non-deterministically**
     — this is the key finding. Across four `test.sh smoke --gdb` runs, three
     produced `Fatal Python error: Failed to import encodings module` and one
     produced a SIGSEGV. `ctest`'s single test shows the same instability,
     failing sometimes as `SEGFAULT` and sometimes on the `encodings` error.
     The non-determinism is itself important: it means no single CI run — green
     or red — was ever trustworthy evidence, which is part of why chasing this
     through `ci(temp)` commits went nowhere.
  3. **The SIGSEGV has a backtrace now.** `getFBConfigAttrib` at
     `Engine/OSGLContext_x11.cpp:433` dereferences a NULL `fbconfig` handed to
     it by `chooseFBConfig` (line 472), reached via `main` →
     `AppManager::load` → `loadFromArgs` → `initializeOpenGLFunctionsOnce` →
     `GPUContextPool::attachGLContextToRender`. A GLX fbconfig-selection NULL
     deref under Xvfb, adjacent to commit `471f15c65`'s "fix pre-existing GLX
     NULL deref" — quite possibly that fix being incomplete.
  4. **`ci.yml` declares `PYTHON_VERSION: '3.10'` and the job is still named
     "Test Ubuntu Python 3.10", but `aswf/ci-baseqt:2027.0` ships Python
     3.13.14.** The variable looks vestigial from the pre-ASWF CI, but given
     that the dominant failure is an embedded-interpreter `encodings` import
     error, an incorrect assumption about which Python the bindings build
     against is a strong suspect. **Check this before anything else.**

- 2026-08-31 — rebase onto `main` rather than merge: M2's 60 commits lived on
  `ci-smoke-test-m2p3t1a`, a name the trunk-based-development decision
  explicitly cites as what its convention exists to prevent. `main` was only
  one commit ahead (M8's squash). Replayed onto a fresh
  `milestone/m2-qt6-migration` for linear history — no force-push needed since
  the target branch is new, and `ci-smoke-test-m2p3t1a` is kept as a safety
  net. Conflict policy: M8's `ci.yml`, `PLAN.md`, and executed milestone files
  win at every conflict; source files never conflicted. Verified afterwards
  that the source tree is byte-identical to the old branch tip across
  `Engine/ Gui/ Global/ libs/ Shiboken/ Tests/ App/ Renderer/ HostSupport/`
  and `CMakeLists.txt`. Five commits went empty under that policy (two
  board-only, three `ci(temp)`) and were dropped: 54 commits, not 59.

- 2026-08-31 — `tools/ci/local/devshell.sh` kept M2's `PYTHON_VERSION=3.13`
  over M8's `3.10`: M8's own `ci.yml` already declares `3.13`, so main's
  `devshell.sh` was internally inconsistent with it and with the ASWF image
  (Python 3.13.14). Carrying `M2.P3.T1b`'s fix forward is what keeps the local
  loop and CI agreeing, which is the whole point of that task.

- 2026-08-31 — `M2.P3.T1c` root cause was `Py_SetProgramName`, not the version
  macros: `NATRON_PY_VERSION_STRING`/`_NO_DOT` derive from `PY_MAJOR_VERSION`/
  `PY_MINOR_VERSION` and were already correct at 3.13, so M7's "`ci.yml` says
  3.10, image ships 3.13.14" lead (recorded above as "check this before
  anything else") was a dead end — `M2.P3.T1b` had already fixed the only
  thing that was wrong there. The real fault was that `Py_SetProgramName` was
  called *unconditionally*: pointed at a build-tree binary, CPython 3.13
  derives `prefix`/`exec_prefix`/`module_search_paths` from that location,
  finds no `lib/python3.13`, and aborts before importing `encodings`. The fix
  is therefore conditional overriding — set `home`/`program_name` only when a
  bundled layout exists — and not merely an API port. Recorded because the
  dead lead was stated with high confidence and cost real time.

- 2026-08-31 — `config.configure_c_stdio = 0` is load-bearing: caught in
  review, not implementation. `Py_Initialize()` uses CPython's *compat*
  config, where `configure_c_stdio` is 0; `PyConfig_InitPythonConfig()`
  defaults it to 1, which makes the interpreter reconfigure the embedding
  application's own `stdin`/`stdout`/`stderr` (unbuffered under
  `PYTHONUNBUFFERED`, and binary mode on Windows). Natron embeds Python; it
  must not start behaving like the `python` binary. The same class of trap
  applies to `config.parse_argv`, which is 1 under `PyConfig_InitPythonConfig`
  versus 0 under `Py_Initialize()` — harmless only because `config.argv` is
  never set. Anyone populating `config.argv` later must set `parse_argv = 0`.

- 2026-08-31 — the M7 GLX `getFBConfigAttrib` SIGSEGV did not reproduce once
  in ~15 local runs while verifying `M2.P3.T1c`; every run instead reported
  `Error while loading OpenGL: std::bad_alloc` and disabled OpenGL. Either
  the allocation failure now short-circuits the GLX path or the original
  diagnosis was of a different configuration. Whoever picks that bug up must
  re-confirm it still reproduces before working it. Separately, the host was
  under heavy memory pressure during verification (swap 4083/4095 MB, two
  runs OOM-killed) — some of the "non-determinism" M7 attributed to the code
  is likely environmental, which is worth knowing before reading run-to-run
  variation as signal.

- 2026-08-31 — `M2.P3.T1e` gave this milestone its first real test signal.
  Before it, `ctest` died as `SEGFAULT`/"Subprocess killed" and reported
  nothing; after it, the `Tests` binary runs to completion — 28 tests, 25
  passed, 3 failed. The three failures (`BaseTest.GenerateDot`,
  `BaseTest.SetValues`, `BaseTest.SimpleNodeConnections`) are pre-existing and
  are **M5's** scope, not M2's, but they are now visible rather than hidden
  behind a crash. M5 should start from this baseline rather than assuming the
  suite was never runnable.

- 2026-08-31 — `M2.P3.T1d`: `pip install --user` is not usable for anything
  `NatronRenderer` must import. Natron disables user site-packages in two
  places — `qputenv("PYTHONNOUSERSITE", "1")` in `AppManager::initPython()`
  and, since `M2.P3.T1c`, `config.user_site_directory = 0` in the `PyConfig`
  startup path — so a `--user` install satisfies a bare
  `python3 -c "import qtpy"` as either uid while remaining invisible inside
  the renderer. The first implementation of T1d was `--user`, passed that
  spot-check, and still failed the real smoke run. Recording it because the
  wrong version looks correct under every check short of running the actual
  binary: **verify Python-dependency changes by running `NatronRenderer`,
  never by `python3 -c`.**

- 2026-08-31 — the local `Dockerfile` is not a CI-shared layer, and a brief
  that assumed otherwise nearly shipped a local-only fix for a CI-wide
  problem (caught before dispatch). CI runs `aswf/ci-baseqt:2027.0` directly
  via `container:` in `ci.yml`; `tools/ci/local/Dockerfile` builds
  `natron-dev:2027.0` for the local loop alone. The only layer both share is
  `tools/ci/local/*.sh`. Anything that must reach CI belongs in a script, not
  the image.

- 2026-08-31 — `M2.P3.T1f`: the smoke-run instability was one memory-safety
  bug, not three symptoms and not the environment. `chooseFBConfig()` in
  `Engine/OSGLContext_x11.cpp` read an uninitialized `nativeCount` after
  `glXGetFBConfigs()` returned NULL without writing `*nelements`, then sized
  a `std::vector` from 17–70 GB of stack garbage. Whether that threw
  `bad_alloc`, got OOM-killed, or segfaulted was decided by the kernel's
  overcommit heuristic. **This retires two earlier entries in this file as
  separate mysteries:** M7's "SIGSEGV at `OSGLContext_x11.cpp:433` vs
  `encodings` error, non-deterministically" and the 2026-08-31 note that the
  GLX SIGSEGV "did not reproduce in ~15 runs" — it did not reproduce because
  the *benign* branch of the same bug was being taken. The `Error while
  loading OpenGL: std::bad_alloc` printed on every run was the mercy, not
  the symptom.

  Two hypotheses were wrong and are recorded so they are not re-tried: it is
  **not** Natron's RAM-proportional cache sizing (those are lazily-populated
  limits; a healthy run peaks at 120 MB), and swap exhaustion is
  **incidental** — full swap lowers the overcommit accept threshold and
  therefore makes kills *less* frequent, not more. The PM's drafted fix of
  bounding the cache in `tools/ci/local/test.sh` would have masked a real
  bug without preventing the allocation; it was correctly refused.

- 2026-08-31 — pre-existing OpenGL-layer defects found while fixing T1f,
  deliberately **not** fixed (out of M2's scope; candidates for M5 or a
  follow-up):
  - `Engine/OSGLContext_x11.cpp:314-331` — every `dlsym()` result is used
    with no NULL check (e.g. `QueryExtension` is called unconditionally at
    :333), so a missing symbol is a null-call crash rather than a graceful
    failure.
  - `~OSGLContext_x11()` never destroys `x11Window.handle` or
    `x11Window.colormap` — the X window and colormap leak on *every* context
    teardown.
  - `Engine/OSGLContext_mac.cpp:531,535` — same uninitialized-out-parameter
    class (`CGLQueryRendererInfo`/`CGLDescribeRenderer`). Not built by this
    Linux-only fork, so it is upstream's problem, but it is real.
  - The container has **no working OpenGL at all** (`glxinfo -B`: no RGB
    visual or fbconfig). Graceful degradation is now deterministic, but any
    future task needing real GL under CI requires an OSMesa/llvmpipe GLX in
    the image or an EGL/OSMesa path in Natron. Two ctest cases already
    self-skip on it.
  - For anyone touching `OSGLContext_wayland.cpp`: `/usr/local/include/EGL/`
    shadows Mesa's headers, lacks the `PFNEGL*PROC` typedefs, and drags in
    `X11/Xlib.h` whose `Bool`/`Status` macros break the Qt includes — which
    is most of what made `M2.P1.T2h` unwinnable. Mesa's `/usr/include/EGL/`
    copy has neither problem.

- 2026-08-31 — `M2.P3.T1d` was the wrong fix, and the PM briefed it wrongly.
  The smoke test failed on a missing `qtpy`, and the task was framed as "make
  the dependency available" without first asking whether the dependency should
  exist. It should not: the fork targets one Qt and one Python, qtpy exists to
  abstract over several, and it is not in the ASWF image — so satisfying it
  required a `pip install` that contradicts the container-as-is requirement
  recorded in `2026-08-30-sealed-package-network.md`. The user caught this;
  `M2.P3.T1g` reverses it. **The general lesson: when a build needs something
  the pinned container does not have, the first question is whether the
  build should need it at all, not how to install it.**

  Worth keeping from T1d's investigation, since it stays true: Natron's own
  startup `import qtpy` (`AppManager.cpp:1806`) has been failing in CI all
  along and failing *softly* — it logs to stderr and the error log, then
  continues — so nothing ever went red over it. That is why the gap survived
  this long, and it is an argument for hardening that import once it points
  at PySide6.

- 2026-08-31 — `M2.P3.T1g` removed qtpy, and with it the milestone's only
  real runtime assertion on the binding layer. The old check
  (`qtpy.API_NAME == 'PySide6'`) tested a **Natron-controlled** behaviour —
  that `QT_API` was set before `Py_Initialize()` — so an ordering regression
  in `initPython()` would have turned it red. Its replacement,
  `check_pyside6_bindings()`, tests properties of the vendored PySide6 wheel
  and **cannot fail in this image**. That is an acceptable outcome for T1g,
  whose job was removing a dependency, but it must not be mistaken for
  coverage: `M2.P3.T1a` now owns restoring a check that can actually fail.
  Recorded because a green tripwire reads exactly like a green guard.
