# Milestone 3: Dependency modernization

`~3-5 days` · low-medium risk · runs in parallel with M2 on a separate branch.

> **Re-planned 2026-08-31 after a freshness check (PLAN-FORMAT.md §5a).** This
> milestone was written to run alongside M2; five milestones have shipped since,
> and its premises moved. Everything below the `## Phase 3.1` heading down to
> the original gate is **historical record** — `M3.P1.T1` is delivered,
> `M3.P1.T2` is answered, and `M3.P1.T3`/`M3.P1.T4` turned out larger than
> scoped. The live task list is `## Phase 3.2` onward. Note every reference to
> `aswf/ci-baseqt:2027` in the historical section is stale; the image is
> `aswf/ci-vfxall:2027-clang21.1`.

Much lighter than initially scoped: `aswf/ci-baseqt:2027` already *is* the
pinned VFX-Platform dependency set — Boost 1.88, OpenColorIO 2.5.x, OpenEXR
3.4.x, Imath 3.2.x, OpenVDB 13.x, Alembic 1.8.x, Qt 6.8.x, PySide6/Shiboken6
6.8.x, all pre-built into `/usr/local`. No custom base image, no building
libraries from source, no Dockerfile to maintain. This milestone is now mostly
about pointing Natron's build at that image and handling what's specific to
Natron.

**Risk note:** this milestone is still betting on the CY2027 draft's
direction (`PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md`), but the
fallback is cheap — if Rocky 9/glibc 2.34 turns out premature, swapping to
`aswf/ci-baseqt:2026` (Rocky 8/glibc 2.28, CY2026-final) is a one-line tag
change in M4's CI, not a rebuild of custom infrastructure.

## Phase 3.1: Point the build at the ASWF image

- [x] M3.P1.T1 — Wire CMake's `find_package` calls at the ASWF-image paths
  - files: `CMakeLists.txt` and any `Find*.cmake` modules under the repo
  - approach: everything lands in `/usr/local` in the image rather than
    distro-standard paths — set `CMAKE_PREFIX_PATH`/`PKG_CONFIG_PATH`
    accordingly. This replaces the entire `NATRON_SYSTEM_LIBS` vs.
    custom-SDK question — it's neither; it's "build against what ASWF ships."
  - verify: CMake configure inside `aswf/ci-baseqt:2027` finds all
    dependencies with no manual path overrides beyond
    `CMAKE_PREFIX_PATH`/`PKG_CONFIG_PATH`.
  - size: S

