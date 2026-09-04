# Milestone 6: Documentation pass

`~0.5-1 day` · low risk. Scope is the repo's **own** docs — the ones a
contributor or an agent reads as instructions. The inherited Sphinx tree under
`Documentation/` is **not** in scope; it moved to M14.

No CI gate covers any of this — no workflow builds Sphinx, and `checks.yml`
runs only clang-format/shellcheck/actionlint — so every `verify` below is a
grep or a script, not a green check.

Re-elaborated 2026-09-03; the original four tasks are cancelled below. See
`## Decisions`.

## Phase 6.1: Make the root and agent-facing docs match what ships

- [x] M6.P1.T5 — Correct the README's plugin and binary-release claims
  - files: `README.md`
  - approach: `README.md:39` lists `OpenFX-G'MIC` with the `(+)` marker that
    `README.md:36` defines as "included in the binary releases". gmic is **not**
    built: the authoritative pin list is `tools/ci/local/fetch-assets.sh:256`
    (`PLUGINS_WANT` = openfx-io, seexpr, openfx-misc, lcms2, libzip,
    imagemagick, openfx-arena) producing four bundles at
    `fetch-assets.sh:261-264` (`IO.ofx`, `Misc.ofx`, `CImg.ofx`, `Arena.ofx`);
    gmic is deferred per
    `docs/decisions/2026-09-02-openfx-gmic-source-unavailable.md`.
    `README.md:40` (`OpenFX-Arena (+)`) is now **correct** — M13 made it true,
    do not "fix" it. Also re-word the `(+)` marker at `README.md:36`, which
    contradicts `README.md:93` ("This fork does not currently publish pre-built
    binaries"), to mean "built by this fork's CI from pinned source". Leave the
    graphics-card list (`README.md:73-87`) alone — stale but not a
    platform/plugin falsehood.
  - verify: `grep -in "gmic" README.md` returns nothing; the plugin bullet list
    names exactly the repos whose refs appear in `fetch-assets.sh:256`; no line
    claims a binary release exists.
  - size: S

- [x] M6.P1.T6 — Retire the `rb-2.4`-era Read the Docs links
  - files: `README.md` (lines 3 and 14 only), `.github/ISSUE_TEMPLATE/bug.yml`,
    `.github/ISSUE_TEMPLATE/config.yml`
  - approach: four links point a reader of *this fork* at upstream's 2.4-era
    manual: `README.md:3` (RTD badge for `version=rb-2.4`), `README.md:14`
    ("User documentation: https://natron.readthedocs.io/"),
    `bug.yml:19` (`…/en/rb-2.4/devel/natronexecution.html`), `config.yml:14`
    (`…/en/rb-2.4/guide/getstarted-troubleshooting.html`). This fork publishes
    nothing to that RTD project; the badge resolves to upstream's. **Delete
    rather than repoint** — whether this fork publishes docs at all is M14's
    subject, so pointing at a URL now would pre-empt it. Drop the badge at
    `README.md:3`, point `README.md:14` at `Documentation/source/` in-tree,
    drop the checkbox at `bug.yml:19` and the contact link at `config.yml:13-15`.
  - verify: `grep -rn "rb-2\.4\|readthedocs" README.md .github/` returns nothing.
  - size: S

- [x] M6.P1.T7 — Correct the local build README's description of the plugin set
  - files: `tools/ci/local/README.md`
  - approach: `tools/ci/local/README.md:56-57` says step 2 "**builds** the
    openfx-io OFX plugin bundle from source at pinned SHAs" — singular, pre-M13.
    It now builds four bundles plus three dependency libraries
    (`fetch-assets.sh:13-18`: `IO.ofx.bundle`, `Misc.ofx.bundle`,
    `CImg.ofx.bundle`, `Arena.ofx.bundle`, and `deps-install/` for
    lcms2/libzip/ImageMagick). `tools/ci/local/README.md:70-73` likewise reasons
    only about `IO.ofx` being self-contained via static SeExpr; Arena's
    self-containment is a different mechanism (bundle-root `Libraries/` plus the
    existing `RUNPATH`, per
    `docs/decisions/2026-09-02-build-openfx-arena.md`). Update both passages;
    keep the pointer to the `fetch-assets.sh` header as the full reasoning.
  - verify: every bundle name in `fetch-assets.sh:261-264` appears in
    `tools/ci/local/README.md`; no sentence describes the step as building one
    plugin.
  - size: S

- [x] M6.P1.T8 — Delete the stale "Qt 6 Migration Status" section from the maintainer skill
  - files: `skills/natron-maintainer/SKILL.md`
  - approach: `SKILL.md:117-119` still lists as **To do**:
    "`QDesktopWidget`/`QApplication::desktop()` → `QScreen` (a handful of files
    in `Gui`)". That is done — the only surviving occurrences are commented out
    (`Gui/CurveWidget.cpp:139`, `Gui/Histogram.cpp:299`,
    `Gui/ViewerGLPrivate.cpp:139`, `Gui/NodeCreationDialog.cpp:188`) and
    `Gui/GuiApplicationManager10.cpp:304-305` carries "QDesktopWidget is gone in
    Qt6; QScreen carries the same values". Delete the whole
    `## Qt 6 Migration Status (as of 2026)` heading and body (`SKILL.md:111-119`)
    rather than editing it: a dated status section in an agent-facing file is
    exactly the "accurate index that nothing forces to track the code" liability
    named in `docs/decisions/2026-09-02-inherited-docs-are-not-requirements.md`.
    The Qt6.8-only fact is already stated at `SKILL.md:82`.
  - verify: `grep -n "To do\|Migration Status\|QDesktopWidget"
    skills/natron-maintainer/SKILL.md` returns nothing.
  - size: S

