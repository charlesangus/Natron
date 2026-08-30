# Milestone 0: Fork & cut scope

`~1 day` · low risk. Housekeeping that makes every later phase smaller — do this
before touching Qt.

## Phase 0.1: Cut cross-platform and legacy scope

- [ ] M0.P1.T1 — Protect the default branch
  - files: GitHub repo branch protection settings (no source files)
  - approach: require the CI workflow from M4 to pass before merge. This
    directly fixes the structural failure mode upstream has right now — a solo
    push broke the release build and nothing caught it for 5+ weeks. Can be
    enabled once M4's workflow exists, but the rule itself can be scaffolded
    now and pointed at the workflow when it lands.
  - verify: attempt a direct push to main is rejected; a PR without a passing
    check cannot merge.
  - size: S

- [x] M0.P1.T2 — Delete Windows/macOS CI & packaging
  - files: `.github/workflows/build_installer.yml`, `.github/workflows/build_pacman_repo.yml`,
    `tools/jenkins/build-OSX-installer.sh`, `tools/jenkins/build-Windows-installer.sh`,
    `*.bat`, `msys*`, `tools/MacPorts/`
  - approach: remove these files/directories outright — no transition period,
    since Windows/macOS aren't being supported going forward.
  - verify: `git grep` for the deleted paths returns nothing; `.github/workflows/`
    contains no Windows/macOS jobs.
  - size: S

- [x] M0.P1.T3 — Delete the qmake build entirely
  - files: all 11 `*.pro` files (`Project.pro`, `Engine/Engine.pro`, etc.),
    `global.pri`, `config.pri`
  - approach: CMake is already the more complete build (it's the one with the
    Qt6 switch) — no transition period needed since Qt5 isn't being supported
    anyway.
  - verify: `find . -name '*.pro' -o -name '*.pri'` returns nothing; CMake
    build still configures and builds.
  - size: S

- [ ] M0.P1.T4 — Strip Qt4-era doc content
  - files: `INSTALL_LINUX.md`
  - approach: cut the sections documenting Qt4/PySide/Shiboken (Python 2) as a
    live option and the Ubuntu 18.04 Travis references — cut now rather than
    porting it forward; the full rewrite happens in M6.
  - verify: `INSTALL_LINUX.md` no longer mentions Qt4, PySide (unversioned),
    Shiboken (unversioned), Python 2, or Ubuntu 18.04/Travis.
  - size: S

- [ ] M0.P1.T5 — Decide on Breakpad
  - files: `libs/google-breakpad`, `CrashReporter`, `CrashReporterCLI`, `BreakpadClient`
  - approach: keep Breakpad — Linux minidumps are still useful — but confirm no
    Windows/macOS-specific symbol upload steps survive now that
    `build_installer.yml` (M0.P1.T2) is gone.
  - verify: Breakpad-related CMake targets still configure and build on Linux;
    `git grep` for symbol-upload steps in remaining CI config returns nothing
    Windows/macOS-specific.
  - size: S

- [x] M0.P1.T6 — Keep the "Natron" name and branding
  - files: none (decision only)
  - approach: confirmed — stays recognizable to the existing user base while
    the fork tracks upstream closely. See decision
    `PLAN/DECISIONS/2026-08-29-keep-natron-branding.md`.
  - verify: n/a — recorded decision, no code change.
  - size: S

**Verification gate:** a clean checkout builds via CMake only, with no
Windows/macOS CI, packaging, qmake, or Qt4-era doc content remaining, and
branch protection is wired to the M4 workflow once it exists.

## Decisions

- 2026-08-29 — Defer M0.P1.T1 (branch protection) to M4: user chose not to
  scaffold branch protection without a required status check. The rule will
  be set up in M4, once the CI workflow it gates on actually exists, rather
  than being enabled now and edited later.
- 2026-08-29 — M0.P1.T2 also removed `ci.yml`'s live `win-test` job and the
  dead macOS-only step in `unix_test` (not in the original file list, but
  required to satisfy the task's own verify criterion: no Windows/macOS jobs
  in `.github/workflows/`), plus the now-orphaned
  `install_natron_pacman_repo.sh`. Stale text mentions of `MINGW-packages`/
  `MacPorts` remain in `tools/README.md`, `tools/jenkins/common.sh`,
  `tools/jenkins/build-plugins.sh`, and
  `Documentation/source/maintainers/codebase-map.rst` — all inside
  Windows-only conditional branches or prose, not live on the Linux path.
  Left for M6 (documentation pass) / a later Jenkins-script cleanup rather
  than expanding T2's scope further.
- 2026-08-29 — M0.P1.T3 deleted `CrashReporter.pro`, `CrashReporterCLI.pro`,
  and `BreakpadClient.pro`. These were already gated out of the qmake default
  build (`CONFIG(enable-breakpad)`) and **CMake never had any build path for
  them at all** — no `CMakeLists.txt` under `CrashReporter/`,
  `CrashReporterCLI/`, or `BreakpadClient/`, and zero Breakpad references in
  the top-level `CMakeLists.txt`. This directly affects M0.P1.T5 ("Decide on
  Breakpad"), whose verify step assumes Breakpad CMake targets already exist
  — they don't. Raised to the user before starting T5.
