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

**Verification gate:** `INSTALL_LINUX.md`, the README, and the Qt6 chapter
describe the shipped Linux/Qt6/CMake-only build as current, with no
historical Qt4/qmake/multi-platform content presented as live.
