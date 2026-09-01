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

- [ ] M3.P1.T6 — Give the CMake build an installed, discoverable OCIO config
  - files: `CMakeLists.txt`, install rules; `Engine/Settings.cpp` (read only —
    `getDefaultOcioConfigPaths()` at ~line 91 defines the search paths)
  - approach: **prerequisite for T7, and the reason T7 is bigger than it
    looks.** Nothing in the CMake-only build installs an
    `OpenColorIO-Configs` directory anywhere `Settings.cpp` searches. The only
    code that ever did is `tools/jenkins/build-Linux-installer.sh`, a
    qmake/Jenkins-era script no workflow invokes. So a from-source build today
    has no runtime-discoverable OCIO config at all unless `OCIO` is pointed by
    hand at the CI test tarball, which is what `tools/ci/local/test.sh` does.
    Establish the install path first; changing the default config name before
    any config is installed changes nothing observable.
  - verify: a fresh `cmake --install` produces a tree where Natron starts and
    resolves its default OCIO config with no `OCIO` environment variable set.
  - size: M

- [ ] M3.P1.T7 — Move the default OCIO config to ACES 2.0
  - files: `Engine/Settings.cpp` (`NATRON_DEFAULT_OCIO_CONFIG_NAME`, ~line 70),
    `tools/ci/local/fetch-assets.sh`, whatever T6 establishes
  - approach: **blocked on an answered product question — see the board's
    `# Open questions`.** Two implementations differ in kind, not degree:
    OpenColorIO 2.5.2 (which the image ships) has **built-in configs**
    addressable as `ocio://` URIs with no tarball or download at all; or keep
    fetching an external config repo and point it at an ACES 2.0 set. The
    current default is the string `"blender"`, matched by directory name
    against the 2018-era `NatronGitHub/OpenColorIO-Configs` tag `Natron-v2.5`.
    Note `OCIO_CONFIG_VERSION=2.5` in the workflows is that tarball's *tag*,
    not an OCIO library version — easy to misread as already ACES-adjacent.
  - verify: a fresh install opens with the chosen ACES config active and no
    environment variable set; release notes record the change.
  - size: M

- [ ] M3.P1.T8 — Handle colorspace-name migration for existing projects
  - files: project load path (`.ntp`/`.ntf` deserialization), Read/Write and
    `OCIOColorSpace`/`OCIODisplay`/`OCIOLookTransform` knob handling
  - approach: **the correctness risk T3 described only as "flag it in release
    notes."** Every colorspace is stored as a plain string (`"Linear"`,
    `"sRGB"`, `"rec709"`) and matched against the active config's colorspace
    names at load. ACES-family configs name them differently
    (`ACES2065-1`, `ACEScg`, `sRGB - Display`), so changing the default
    silently breaks colorspace resolution in every previously-saved project,
    not just new ones. Decide and implement: a name-mapping table applied on
    load, pinning a project's config at save time, or an explicit documented
    break. Sequenced after T7 because the target config's naming decides the
    mapping.
  - verify: a project saved against the old default loads with its colorspaces
    resolved (or fails loudly with an actionable message), proven by a test.
  - size: L

- [ ] M3.P1.T9 — Settle `openfx-misc`
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: **the half of `M3.P1.T4` that is genuinely open.** `openfx-io` is
    thoroughly settled — built from a pinned fork SHA against the image's own
    OIIO/OCIO/OpenEXR/LibRaw, with 28/28 ctest cases green. `openfx-misc` has
    never been fetched, built, or tested anywhere in this pipeline; it appears
    only in legacy `tools/jenkins/` scripts. Nothing in the current test
    surface needs it — `BaseTest`'s three plugin IDs (`ReadOIIO`, `WriteOIIO`,
    `SeNoise`) are satisfied by `openfx-io` plus `SeExpr`, and `SeNoise` lives
    in `openfx-io`, not `openfx-misc` as an earlier decision wrongly claimed.
    So this is a scope question before it is a build question: either build it
    from source the way `openfx-io` is, or record a decision closing it.
  - verify: either `openfx-misc` builds in CI against
    `aswf/ci-vfxall:2027-clang21.1`, or a decision file explains its exclusion.
  - size: M

- [ ] M3.P1.T10 — Resolve the `extra-cmake-modules` / Wayland gap
  - files: `CMakeLists.txt` (~line 114, `find_package(ECM NO_MODULE)`)
  - approach: not in the original M3 at all, but it is dependency
    modernization. `NATRON_ENABLE_WAYLAND` exists as an option, yet ECM is not
    in the image and EPEL is unreachable from the build environment, so the
    `find_package` quietly fails and Wayland support never activates. M7
    prototyped a from-source ECM build and confirmed it works, but chose not to
    carry it. A silent no-op option is the worst of both: either vendor the
    from-source ECM build, or close the option as unsupported on this image and
    say so where someone would look.
  - verify: either Wayland support demonstrably activates in the container, or
    the option is gone/documented as unsupported and no silent `find_package`
    failure remains.
  - size: M

**Verification gate:** the project builds cleanly inside
`aswf/ci-vfxall:2027-clang21.1` via `CMAKE_PREFIX_PATH=/usr/local` with no
distro-package installs in CI (already true today); an installed build resolves
its default OCIO config with no environment variable set, and that config is
ACES 2.0; existing project files have a tested colorspace-name migration path
or a loud, documented failure; `openfx-misc` either builds against the same
image or has a recorded exclusion; and `NATRON_ENABLE_WAYLAND` is no longer a
silent no-op.

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