- [x] M3.P1.T2 — Confirm exact pinned versions with `ci-baseqt:2027`'s package manifest
  - files: none (verification against the image's own docs/labels)
  - approach: the "2027" tag tracks the draft — re-check versions against the
    image's own docs/labels before relying on specific numbers in code or
    docs, since the draft (and the image) can still move before Jan 2027.
  - verify: version numbers referenced in M3/M6 docs match what
    `docker run aswf/ci-baseqt:2027` actually reports.
  - size: S

- [ ] ~~M3.P1.T3 — Swap the OCIO config default to ACES 2.0~~ — superseded by
  `M3.P1.T6`, `M3.P1.T7` and `M3.P1.T8`
  - files: OCIO config wiring (wherever the current default config path/URL
    is set — the `OpenColorIO-Configs` tarball reference)
  - approach: Natron currently ships a 2018-era `OpenColorIO-Configs` tarball
    (blender/natron/nuke-default profiles) — a separate download, not
    something the ASWF image provides. ACES 2.0 is the VFX-Platform-mandated
    default for CY2026+; this is a color-science-visible change to the
    out-of-the-box experience, not just a build detail. Flag it in release
    notes.
  - verify: a fresh install opens with ACES 2.0 as the active OCIO config;
    release notes mention the change.
  - size: M

- [ ] ~~M3.P1.T4 — Check plugin-repo compatibility~~ — `openfx-io` half is
  delivered; the `openfx-misc` half is superseded by `M3.P1.T9`
  - files: none in this repo (verification against `openfx-io`/`openfx-misc`)
  - approach: `openfx-io` and `openfx-misc` are separate repos (not
    submodules) that CI currently downloads as prebuilt zips. No Qt
    dependency, but confirm they build cleanly against the image's gcc
    14.2/C++20/OpenColorIO 2.5/OpenEXR 3.4 before wiring them into the new CI
    (M4).
  - verify: both plugin repos build successfully inside
    `aswf/ci-baseqt:2027`.
  - size: S

_(Superseded gate, kept as record: "the project builds cleanly inside
`aswf/ci-baseqt:2027` with no distro-package fallbacks, ACES 2.0 is the default
OCIO config, and `openfx-io`/`openfx-misc` build against the same image.")_

## Phase 3.2: What actually remains

- [x] M3.P1.T5 — Close out T1 and T2 as delivered
  - files: none (record only)
  - approach: `M3.P1.T1` landed during the Qt6 migration and went further than
    its brief: `build.sh` passes `-DCMAKE_PREFIX_PATH=/usr/local` on every
    configure, and resolving a HarfBuzz/FreeType link failure additionally
    required `find_package(harfbuzz CONFIG REQUIRED)` and ordered, promoted
    link entries (`CMakeLists.txt`, `Engine/CMakeLists.txt`). No
    `PKG_CONFIG_PATH` override was needed. `M3.P1.T2`'s verification is
    captured in `DECISIONS/2026-08-31-switch-ci-image-to-vfxall.md` and
    `2026-08-29-pin-exact-aswf-tag.md`, which also carries the standing rule to
    re-verify against the releases list before any future bump — a better home
    than a one-time checkbox. Note the old brief's Boost 1.88 is wrong; the
    image ships 1.91.0.
  - verify: recorded in this file's `## Decisions`.
  - size: S

- [x] M3.P1.T6 — Confirm OCIO 2.5.2 publishes an ACES 2.0 built-in config
  - files: none (verification + a decision entry in this file)
  - approach: `DECISIONS/2026-08-31-aces-via-ocio-builtin-config.md` rests on
    one unverified premise — that ACES 2.0 specifically is among the built-in
    configs OpenColorIO 2.5.2 publishes. Settle it before any code moves. Query
    the library the image actually ships rather than the docs: OCIO's Python or
    C++ API enumerates built-in configs, and `ocio://` URIs resolve through
    `OCIO::Config::CreateFromBuiltinConfig`. Record the exact URI string to use
    (built-in names are versioned and specific, e.g. the
    `studio-config`/`cg-config` families). **If ACES 2.0 is not published at
    2.5.2, stop and say so** — the fallback is an external ACES config set,
    which reopens the install-path work this decision was chosen to avoid, and
    that is a change of plan rather than a change of task.
  - verify: a `## Decisions` entry naming the exact `ocio://` URI and the
    command whose output confirmed it, run inside
    `aswf/ci-vfxall:2027-clang21.1`.
  - size: S

- [x] M3.P1.T7 — Make the default OCIO config the ACES 2.0 Studio built-in
  - files: `Engine/Settings.cpp` (`NATRON_DEFAULT_OCIO_CONFIG_NAME` ~line 70,
    the `_ocioConfigKnob` population ~lines 626-658, and the resolution in
    `restoreOCIOConfig`-style code ~lines 2148-2221), `tools/ci/local/test.sh`
  - approach: target
    `ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5` per
    `DECISIONS/2026-08-31-aces-via-ocio-builtin-config.md`. The mechanism is
    friendlier than it looks: Natron ultimately just **sets the `OCIO`
    environment variable** to a config path and lets OCIO and the plugins pick
    it up, and OCIO accepts an `ocio://` URI there as readily as a file path.
    The work is in the two places that assume a *directory on disk*: the
    `_ocioConfigKnob` choice list is populated by enumerating subdirectories of
    the OCIO configs dir, and resolution appends `.ocio`/`config.ocio` and
    calls `errorDialog` when the file is missing. Add the built-in as a first
    class entry that bypasses that path rather than special-casing a fake
    directory name. Keep the existing behaviour intact: the `OCIO` environment
    variable still overrides everything, `Custom config` still works, and a
    user-supplied on-disk config still wins. `test.sh` currently pins
    `OCIO=.../OpenColorIO-Configs/blender/config.ocio` from the fetched
    tarball; decide deliberately whether CI keeps testing the tarball or moves
    to the built-in, and say which in the report.
  - verify: a from-source build with no `OCIO` environment variable set and no
    config directory installed starts with
    `studio-config-v4.0.0_aces-v2.0_ocio-v2.5` active; setting `OCIO` to an
    on-disk config still overrides it; `Custom config` still resolves.
  - size: M

- [x] M3.P1.T12 — Patch the `openfx-io` fork's colorspace resolution
  - files: our `charlesangus/openfx-io` fork (`IOSupport/GenericOCIO.cpp`), then
    the pinned SHA in `tools/ci/local/fetch-assets.sh`
  - approach: per `DECISIONS/2026-09-01-fix-openfx-io-colorspace-sentinel.md`
    and `2026-09-01-png-output-srgb-display.md`. **Blocks `M3.P1.T7`** — the
    ACES default cannot ship until this lands, because a fresh project cannot
    render without it. Five changes, ~41 lines, one file: guard the `-1`
    sentinel in `canonicalizeColorSpace()`; add an
    `existingColorSpaceOrFallback()` preferring the name itself, then the
    `scene_linear` role, then `default`, then colorspace 0; use it in
    `describeInContextInput`/`describeInContextOutput`; add `sRGB - Display`
    and `Camera Rec.709` to `colorSpaceName()`'s chains; and fix
    `GenericOCIO.cpp:958`, which writes the literal `"default"` regardless of
    what it just computed. A working patch exists at
    `/tmp/ocioprobe/patch.diff` with a built bundle at `/tmp/ofxio-patched/` —
    treat it as a reference, not as something to apply blind. Land the sentinel
    guard as its own commit; keeping it self-contained is what leaves
    upstreaming cheap, though it is not being upstreamed
    (`DECISIONS/2026-09-01-no-upstream-pr-for-ocio-sentinel.md`).
  - verify: patched plugin on the old `blender` config produces byte-identical
    output to the stock plugin (regression check); on the ACES built-in a fresh
    project renders, and scene-linear 0.18 through EXR→PNG lands on 118/255,
    not 46/255.
  - size: M

- [x] M3.P1.T13 — Give the smoke test a mixed-format assertion
  - files: `tools/ci/smoke_test.py` (or wherever the smoke test lives),
    `tools/ci/local/test.sh`
  - approach: **the smoke test currently cannot tell a correct colour fix from a
    wrong one.** It round-trips PNG→PNG, so a reader and writer that are both
    wrong cancel out — the 2.5x-too-dark `scene_linear` substitution passes it
    cleanly. Add a mixed-format case: render a known scene-linear value through
    EXR→PNG and assert the resulting 8-bit code value. Roughly ten lines, and it
    is the only thing standing between this project and shipping a silent
    colour error. **Fix the exit code first, or the assertion is worthless:**
    `T12` found that `smoke_test.py` prints `[smoke] SMOKE TEST FAILED` and then
    lets `NatronRenderer` exit **0** — the `SystemExit` surfaces as a printed
    traceback rather than going through CPython's `handle_system_exit()`, so the
    embedded interpreter never sets the process status. This contradicts the
    long NOTE in the module docstring, and it means CI would have gone green on
    the `default`-colorspace bug `T12` just fixed. Whatever makes the failure
    propagate has to be proven by a deliberately failing run, not by reading the
    code.
  - verify: a deliberately mis-mapped write colorspace makes
    `tools/ci/local/test.sh smoke` exit non-zero and the new assertion name
    appears in the output; the fixed one exits 0.
  - size: S

- [x] M3.P1.T14 — Move the `format` gate from whole files to changed lines
  - files: `.github/workflows/checks.yml` (the `format` job)
  - approach: per `DECISIONS/2026-09-01-format-gate-changed-lines-only.md`.
    The job runs `clang-format --dry-run --Werror` over each file in the
    merge-base diff, so touching one function in a legacy file fails on the
    other thousand lines: `Engine/Settings.cpp` alone measures **~1610**
    violations at HEAD, and `M3.P1.T7` adds ~20 in the same house style
    (`foo( bar )`, which the WebKit-based `.clang-format` systematically
    rejects). Switch to `git clang-format --diff` against the merge base.
    Keep everything M10 built deliberately into this job: the bare
    `ubuntu-latest` runner with no build container, and `clang-format` pinned
    to 21.1.8 to match the container's clang — an unpinned runner-supplied
    binary makes the verdict depend on when GitHub last rolled its image.
    Note `git clang-format` needs the merge base fetched, so the checkout
    depth matters; and it exits 0 with a diff on stdout rather than failing,
    so the job has to turn a non-empty diff into a failure itself, and print
    it.
  - verify: the job fails on a deliberately mis-formatted added line and
    passes on `M3.P1.T7`'s commit, which touches `Engine/Settings.cpp`;
    proven by pushing the branch and reading the run, not by reasoning about
    the YAML.
  - size: S

- [x] M3.P1.T15 — Bring this milestone's changed C++ lines into format conformance
  - files: `Engine/Settings.cpp` (only the lines `M3.P1.T7` added or edited)
  - approach: `M3.P1.T14`'s gate correctly reports ~30 non-conformant lines in
    `57e4e80fa` — they were written in the surrounding house style
    (`foo( bar )`), which `.clang-format` rejects. Run `git clang-format`
    against the merge base with `main` and commit only what it rewrites; do
    not reformat anything the milestone did not touch, and do not widen the
    diff. The result will read as clang-format style inside functions written
    in house style; that mixing is the accepted cost of holding changed lines
    to the config rather than reformatting whole files
    (`DECISIONS/2026-09-01-format-gate-changed-lines-only.md`). Use the pinned
    `clang-format 21.1.8`, not whatever is on `PATH`, or the reformat will not
    match what CI checks.
  - verify: `git clang-format --diff <merge-base>` is empty; the `format` job
    passes on a pushed run of the branch; build, `test.sh ctest` and
    `test.sh smoke` all still green — a reformat must not change behaviour.
  - size: S

- [x] M3.P1.T8 — Fail loudly on projects saved against the old config
  - files: project load path (`.ntp`/`.ntf` deserialization), Read/Write and
    `OCIOColorSpace`/`OCIODisplay`/`OCIOLookTransform` knob handling
  - approach: per `DECISIONS/2026-08-31-aces-via-ocio-builtin-config.md`, no
    name mapping and no per-project config pinning — a project whose stored
    colorspace strings (`"Linear"`, `"sRGB"`, `"rec709"`) do not exist in the
    active ACES config must fail **loudly and actionably**, naming the
    colorspace that could not be resolved and what the user should do, rather
    than silently falling back to a default and quietly changing the picture.
    Audit what the current code does on an unresolvable colorspace first — the
    risk is that it already fails silently, in which case this task is about
    making an existing quiet failure loud, not adding a new check.
  - verify: a project file saved against the old `"blender"` default produces a
    clear, actionable error naming the unresolved colorspace, proven by a test.
  - size: M

- [x] M3.P1.T9 — Build `openfx-misc` from source in CI
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: per `DECISIONS/2026-08-31-build-openfx-misc-in-ci.md`. Mirror
    exactly what the script already does for `openfx-io`: pin a source SHA,
    build against the image's own OIIO/OCIO/OpenEXR/LibRaw rather than
    downloading a prebuilt bundle, and land the result in the same plugin
    directory the ctest cases load from. Expect to fork it if it needs fixes —
    `DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md` establishes that
    small fixes to NatronGitHub repos land on our fork and we pin the fork.
    Watch the asset-cache key: it is keyed on `hashFiles('fetch-assets.sh')`,
    so editing the script correctly invalidates it, and the first run will be
    slow.
  - verify: CI builds `openfx-misc` against `aswf/ci-vfxall:2027-clang21.1` and
    stays green; the added wall-clock is recorded in this file's `## Decisions`
    so the cost is visible.
  - size: M

- [x] M3.P1.T10 — Remove `NATRON_ENABLE_WAYLAND` and its dead detection
  - files: `CMakeLists.txt` (~line 114, `find_package(ECM NO_MODULE)` and the
    `NATRON_ENABLE_WAYLAND` option), `Engine`/`Gui` Wayland sources and any
    build wiring that references them
  - approach: per `DECISIONS/2026-08-31-drop-wayland-support.md`. The option is
    a silent no-op — ECM is absent from the image and EPEL is unreachable, and
    the `find_package` is not `REQUIRED`, so detection fails quietly and
    `OSGLContext_wayland.cpp` never activates. Remove the option and the
    detection rather than leaving a switch that misleads. Decide explicitly
    whether the Wayland source files go too or stay as dormant code, and say
    which in the decision — note the sources are also hard to build here
    (`/usr/local/include/EGL/` shadows Mesa's headers and drags in `X11/Xlib.h`,
    whose `Bool`/`Status` macros break the Qt includes), which is what made an
    earlier attempt unwinnable.
  - verify: `grep -rn "NATRON_ENABLE_WAYLAND\|find_package(ECM" .` returns
    nothing outside history; a clean CMake configure logs no failed ECM lookup;
    CI stays green.
  - size: S

- [x] M3.P1.T11 — Delete the dead Windows/macOS/qmake files
  - files: `Gui/QtMac.mm`, `Gui/TaskBarMac.mm`,
    `.github/workflows/gen_config.sh`, `tools/travis/`, `tools/jenkins/`,
    `Project-makefile.xcodeproj/`, `Project-xcode.xcodeproj/`, `Natron.spec`
  - approach: per `DECISIONS/2026-08-31-delete-dead-platform-files.md`. M0 cut
    Windows, macOS and qmake but left their artifacts tracked, and M10 then
    spent real effort making `shellcheck` pass on `gen_config.sh` — a generator
    for a build system this fork no longer has. **Confirm each path is
    genuinely unreferenced before deleting it**: grep for every filename across
    the tree, the workflows, and `CMakeLists.txt`, and remove the `lint-ci`
    reference to `gen_config.sh` in `.github/workflows/checks.yml` in the same
    change, or the gate goes red on a missing file. `Natron.spec` and the Xcode
    projects are packaging inputs — check nothing in `Documentation/` or the
    release process still points at them before removing.
  - verify: CI green; `grep -rn` for each deleted filename returns nothing
    outside `.plan/`, `docs/decisions/` and git history; `lint-ci` still passes
    with no reference to a file that no longer exists.
  - size: M

**Verification gate:** the project builds cleanly inside
`aswf/ci-vfxall:2027-clang21.1` via `CMAKE_PREFIX_PATH=/usr/local` with no
distro-package installs in CI (already true today); a from-source build with no
`OCIO` environment variable set starts with the ACES 2.0 built-in config
active; a project saved against the old `"blender"` default puts each offending
node into an error state naming the colorspace, **without blocking the load**;
`openfx-misc` builds from source in CI and the suite stays green;
`NATRON_ENABLE_WAYLAND` and its dead ECM detection are gone; and the
qmake/Windows/macOS leftovers are deleted with `lint-ci` still green.

_Gate amended 2026-09-01 on two points, both recorded below: the original
"fails loudly and actionably" was written expecting a refused load, which the
user overruled; and `tools/jenkins/` is **kept**, not deleted, because it turned
out to be live._

## Decisions

- 2026-08-31 — freshness check before promotion (PLAN-FORMAT.md §5a) found most
  of this milestone stale, but re-planned it task-by-task rather than
  re-stubbing it. `M3.P1.T1` is delivered outright; `M3.P1.T2` is answered and
  belongs as a standing rule in `DECISIONS/2026-08-29-pin-exact-aswf-tag.md`
  rather than a checkbox; `M3.P1.T3` and `M3.P1.T4` are real but **larger** than
  scoped, so closing the milestone as delivered would have dropped genuine
  work. Evidence: `build.sh:158` passes `-DCMAKE_PREFIX_PATH=/usr/local`, and
  CI run 33452464226 on `main` shows FreeType and Expat resolving from
  `/usr/local`. Original T1-T4 are kept as historical record; new work carries
  IDs from T5 on, never reusing an ID.

- 2026-08-31 — the OCIO work splits into three tasks, not one, because the
  original T3 hid two prerequisites. There is no install path that puts an OCIO
  config where `Settings.cpp` looks for it (T6), and every saved project stores
  colorspaces as bare strings that an ACES config renames (T8). "Swap the
  default and flag it in release notes" would have produced a default nothing
  could load and a silent break in every existing project.

- 2026-08-31 — the four open questions this milestone hung on were put to the
  user and answered: ACES 2.0 via OCIO's built-in `ocio://` configs; existing
  projects break loudly rather than being migrated or pinned; `openfx-misc` is
  built from source in CI despite no test needing it; `NATRON_ENABLE_WAYLAND`
  is removed rather than left as a silent no-op; and the dead platform-file
  sweep folds in here as `M3.P1.T11` rather than getting its own milestone.
  Recorded as four project-wide decisions (§3a). The built-in-config choice
  dissolved `M3.P1.T6`'s original subject — there is no config directory to
  install if the config is a URI — so T6 was rewritten to verify the premise
  that choice rests on, which is now the one thing that could still overturn
  it.

- 2026-08-31 — **`M3.P1.T6`: the premise holds.** Verified against the library
  in `aswf/ci-vfxall:2027-clang21.1`, not against documentation
  (`/usr/local/bin/python3 -c "import PyOpenColorIO ..."`; note `/usr/bin/python3`
  is 3.9 and has no bindings). OpenColorIO reports **2.5.2**, and its built-in
  registry publishes **8** configs, of which exactly two are ACES 2.0:

  - `cg-config-v4.0.0_aces-v2.0_ocio-v2.5` — recommended, **and the default**
  - `studio-config-v4.0.0_aces-v2.0_ocio-v2.5` — recommended, not default

  The other six carry `aces-v1.3`. The identifiers embed three independent
  version numbers and must not be conflated: `v4.0.0` is the config family's own
  colorspace-set version, `aces-v2.0` is the ACES spec version, `ocio-v2.5` is
  the minimum library version. There is no `v3.x` family, so config version and
  ACES version do not increment in step. `ocio://default` resolves to the CG
  config today, confirmed by loading it and comparing names rather than by
  trusting the registry's boolean.

- 2026-09-01 — **`M3.P1.T12` landed as two commits on
  `charlesangus/openfx-io`, fast-forwarded onto the fork's `master`** so the
  pinned SHA stays on the mainline and the fork remains "N commits ahead, zero
  behind": `60c0275` (the `-1` sentinel guard, `+7/-0`, self-contained) and
  `40764b2` (the rest, `+37/-3`) — one file, `IOSupport/GenericOCIO.cpp`.
  Measured, not argued: on the old `blender` config the patched bundle's renders
  are md5-identical to the stock bundle's in both PNG→PNG and EXR→PNG, and the
  resolved parameter defaults are unchanged; on the ACES built-in the stock
  bundle reproduces `Color space 'default' could not be found.` and the patched
  one renders, resolving the writer to `sRGB - Display`; scene-linear 0.18
  through EXR→PNG lands on **118/255** on both configs, with bit-identical
  decoded pixel arrays.

- 2026-09-01 — **`M3.P1.T11`: `tools/jenkins/` is kept, against
  `DECISIONS/2026-08-31-delete-dead-platform-files.md`, because it is not
  dead.** `tools/docker/natron-sdk/build.sh` and
  `tools/docker/natron-sdk-rockylinux8/build.sh` both `cp ../../jenkins/*.sh .`
  and run `include/scripts/build-Linux-sdk.sh`, and `INSTALL_LINUX.md` — the
  install guide for this fork's only supported platform — instructs running it
  directly. Deleting it would have broken the documented SDK build. Near-miss
  worth recording: `tools/jenkins/build-Linux-installer.sh` does reference a
  `Natron.spec`, but its own bundled copy under `include/natron/`, not the root
  file, which is why deleting the root one is safe. **`tools/jenkins/` is
  arguably dead in spirit** — the ASWF image replaced the custom SDK — but
  retiring it means updating `INSTALL_LINUX.md` and `tools/docker/`, which is
  M6's subject, not a file sweep's.

  Also deleted beyond the original list: `.travis.yml.disabled`, whose entire
  content was four references to the `tools/travis/` scripts this task removed.

- 2026-09-01 — **`M3.P1.T9`: `openfx-misc` needs no fork, and costs ~9 minutes.**
  Unlike `openfx-io` it has no dependency on OIIO/OCIO/SeExpr and no
  `CMakeLists.txt` bug, so upstream `NatronGitHub/openfx-misc` builds clean
  against the container as-is and is pinned directly at
  `0abd46b5a8cbc98fa24579042129460d0aa87b8f`. The one genuine trap: its CMake
  build **globs `CImg/CImg.h` and `CImg/Inpaint/inpaint.h` as sources but never
  fetches them** — only upstream's *Makefile* build does, via `curl` plus a
  `patch`. So `cmake --build` links `Misc.ofx` (which doesn't need them) and
  then dies partway through `CImg.ofx`. `fetch-assets.sh` replicates those two
  fetches and the patch, reading the CImg commit out of `openfx-misc`'s own
  `CImg/Makefile` rather than pinning it a second time, so it cannot drift when
  the `openfx-misc` pin is bumped. Measured cold, from the PM's own timed run:
  `openfx-io` **2m35s**, `openfx-misc` **8m59s**, plugins total **11m34s**. The
  asset cache is keyed on `hashFiles('fetch-assets.sh')`, so that is paid when
  the script changes, not per run. ctest 30/30 and smoke green with all three
  bundles on `OFX_PLUGIN_PATH`.

- 2026-09-01 — process note, not a code decision: T9's implementer parked three
  times on background builds that were reaped when its turn ended, losing its
  own work each time. The PM took the long-running verification over directly,
  running the build detached *inside* the container (`docker exec -d`) with a
  persistent monitor watching for either an exit marker or the disappearance of
  the build processes. **For work in this repo that takes more than a few
  minutes, drive the build from the coordinator, not from a subagent.**

- 2026-09-01 — **`M3.P1.T8` uncovered that `M3.P1.T7` had broken every saved
  project, not only old ones.** `OfxStringInstance::projectEnvVar_getProxy` runs
  every OFX file-path parameter through `Project::canonicalizePath`, and
  `Project::isRelative()` returns true for anything not starting with `/` — so
  the new `ocio://…` default was treated as a relative path and the project
  directory was prepended, giving `Invalid OCIO config. file
  "/path/to/project/ocio://studio-config-…"`. A valid project and one carrying
  old colorspaces were indistinguishable, and both exited 0. The smoke test
  could not see it because it never saves a project, so `[OCIO]` stays empty and
  `canonicalizePath` short-circuits. Fixed separately in `2b22c7d94` so a bisect
  finds it on its own. **Standing lesson: T7's verification never saved and
  reloaded a project — "it starts with the right config" and "a project round-trips"
  are different claims.**

- 2026-09-01 — **`M3.P1.T8`: the user overrode the task's own brief on how
  loudly to break.** The brief and
  `DECISIONS/2026-08-31-aces-via-ocio-builtin-config.md` were read as licensing a
  refused load; the first implementation threw out of `loadProjectInternal()`.
  Asked, the user was unambiguous: "Nodes with bad parameters should error out.
  We should never prevent loading. Rendering prevention is only by virtue of
  error nodes in the graph. We should not do anything other than make a noise
  with a bad param enter an error state." Reworked to set a per-node persistent
  error and nothing else. The reasoning that settles it: a project the user
  cannot open is a project they cannot repair, and the refused-load message's own
  advice — fix the parameters and re-save — was unfollowable in the GUI.

- 2026-09-01 — the `openfx-io` fallback from `M3.P1.T12` and `M3.P1.T8`'s loud
  failure do **not** conflict, established by experiment rather than by reading.
  `existingColorSpaceOrFallback()` is reachable only from
  `describeInContextInput`/`describeInContextOutput`, which feed
  `StringParamDescriptor` *defaults*; it is absent from `createInstance`,
  `changedParam`, `getInputColorspace*` and every render path. A project storing
  `ocioInputSpace = "Linear"` reads back that exact string from the live knob,
  not `scene_linear`. The `KnobChoice` mirrors never enter it at all — they carry
  `setIsPersistent(false)` and are absent from a saved `.ntp`.

- 2026-09-01 — three pre-existing render-failure signalling gaps were measured
  during `M3.P1.T8` and deliberately **not** fixed here, per the user's
  instruction above: a persistent error message does not stop a render,
  `setPersistentMessage` stores nothing in background mode, and a failed render
  exits 0. Recorded with full evidence in
  `DECISIONS/2026-09-01-render-failure-signalling-gaps.md` and routed to M5.

- 2026-09-01 — **`M3.P1.T14`: "tune `.clang-format` to describe the house
  style" is measurably dead, not merely unattractive.** It was the runner-up
  when the gate question was put to the user, on the theory that Natron's
  `foo( bar )` spacing is WebKit plus spaces in parens. Measured against the
  pinned `clang-format 21.1.8`, adding `SpacesInParens: Custom` with
  `Other: true` makes things **worse**, not better: `Engine/Settings.cpp` goes
  from 1630 violations to 4120, `Engine/AppManager.cpp` from 971 to 2736,
  `Gui/Gui.cpp` from 258 to 753. The house spacing is not applied consistently
  enough for any config to match it, so no amount of tuning gets whole-file
  checking to green. Don't revisit this without new evidence.

- 2026-09-01 — the changed-lines gate is working as intended and its first
  verdict is against **our own** new code: `57e4e80fa`'s ~30 added lines are in
  house style and fail. That is the accepted cost of the choice, not a defect —
  new lines conform to the config, so C++ this fork adds will read as
  clang-format style inside functions that don't. `M3.P1.T15` applies it.

- 2026-09-01 — **`M3.P1.T7` landed from a draft the crashed session left
  uncommitted, reviewed rather than replayed.** The mechanism was sound and
  kept; four things were wrong. `find_package(OpenColorIO 2.2)` was the wrong
  floor — 2.2 is when built-in configs appeared, but this specific config only
  exists in the 2.5 registry, so a 2.2 build would compile and then fail on its
  own default; raised to 2.5. An `#if OCIO_VERSION_HEX >= 0x02020000 / #else`
  branch was unreachable under a `REQUIRED` `find_package` for the same version;
  deleted, with a `catch (...)` added instead. `int defaultIndex = 0` was left
  mutable after its loop assignment was deleted, reading as if the loop still
  set it. And `if (_ocioConfigKnob->getNumEntries() == 1)` became dead once the
  built-in guaranteed a second entry. Verified independently of the implementer:
  build, ctest 28/28, and smoke all green, with the smoke run reporting
  `active OCIO config: 'studio-config-v4.0.0_aces-v2.0_ocio-v2.5'` and 118/255.

- 2026-09-01 — two consequences of `M3.P1.T7` worth carrying forward.
  `NatronEngine` now links `OpenColorIO::OpenColorIO` and `find_package(
  OpenColorIO 2.5 REQUIRED)` is mandatory for **any** Natron build, not just the
  plugins — kept deliberately, because it turns "the chosen default silently
  doesn't exist" into a build-time error plus a clear runtime dialog. And
  `doOCIOStartupCheckIfNeeded()` (GUI only, on by default) will prompt every
  upgrading user whose saved selection is `blender` with "…is not the default
  one (ACES 2.0 Studio (built-in)), would you like to set it to the default
  config?". That is the knob's designed behaviour for a changed default, not a
  bug, but it is a first-launch prompt for every existing user and belongs in
  the release notes.

- 2026-09-01 — pre-existing bug found while reviewing `M3.P1.T7`, deliberately
  left alone as out of scope: the `Q_FOREACH` over `getDefaultOcioConfigPaths()`
  in `tryLoadOpenColorIOConfig()` has no `break`, so the **last** existing
  config directory wins rather than the first — inconsistent with the knob
  population loop, which does `break`.

- 2026-09-01 — **`M3.P1.T13`: the smoke test's exit-code bug had a specific
  cause, worth remembering because it will recur.**
  `AppInstance::loadPythonScript()` picks between executing a script directly
  and importing it as a module with a plain `content.contains("def
  createInstance")` search over the file's **text, comments included** — and
  `smoke_test.py`'s own docstring spelled that name out while explaining why it
  must not define it. On the import branch,
  `NATRON_PYTHON_NAMESPACE::interpretPythonScript()` calls `PyErr_Fetch()` to
  format the pending exception into a `std::string`, which *clears the error
  indicator*, so `PyErr_Print()` → `handle_system_exit()` → `Py_Exit()` never
  runs and `sys.exit()` is inert. Natron then rendered the project's write nodes
  and the process status described that instead. Measured across all three
  branches; the fix exits through `os._exit()` so the status no longer depends
  on which branch is taken, or on what a future editor writes in a comment.
  Verified in both directions independently of the implementer: clean run exits
  0 with the 118/255 assertion executing, injected failure exits 1.

- 2026-09-01 — a refinement to the "the `blender` config works by luck" framing
  in `DECISIONS/2026-09-01-fix-openfx-io-colorspace-sentinel.md`: `blender`
  defines no `default` role either, so it *does* trip the `-1 == -1` collision —
  for any name it does not know. It escapes only because every default the
  shipped readers and writers actually pass (`sRGB`, `scene_linear`,
  `reference`) resolves in it. The luck is in the names, not solely in
  `reference` sitting at index 0. Doesn't change the fix; does mean the bug was
  never `blender`-proof, only unexercised.

- 2026-08-31 — **`M3.P1.T8`'s break is confirmed, not hypothetical.**
  `cfg.getColorSpace()` returns null for `"Linear"`, `"sRGB"`, `"rec709"`,
  `"Rec709"` and `"Rec.709"` in **both** ACES 2.0 configs, and dumping every
  colorspace's alias list found none of those bare tokens. There is no silent
  alias fallback to rely on. Nearest equivalents, none of them exact:
  `Linear Rec.709 (sRGB)` for `"Linear"`; `sRGB Encoded Rec.709 (sRGB)` (a
  texture space) for `"sRGB"` — not to be confused with `sRGB - Display`, which
  is a display role; and for `"rec709"`, `Camera Rec.709`, which exists **only
  in the Studio config**. The CG config has no camera-referred Rec.709 at all,
  only display-oriented curves.