- [ ] M6.P1.T9 — Settle the issue-tracker policy on this fork's own tracker
  - files: `README.md`, `CONTRIBUTING.md`, `.github/ISSUE_TEMPLATE/feature.yml`
  - approach: three docs give three answers. `README.md:11` says "this fork does
    not use GitHub Issues; bugs and feature requests are tracked upstream at
    NatronGitHub/Natron/issues"; `CONTRIBUTING.md:39-40` also points upstream;
    but `.github/ISSUE_TEMPLATE/bug.yml:22` and `feature.yml:19` link to
    `charlesangus/Natron/issues`. `gh api repos/charlesangus/Natron` reports
    `has_issues: true` as of 2026-09-03 — M12 recorded `false` on 2026-09-02, so
    this changed. **The templates are right and the prose is wrong:** rewrite
    `README.md:11` and `CONTRIBUTING.md:36-54` to route reporters to this fork's
    tracker. Keep `.github/ISSUE_TEMPLATE/`. Also drop the agreement checkbox at
    `feature.yml:15`, which requires assent to
    `hackmd.io/@natron-dev-awesome/B1SW6Hbau` — an upstream document this fork
    does not control and whose liveness could not be confirmed.
  - verify: no doc says this fork does not use GitHub Issues; `grep -rn
    "NatronGitHub/Natron/issues" README.md CONTRIBUTING.md .github/` returns
    nothing; `grep -n "hackmd" .github/ISSUE_TEMPLATE/feature.yml` returns
    nothing.
  - size: S

- [ ] M6.P1.T10 — Delete the inherited CHANGELOG and the placeholder code of conduct
  - files: `CHANGELOG.md`, `CODE_OF_CONDUCT.md`, plus any file referencing them
  - approach: both are inherited artifacts that mislead rather than inform.
    `CHANGELOG.md` is 1122 lines of pure upstream history ending at "Version
    2.5.0" with zero fork entries, and it opens (`:1-3`) with a `# Known Bugs`
    heading presented as current, linking a single upstream issue — git is this
    fork's changelog. `CODE_OF_CONDUCT.md:58` still carries the unfilled
    template placeholder "contacting the project team at [INSERT EMAIL
    ADDRESS]", i.e. the one document whose purpose is telling someone where to
    report harassment does not do so. Delete both, and fix every inbound
    reference (check `README.md`, `CONTRIBUTING.md`, `.github/`); GitHub surfaces
    a repo-level default code of conduct in their absence.
  - verify: neither file exists; `grep -rn "CHANGELOG\|CODE_OF_CONDUCT" --include=*.md
    --include=*.yml --include=*.yaml . --exclude-dir=.git --exclude-dir=.plan
    --exclude-dir=libs --exclude-dir=Documentation` returns nothing.
  - size: S

## Phase 6.2: Retire the `docs/maintainer-notes.md` holding pen

