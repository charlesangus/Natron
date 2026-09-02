# Milestone 6: Documentation pass

`~1-2 days` · low risk. Do this last, once the build is actually the thing
being documented.

## Phase 6.1: Rewrite for the fork's actual shape

- [ ] M6.P1.T1 — Rewrite `INSTALL_LINUX.md` from scratch
  - files: `INSTALL_LINUX.md`
  - approach: one path — CMake + Qt6 + `aswf/ci-baseqt:2027` (or an
    equivalent local setup) on Rocky Linux 9. Drop
    Docker/CentOS-SDK/qmake/Qt4/config.pri/Arch-AUR content — all of it
    describes builds this fork no longer supports.
  - verify: a contributor following the doc verbatim on a fresh Rocky Linux 9
    machine (or the ASWF container) produces a working build.
  - size: M

- [ ] M6.P1.T2 — Retire or archive the macOS/Windows maintainer chapters
  - files: `Documentation/source/maintainers/build-macos.rst` and equivalent
    Windows chapter(s)
  - approach: keep as historical reference if this ever needs to go
    multi-platform again, but mark them explicitly out of scope so nobody
    spends time keeping them current.
  - verify: each retired chapter carries an explicit "out of scope for this
    fork" notice at the top.
  - size: S

- [ ] M6.P1.T3 — Update the README and Qt6 chapter to reflect "done," not "planned"
  - files: `README.md`, `Documentation/source/maintainers/qt6-migration.rst`
  - approach: `qt6-migration.rst` is written as a plan against a Qt5
    baseline — once M2 lands, it becomes historical record; replace it with
    the current architecture doc.
  - verify: neither doc references Qt5 as the current toolkit or describes
    the migration as future work.
  - size: S

- [ ] M6.P1.T4 — Reconcile guide prose with the plugin set that actually ships
  - files: `Documentation/source/guide/tutorials-svgworkflow.rst`,
    `Documentation/source/guide/tutorials-writedoc.rst`,
    `Documentation/source/guide/getstarted-about-features.rst`,
    `Documentation/source/guide/tutorials.rst`
  - approach: M13 deleted the reference pages for plugins this fork does not
    build, but left hand-written guide prose that still assumes them.
    `tutorials-svgworkflow.rst` is an entire tutorial for a workflow `ReadSVG`
    provided, and it cannot be followed at all; the other two list
    `openfx-gmic` among the shipped plugin sets. Retire the SVG tutorial (and
    its `tutorials.rst` toctree entry) and correct the two feature lists.
    Unlike `source/plugins/`, none of this is generated — `genStaticDocs.sh`
    will not restore it, so deleting is a real decision, not a no-op.
  - verify: no page under `Documentation/source/guide/` instructs the reader to
    use a plugin absent from the shipped bundles; no toctree entry dangles.
  - size: S

**Verification gate:** `INSTALL_LINUX.md`, the README, and the Qt6 chapter
describe the shipped Linux/Qt6/CMake-only build as current, with no
historical Qt4/qmake/multi-platform content presented as live; and no guide
page describes plugin capability this fork does not build.
