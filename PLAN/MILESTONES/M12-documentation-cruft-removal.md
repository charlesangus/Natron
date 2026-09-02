# Milestone 12: Documentation cruft removal

`urgent` · low technical risk, high context-pollution payoff. Almost entirely
deletion; the code keeps building because none of it is a build input.

Triggered by a concrete incident: `Documentation/source/maintainers/todo.rst`
was read during plan authoring as a source of *requirements*, importing eight
upstream `NatronGitHub/Natron` bug reports into M5 that nobody asked for. A
three-agent audit found the cause is systemic, not a one-off — all 17 files in
`Documentation/source/maintainers/` are upstream material (commits `9038672dd`,
`1d8c88c83`, July 2026) authored **six weeks before this fork's first commit**
and wired into a Sphinx toctree as if they were current guidance. Nothing
validates them: no workflow runs Sphinx, and `nightly.yml` sets
`paths-ignore: [Documentation]`.

The goal is not tidiness. It is that a doc which reads as authoritative while
describing a build, platform set, or roadmap this fork deleted will keep
producing wrong work — by humans and by agents.

## Phase 12.1: Stop the bleeding

- [x] M12.P1.T1 — Correct the agent-facing maintainer skill
  - files: `skills/natron-maintainer/SKILL.md`
  - approach: highest blast radius in the repo, because it is read by agents as
    instructions rather than as reference. L90 says
    ``Build (qmake, Qt5 only today): `qmake Project.pro && make` `` — a deleted
    file. L92-95 mandates astyle. L140 says "qmake **and** CMake must stay in
    sync." Replace with the real build (`tools/ci/local/*.sh`, CMake, Qt6.8) and
    the real formatter (`clang-format`, changed lines only, per
    `DECISIONS/2026-09-01-format-gate-changed-lines-only.md`). Remove every
    qmake/Qt5/astyle instruction.
  - verify: `grep -nE "qmake|Project\.pro|astyle|Qt ?5" skills/natron-maintainer/SKILL.md`
    returns nothing; every command the file names exists and runs.
  - size: S