- [ ] M6.P2.T11 — Fold the contributor-facing invariants into `CONTRIBUTING.md`
  - files: `CONTRIBUTING.md`, `docs/maintainer-notes.md` (read only)
  - approach: `docs/maintainer-notes.md:1-5` declares itself a holding pen to be
    distributed and deleted (M12.P1.T2). Its factual claims were spot-checked and
    hold: `QT_NO_CAST_FROM_ASCII` at `CMakeLists.txt:103`,
    `Global/FloatingPointExceptions.h` exists, `natron-python` is the
    `OUTPUT_NAME` at `PythonBin/CMakeLists.txt:28`, `KnobSignalSlotHandler` at
    `Engine/Knob.h:54`. Move the *project-wide conventions* — everything that is
    guidance to a contributor rather than a fact about one file — into a new
    `Architecture Invariants` section in `CONTRIBUTING.md`, after `Coding Style`
    and before `This Fork: Branching Model`: the shared_ptr/weak_ptr ownership
    rule (`:9-16`), PIMPL and `Fwd`-header conventions (`:17-24`), the
    Node/EffectInstance split (`:33-40`), the headless-Engine rule (`:42-49`),
    render TLS / abort discipline / `Hash64` cache keying (`:51-67`),
    undo-command and `ViewIdx` rules (`:69-75`), the OpenFX host triage heuristic
    and API-stability rule (`:77-87`), serialization-versioning and `Py*`
    stability (`:89-101`), build/toolchain notes (`:103-116`), and the
    numbered-source-file convention (`:139-144`). Leave `:25-31`, `:53-60` to
    T12 and `:118-137` to T13.
  - verify: `CONTRIBUTING.md` has an `Architecture Invariants` section; every
    non-heading bullet in `docs/maintainer-notes.md` outside `:25-31`, `:53-60`
    and `:118-137` has a corresponding sentence in `CONTRIBUTING.md`.
  - size: M

- [x] M6.P2.T12 — Add the two header comments that clear the comment bar
  - files: `Engine/Knob.h`, `Engine/ParallelRenderArgs.h`
  - approach: **do not** distribute a dozen invariants as header comments. A
    comment survives only if it explains a non-obvious *why* at that site, and
    most of these notes are project-wide conventions with no single site —
    "parent→child is `shared_ptr`" pinned to `Engine/Knob.h` helps nobody editing
    `Engine/Project.h`, and the `BOOST_CLASS_VERSION` rule has no home at all
    (the macro appears across dozens of `Engine/*Serialization.h`). Exactly two
    items are non-obvious *and* site-local, and neither site is commented today:
    (a) `Engine/Knob.h:54`, where `class KnobSignalSlotHandler : public QObject`
    sits unexplained — add the why from `maintainer-notes.md:25-31` (knobs are
    created in large numbers and must stay lightweight and copyable, so `KnobI`
    is deliberately not a `QObject` and delegates signals to this companion);
    (b) `Engine/ParallelRenderArgs.h:245`, where `class ParallelRenderArgsSetter`
    is declared bare — add the why from `maintainer-notes.md:53-60` (per-render
    context is captured up front into TLS, not read from live node state; a new
    render entry point must install this or renders go inconsistent).
  - verify: `git diff --stat` touches exactly those two headers; the `format`
    job's changed-lines clang-format check is clean; each comment names the
    *reason*, not the mechanism.
  - size: S

- [ ] M6.P2.T13 — Preserve the QRegExp porting rule as a decision record and delete the holding pen
  - files: new `docs/decisions/<date>-qregexp-to-qregularexpression-mapping.md`,
    `docs/maintainer-notes.md` (deleted)
  - approach: `maintainer-notes.md:118-137` fits neither `CONTRIBUTING.md` nor a
    comment: it is a *historical migration rule* whose call sites no longer exist
    (the note records that no `QRegExp` remains, and that the surviving
    `QRegularExpression::wildcardToRegularExpression()` sites are
    `Engine/FileSystemModel.cpp`, `Gui/NodeCreationDialog.cpp`,
    `Gui/PreferencesPanel.cpp`, `Gui/NodeGraph45.cpp`,
    `Gui/RenderStatsDialog.cpp`). A contributor guide is the wrong place for a
    completed migration's semantics, and there is no site to comment.
    `docs/decisions/` is the project's archive for exactly this — adding a *new*
    record is not rewriting a historical one. Write up the `indexIn` → `match()`
    and `exactMatch` → anchored-pattern (`\A…\z`) gap plus the wildcard-mode
    caveat, marked retrospective. Then `git rm docs/maintainer-notes.md`.
  - verify: `docs/maintainer-notes.md` no longer exists; `grep -rn
    "maintainer-notes" . --exclude-dir=.git --exclude-dir=.plan
    --exclude-dir=build` returns nothing.
  - size: S

