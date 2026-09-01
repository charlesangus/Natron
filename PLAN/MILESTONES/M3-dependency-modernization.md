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

- [ ] M3.P1.T7 — Make the default OCIO config an `ocio://` built-in
  - files: `Engine/Settings.cpp` (`NATRON_DEFAULT_OCIO_CONFIG_NAME` ~line 70,
    `getDefaultOcioConfigPaths()` ~line 91), `tools/ci/local/test.sh`
  - approach: the default is currently the bare string `"blender"`, matched by
    *directory name* against configs found on disk. A built-in config is a URI,
    not a directory, so this is not a one-line string swap — the resolution
    path has to accept a URI and hand it to OCIO directly, while still allowing
    a user-supplied on-disk config to win. Keep the existing on-disk search as
    a fallback so a user pointing `$OCIO` or dropping in a config still works.
    `test.sh` currently pins `OCIO=.../OpenColorIO-Configs/blender/config.ocio`
    from the fetched tarball; decide whether CI should keep testing against the
    tarball or move to the built-in, and make that deliberate.
  - verify: a from-source build with no `OCIO` environment variable set and no
    config directory installed starts with the ACES 2.0 built-in active; a
    build with `$OCIO` pointed at an on-disk config still honours it.
  - size: M

- [ ] M3.P1.T8 — Fail loudly on projects saved against the old config
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

- [ ] M3.P1.T9 — Build `openfx-misc` from source in CI
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

- [ ] M3.P1.T10 — Remove `NATRON_ENABLE_WAYLAND` and its dead detection
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

- [ ] M3.P1.T11 — Delete the dead Windows/macOS/qmake files
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
active; a project saved against the old `"blender"` default fails loudly and
actionably rather than silently; `openfx-misc` builds from source in CI and the
suite stays green; `NATRON_ENABLE_WAYLAND` and its dead ECM detection are gone;
and the qmake/Windows/macOS leftovers are deleted with `lint-ci` still green.

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