- [x] M12.P1.T2 — Salvage the maintainer docs' non-recoverable knowledge
  - files: new `docs/maintainer-notes.md`
  - approach: capture, verbatim with provenance, only the passages the audits
    classed as knowledge NOT derivable from the code — design rationale,
    invariants, and gotchas. At minimum: the `shared_ptr`/`weak_ptr` cycle rule
    (parent→child strong, child→parent weak, never a second `shared_ptr` from
    raw `this`); serialization changes are data-loss-critical and must version;
    render code reads TLS `ParallelRenderArgs`, never live node state, and must
    poll the abort flag; cache invalidation is by 64-bit hash, which is why
    there is no dirty-tracking; `KnobI` is deliberately not a `QObject` and
    `KnobSignalSlotHandler` is the workaround; the `Node`/`EffectInstance` split
    exists to swap decoders and run render clones without touching graph
    topology; Fwd headers are why the project compiles at this size; OFX triage
    (all plugins → host glue, one plugin → that plugin's repo); the host↔plugin
    boundary is effectively a public API, so changes must be additive; undo must
    go through an undo command or it silently breaks; `ViewIdx` must be plumbed
    through or stereo silently breaks; `DEBUG` builds trap FP exceptions;
    `QT_NO_CAST_FROM_ASCII` requires `QString::fromUtf8`; numbers in
    `Gui20.cpp`/`NodeGraph45.cpp` carry no meaning. **Include the `QRegExp`
    note** — `QRegularExpression::match()` is find-anywhere and has no
    `exactMatch()` equivalent, so old `exactMatch` sites need `\A…\z` anchoring,
    and `WildcardUnix` needs `wildcardToRegularExpression()`. Every `QRegExp`
    call site is gone, so this is unrecoverable from the code. This file is a
    holding pen, not a destination: M6 distributes it to `CONTRIBUTING.md` and
    to comments at the relevant headers, then deletes it.
  - verify: each captured passage names its source file; a reader can act on
    every entry without consulting the deleted docs.
  - size: M

- [x] M12.P1.T3 — Delete the maintainers subtree
  - files: `Documentation/source/maintainers/` (17 files),
    `Documentation/source/index.rst`
  - approach: delete the directory outright, after M12.P1.T2 has captured the
    salvage. Four files are upstream's roadmap and issue tracker; the rest are
    either accurate-today indexes that will rot silently or actively wrong —
    `contributing.rst:19-27` sends contributors to `master`/`RB-x.y`, which this
    fork's own `CONTRIBUTING.md` calls frozen history; `building.rst:113-132`
    names astyle authoritative when the required `format` check runs
    clang-format; `subsystems.rst:50-65` documents the removed Breakpad
    subsystem and cites `ExistenceCheckerThread`, a class that exists nowhere.
    Remove the `maintainers/` toctree entry and the prose reference at
    `Documentation/source/index.rst:16`.
  - verify: `grep -rn "maintainers" Documentation/source/ --include=*.rst`
    returns nothing; `sphinx-build -b html` emits no missing-document warnings.
  - size: S

## Phase 12.2: Bulk removal

- [ ] M12.P2.T1 — Delete the dead build and release tooling
  - files: `tools/{jenkins,docker,homebrew,appimage,valgrind,uncrustify,normalize}/`,
    `tools/{README.md,mkTarballs.sh}`, `tools/utils/{fixpngs.sh,Natron-Linux-hotfix.sh,natron-sdk-setup-linux.sh}`,
    `tools/license/{ha-cla-README.txt,natron-cla-*.odt}`, `INSTALL_LINUX.md`
  - approach: ~453 files, 3.8 MB. `checks.yml:153` shellchecks only
    `tools/ci/local/*.sh`, so none of this gates anything. `tools/jenkins/` is
    102 Qt patches (mostly Qt 4.6-4.8), 13 Python 2.7 CVE patches, a breakpad
    patch, 143 superseded SDK scripts, and tarballs last touched in 2014; M3
    spared it only because `tools/docker/` references it, and `tools/docker/` is
    itself referenced by no workflow — the dependency is circular. Update the
    `INSTALL_LINUX.md` references at L39-43, L53-56 and L73 in the same commit.
    **Must not delete:** `tools/ci/`, `tools/utils/{sourceList,sourceCleanup}.py`
    (configure-time inputs to `Engine/CMakeLists.txt:49,55,56` and
    `Gui/CMakeLists.txt:42,48,49`), `tools/license/components/` (60 files
    embedded via `Gui/GuiResources.qrc:7-66` and rendered in the About window —
    legally required attribution), `tools/genStaticDocs.sh` (named as their
    contract by five C++ comments).
  - verify: a clean CMake configure and build succeeds; `format`, `lint-ci` and
    `build-and-test` pass; `grep -rn "tools/jenkins\|tools/docker" --include=*.md
    --include=*.sh --include=*.yml .` returns nothing outside git history.
  - size: M

- [ ] M12.P2.T2 — Delete the dead-platform and dead-infrastructure root files
  - files: `INSTALL_MACOS.md`, `INSTALL_FREEBSD.md`, `README_breakpad.md`,
    `OpenColorIO-Configs-README.md`, `BUGS.md`, `LATEST_VERSION_README.txt`,
    `.travis-coverity-scan-build.sh`, `Natron.rc`, `App/CMakeLists.txt`,
    `Renderer/CMakeLists.txt`, `README.md`
  - approach: all describe platforms, build systems or services this fork
    dropped. `Natron.rc` needs its `if(WIN32)` blocks removed alongside it
    (`App/CMakeLists.txt:21-23`, `Renderer/CMakeLists.txt:21-23`); it is already
    a broken orphan, pointing at `natronIcon256_windows.ico` which no longer
    exists. Fix the dangling links at `README.md:108-110` in the same commit
    (L110 already links a deleted `INSTALL_WINDOWS.md`); the fuller README
    rewrite is M12.P3.T1.
  - verify: configure and build succeed; no tracked file references any deleted
    path.
  - size: S

- [ ] M12.P2.T3 — Delete orphaned and checked-in artifacts
  - files: `Documentation/{Presentation.md,TuttleOFX-README.txt,ofxActionsSupported.rtf,ofxPropSupported.rtf,ofx_plugin_programming_guide.html}`,
    `Documentation/source/guide/tutorials-hugin.md`,
    (**not** `.github/workflows/verify_plugin_loads.cpp` — see below),
    `libs/{ceres,libmv}/files.txt`, the `mkfiles.sh` that generated them,
    `tools/docker/natron-sdk-centos6/.build.sh.swp`
  - approach: none is referenced by any build, workflow, qrc or toctree.
    **Keep `.github/workflows/verify_plugin_loads.cpp`.** It was slated as an
    orphan — a 58-line C++ file in a directory of YAML, referenced by nothing.
    M13's scout then had to hand-write exactly that probe (`dlopen` +
    `OfxGetNumberOfPlugins`/`OfxGetPlugin`) to verify the arena bundle, so it is
    the tool the job needs rather than dead weight. Move it somewhere
    `fetch-assets.sh` can build and run it; do not delete it.
    Highlights: a tracked Vim swap file committed in 2020; `.txt` manifests
    generated for the deleted `.pro` files, with zero hits in any CMake file; a
    771-line readme for a different project. Check the vendored HTML's license
    before deleting it. Add `*.swp` to `.gitignore`.
  - verify: configure and build succeed; `git status` clean; no dangling
    references.
  - size: S

- [x] ~~M12.P2.T4 — Delete the reference for plugins this fork does not build~~
  - **Cancelled 2026-09-02.** The premise was that openfx-gmic and openfx-arena
    are outside this fork's plugin surface. The user's decision is the opposite:
    build the full upstream plugin set. `eu.gmic.*`, `net.fxarena.*` and
    `_groupGMIC.rst` therefore document shipped capability and stay. See M13.

## Phase 12.3: Repair what must stay