## Cancelled tasks

- [ ] ~~M6.P1.T1 — Rewrite `INSTALL_LINUX.md` from scratch~~
  - **Cancelled 2026-09-03.** M12.P2.T1 deleted the file. `README.md:99` now
    points at `tools/ci/local/README.md`, which M6.P1.T7 audits instead.
- [ ] ~~M6.P1.T2 — Retire or archive the macOS/Windows maintainer chapters~~
  - **Cancelled 2026-09-03.** M12.P1.T3 deleted the entire
    `Documentation/source/maintainers/` subtree. The task's "keep as historical
    reference with an out-of-scope notice" approach was explicitly overruled by
    `docs/decisions/2026-09-02-inherited-docs-are-not-requirements.md`.
- [ ] ~~M6.P1.T3 — Update the README and Qt6 chapter to reflect "done," not "planned"~~
  - **Cancelled 2026-09-03.** `qt6-migration.rst` no longer exists and M12.P3.T1
    rewrote `README.md`. The README's remaining defects are narrower and are now
    M6.P1.T5, T6 and T9.
- [ ] ~~M6.P1.T4 — Reconcile guide prose with the plugin set that actually ships~~
  - **Cancelled 2026-09-03.** Its subject is the `Documentation/` tree, which
    moved to M14; its README half became M6.P1.T5.

## Decisions

- 2026-09-03 — M6 was re-elaborated wholesale rather than freshness-patched.
  Three of its four tasks named files M12 had deleted (`INSTALL_LINUX.md`,
  `Documentation/source/maintainers/build-macos.rst`, `qt6-migration.rst`) and
  the fourth's premise was partly inverted by M13 building the full plugin set.
  Per PLAN-FORMAT.md §5a, when most of a milestone fails the check its premise
  has changed and it is treated as a stub. Old IDs T1-T4 are struck through, not
  reused; new tasks start at T5.

- 2026-09-03 — the inherited Sphinx tree under `Documentation/` is **out of
  M6's scope** and became M14. The user's call, asked as a scope question: the
  tree's correctness and its (absent) doc CI are one coherent body of work —
  ~70 pages of 2.4-era content, dead-platform install chapters, a `ReadSVG`
  tutorial for a plugin this fork does not build, two dangling toctree entries,
  and a `.readthedocs.yaml` that points at upstream's RTD project with
  requirements that likely will not install on the Python it requests. Folding
  that into a "documentation pass" would have made M6 the largest milestone in
  the plan. M6 keeps the docs a contributor or agent reads as *instructions*.

- 2026-09-03 — `.github/ISSUE_TEMPLATE/` is kept and the prose is fixed, not the
  reverse. `gh api repos/charlesangus/Natron` reports `has_issues: true`, which
  contradicts M12's 2026-09-02 record of `false`; the setting changed. The user
  confirmed this fork's own tracker is the policy.

