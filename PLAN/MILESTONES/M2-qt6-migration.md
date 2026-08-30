# Milestone 2: Land the Qt6 migration

`~2-3 weeks` · highest risk. The bulk of the effort, but most of it is already
scoped or already written. Three pieces of this exist as open upstream PRs —
pull them in rather than redoing the work.

Because Qt5 isn't being kept, skip upstream's discipline of gating everything
behind `#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)`. Apply replacements
unconditionally and delete the old branch — less code comes out of this
milestone than went in.

## Phase 2.1: Pull in existing Qt6 work

- [ ] M2.P1.T1 — Cherry-pick `lockewerks`'s PRs #1084 & #1085
  - files: whatever the two PRs touch (per upstream diff)
  - approach: *(ready now)* "Replace the Qt6-removed APIs that have Qt5
    equivalents" and "Build against C++20" — both open, mergeable, dated 25
    Aug 2026. Written directly off the maintainer guide's Qt6 audit.
  - verify: both PRs cherry-pick cleanly (or with trivial conflict
    resolution) and the project still builds.
  - size: M

- [ ] M2.P1.T2 — Rebase PR #1019 ("Qt6 support")
  - files: whatever PR #1019 touches (per upstream diff, expect drift)
  - approach: *(biggest unknown)* opened Dec 2024, now stale and conflicting.
    Budget extra time for whatever's drifted in 20 months — expect to redo
    parts by hand rather than a clean rebase.
  - verify: rebased branch builds cleanly against the current M1 toolchain
    baseline.
  - size: L

## Phase 2.2: Mechanical Qt6 API replacements

- [ ] M2.P2.T1 — Fix the ~33 `QRegExp` sites (16 files)
  - files: `ScriptTextEdit.cpp`, `Project.cpp`, `CLArgs.cpp`,
    `NodeCreationDialog.cpp`, and the remaining sites inventoried in
    `Documentation/source/maintainers/qt6-migration.rst`
  - approach: verified `QRegularExpression` replacements are already written
    and compile-tested in that doc — this is application work, not
    investigation.
  - verify: `git grep -c QRegExp` across the tree returns 0; project builds.
  - size: M

- [ ] M2.P2.T2 — Fix `QDesktopWidget` (6 files) and the one `QVariant::Type` site
  - files: the 6 `QDesktopWidget` sites and the single `QVariant::Type` site,
    per `Documentation/source/maintainers/qt6-migration.rst`
  - approach: replace with `QScreen`/`QGuiApplication::screens()` and
    `QMetaType` respectively — also pre-audited in the same doc.
  - verify: `git grep -c QDesktopWidget` and `git grep -c 'QVariant::Type'`
    across the tree both return 0; project builds.
  - size: S

## Phase 2.3: Bindings and validation

- [ ] M2.P3.T1 — Regenerate PySide6/Shiboken6 bindings
  - files: `Engine/NatronEngine`, `Gui/NatronGui`
  - approach: fix the enum/`QFlags` class of runtime bugs (upstream issue
    #854 — flags passed as raw `int` no longer implicitly convert in
    Qt6/PySide6). This is where most *runtime*, as opposed to compile-time,
    surprises will show up.
  - verify: regenerated bindings build; a Python console session exercising
    enum/flags-taking APIs (e.g. node property setters) runs without
    conversion errors.
  - size: L

- [ ] M2.P3.T2 — Validate the GUI end-to-end
  - files: none (manual QA pass across the running application)
  - approach: file dialogs, node graph, viewer, curve editor/dope sheet,
    roto/tracker overlays, Python console. There's no automated UI test
    suite, so this is a manual pass — mirror the checklist upstream used for
    the Qt4→Qt5 migration (referenced in issue #827).
  - verify: each area on the checklist opens and operates without visual
    corruption or crashes on Qt6.
  - size: M

**Verification gate:** the ctest suite (M5) passes, the Python bindings load
and operate without enum/QFlags errors, and the manual GUI checklist above
passes end-to-end on Qt6.8.x.