- [ ] M12.P3.T1 — Rewrite the README for what this fork actually is
  - files: `README.md`
  - approach: L7 claims "portable and cross-platform (GNU/Linux, macOS, and
    Microsoft Windows)"; L20 "source is still C++98"; L22 "builds with Qt4 or
    Qt5, does not yet support Qt6"; L137 "main development branch is master…
    stable is RB-2.5"; L3 carries Travis and Coveralls badges for `rb-2.4`.
    State the real thing: Linux-only, Qt6.8, C++20, CMake, `main`, the three
    required checks.
  - verify: no claim in the file contradicts the tree; every link resolves.
  - size: S

- [ ] M12.P3.T2 — Reconcile the contributor-facing process docs
  - files: `CONTRIBUTING.md`, `.github/PULL_REQUEST_TEMPLATE.md`,
    `.github/ISSUE_TEMPLATE/*.yml`, `.git-hooks/pre-commit`
  - approach: `CONTRIBUTING.md` contradicts itself — L76 says use `master`,
    L114 says `main`. All four `.github/` templates route contributors to
    upstream `RB-2.4`; `bug.yml:40` defaults the OS field to "macOS 10.15
    Catalina", L59-69 asks about an official installer when packaging is
    deferred, and L93 asks for a Crash ID from a crash reporter cut in M0.
    `.git-hooks/pre-commit:10` hard-fails when astyle is absent while CI
    enforces clang-format — a contributor who installs the hook is fighting the
    gate. Make the hook run the same check CI does, or delete it.
  - verify: no doc names `master`, `RB-*` or astyle as current; the hook agrees
    with `checks.yml`.
  - size: S

- [ ] M12.P3.T3 — Prune dead entries from the ignore and attribute files
  - files: `.gitignore`, `.gitattributes`
  - approach: ~25 dead `.gitignore` entries (`config.pri` twice,
    `breakpadpro.pri`, 20 `*.xcodeproj/`); `.gitattributes` covers `*.pro`,
    `*.sln`, `*.vcxproj` and VC6 `*.dsp`, and has a typo at L42 (`eof=crlf`).
    Keep `/.plan/` handling and anything still live.
  - verify: `git status` is clean on a fresh checkout; `git check-attr` behaves
    for real file types.
  - size: S

- [ ] M12.P3.T4 — Delete the dead macOS and Windows GL backends
  - files: `Engine/OSGLContext_mac.{h,cpp}`, `Engine/OSGLContext_win.{h,cpp}`,
    `Engine/CMakeLists.txt`
  - approach: code, not docs, but found by this audit and caused by the same
    drift. Commit `056aa7b27` ("delete the dead Windows/macOS/qmake artifacts")
    enumerates what it removed and does not touch these — they were missed. Both
    are wrapped entirely in `#ifdef __NATRON_OSX__` / `__NATRON_WIN32__`, never
    defined on this build, so they compile to empty translation units. Two
    deleted docs described them as live GL backends, which is how they surfaced.
  - verify: configure and build succeed; `build-and-test` stays green;
    `grep -rn "OSGLContext_mac\|OSGLContext_win" --include=*.cpp --include=*.h
    --include=*.txt .` returns nothing.
  - size: S

## Decisions

- 2026-09-02 — `M12.P1.T1` removed the skill's whole "Issue Triage Method"
  section, not just its reference to the doomed `issue-triage.rst`. The section
  instructed agents to `curl api.github.com/repos/NatronGitHub/Natron/issues`
  and apply upstream's `prio:*`/P0-P3 labels — a live mechanism for pulling
  upstream work into this project, which would have survived deleting the doc
  link alone. The link was the symptom; the procedure was the defect.

- 2026-09-02 — the entire `Documentation/source/maintainers/` subtree is
  deleted rather than partially rewritten, even though the audits would have
  kept `architecture.rst` (no substantive falsehoods found) and rewritten
  `openfx-host.rst`. Rationale: the user's objection is drift, not present
  inaccuracy, and `gui.rst` demonstrated the problem — ~50 class names and
  numbered-file ranges, every claim verified true today, and still a liability
  because nothing forces it to track the code. Accurate-today indexes are the
  failure mode, not the exception. Fresh maintainer docs, if wanted, get written
  from the code in M6.

- 2026-09-02 — the inherited credential at `tools/jenkins/README.md:136` is
  removed by deletion only, with no history rewrite. It belongs to an upstream
  `natron-ci` account this fork does not control, it predates the fork, and it
  remains in `NatronGitHub/Natron`'s history regardless of what we do; rewriting
  our history would invalidate every SHA on `main` and break the plan branch's
  recorded code SHAs for no security gain. Recorded as a project-wide decision.

**Verification gate:** a clean configure and build succeeds and `format`,
`lint-ci` and `build-and-test` are green; no tracked file references a deleted
path; no remaining doc instructs a reader to use qmake, target Qt5, run astyle,
build for Windows or macOS, or treat `master`/`RB-*` as a merge target; and
every category-(b) passage the audits identified survives in
`docs/maintainer-notes.md` with its provenance.