- 2026-09-03 — `CODE_OF_CONDUCT.md` is deleted rather than given a contact
  address, and `CHANGELOG.md` is deleted rather than annotated ("git is the
  changelog"). Both were offered to the user as fix-or-delete; both came back
  delete.

- 2026-09-03 — `M6.P1.T5` left the plugin bullets pointing at
  `github.com/NatronGitHub/openfx-*` even though `fetch-assets.sh` pins
  **`charlesangus/openfx-io`** (`:202`) and **`charlesangus/openfx-arena`**
  (`:249`) — this fork's own forks; only `openfx-misc` (`:214`) is genuinely
  pinned to NatronGitHub. Surfaced by the reviewer, deliberately not fixed:
  `DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md` frames those forks
  as thin, upstream-tracking deltas meant to stay indistinguishable from
  upstream for a general reader, so linking the canonical project page is
  defensible. Raised with the user rather than decided here.

- 2026-09-03 — `M6.P1.T8`'s deletion also removed a "To do: regenerate
  PySide6/Shiboken6 bindings (fixes enum/flag issue #854)" item. Checked before
  accepting the loss: that work is **done**, not dropped — `M2.P3.T1` completed
  it on 2026-08-30 and `DECISIONS/2026-08-31-drop-qtpy.md` records that #854's
  enum-semantics failure mode cannot occur once nothing consults qtpy. The one
  residue is a test-coverage gap, not unfinished binding work:
  `PyGuiApplication::addMenuCommand` is the only remaining QFlags-taking bound
  API and is GUI-only, so headless CI does not exercise it.

- 2026-09-03 — the deleted section's claim that "remaining `#if QT_VERSION`
  guards only gate Qt 6 minor-version features" was **not** re-homed as durable
  guidance, because it is false. `Gui/Gui.cpp:284,416` and
  `Gui/TableModelView.cpp:46,1011` match it, but `Global/GlobalDefines.h:59` is
  a Qt5-era `#error` floor check (`QT_VERSION < 5.15.3`) that is dead in a
  Qt 6.8-only build, and `libs/qhttpserver/src/qhttpconnection.cpp:153` gates on
  Qt 5.0. Restating the generalisation would have recreated the same drift
  liability the task removed. The dead `GlobalDefines.h` guard is a real but
  out-of-scope code finding — it belongs to a Qt5 dead-code sweep, not a docs
  pass.

- 2026-09-03 — `M6.P1.T7` left `tools/ci/local/README.md`'s pre-existing
  "the resulting `IO.ofx` is self-contained" unchanged, though it overstates.
  `fetch-assets.sh:635-641` records that `IO.ofx` also carries
  `RUNPATH=$ORIGIN/../../Libraries` with nothing populating that directory, so a
  bare `dlopen` of it resolves OCIO/OIIO only via the container's ldconfig
  state — which is why the script treats that probe as a non-gating warning.
  The sentence is defensible read as scoped to SeExpr (the one library that
  would otherwise need `LD_LIBRARY_PATH`), it predates this task, and the
  task's own added claim about `LD_LIBRARY_PATH` is literally true. Noted rather
  than caveated, to keep the task to one coherent change.

- 2026-09-03 — `M6.P1.T6`'s reviewer surfaced two things for later tasks.
  (a) `.github/ISSUE_TEMPLATE/config.yml` carries a **second** link to
  `CODE_OF_CONDUCT.md` (a "Read the Code of Conduct" contact entry) beyond the
  README badge; `M6.P1.T10` must clear both or it leaves a dangling link.
  (b) The README badge row's `repology` and `openhub` badges report metrics for
  the **upstream** `natron` project, not this fork — arguably the same class of
  falsehood as the RTD badge just removed, since a reader would read upstream's
  packaging and community-health stats as this fork's. Left alone as out of
  scope for T6; raised with the user rather than folded silently into T10.

- 2026-09-03 — the `format` gate **can** be run locally after all, which
  matters for every remaining code-touching task. `pip install clang-format`
  fails plain, but `pip install --user --break-system-packages
  "clang-format==21.1.8"` succeeds, and the CI invocation then reproduces
  exactly: `git clang-format --binary $(python3 -c 'import clang_format, os;
  print(os.path.join(os.path.dirname(clang_format.__file__), "data", "bin",
  "clang-format"))') --extensions c,cc,cpp,h,hpp,mm --diff HEAD -- <paths>`.
  Found by `M6.P2.T12`'s reviewer, which used it to catch a real violation (a
  stray double blank line) that would otherwise have failed CI. There is no
  Docker or general network access in this sandbox, so this is the only local
  gate available.

- 2026-09-03 — M12's deferred idea of writing fresh maintainer docs from the
  code is **not** in M6. `docs/decisions/2026-09-02-inherited-docs-are-not-requirements.md`
  argues `gui.rst` was deleted *despite every claim verifying true*, because an
  accurate index that nothing forces to track the code is a liability with a
  delay fuse. Writing a new one re-lights that fuse. `CONTRIBUTING.md`'s new
  invariants section (M6.P2.T11) plus `skills/natron-maintainer/SKILL.md` cover
  the onboarding need and are short enough to stay honest.

**Verification gate:** `grep -rin "gmic\|rb-2\.4\|readthedocs"
README.md CONTRIBUTING.md .github/ skills/ tools/ci/local/README.md` returns
nothing; the plugin names in `README.md` and `tools/ci/local/README.md` are a
subset of the bundles in `tools/ci/local/fetch-assets.sh:261-264`;
`CHANGELOG.md`, `CODE_OF_CONDUCT.md` and `docs/maintainer-notes.md` are gone
with no dangling references; no root or agent-facing doc routes a contributor to
`master`, `RB-*`, upstream's issue tracker, or a plugin this fork does not
build; and `format`, `lint-ci` and `build-and-test` are green.
