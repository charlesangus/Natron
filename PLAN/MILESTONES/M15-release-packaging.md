# Milestone 15: Release packaging — portable tarball and AppImage

Produce two release artifacts from a tag: `Natron-<version>-linux-x86_64.tar.xz`
and `Natron-<version>-x86_64.AppImage`, both built from one staged tree, both
carrying `Natron`, `NatronRenderer`, `natron-python`, the four OFX bundles, the
OCIO configs and the runtime library closure. Rationale and the rejected
alternatives are in `DECISIONS/2026-09-03-package-as-tarball-and-appimage.md`,
which supersedes `2026-08-29-defer-packaging-decision.md`.

**There is no packaging in this fork today, and `cmake --install` does not
produce a runnable tree.** The install rules cover three binaries plus desktop
metadata (`App/CMakeLists.txt:48-58`, `Renderer/CMakeLists.txt:37`,
`PythonBin/CMakeLists.txt:42`) and nothing else — no OFX bundles, no OCIO
configs, no fontconfig tree, no Python runtime. Phase 15.1 is therefore not
packaging polish; it is the prerequisite that makes any generator viable.
M12 deleted upstream's entire pipeline in `576c20782`; recover any of it with
`git show 576c20782^:<path>`, but treat it as reference, not a base — it was
Jenkins-shaped and assumed a bespoke centos6 SDK.

**The bundle layout is not a choice — the code already hardcodes it.** Every
path below was read out of the tree, and the staged prefix must satisfy all of
them:

| Path, relative to the prefix | Consumer |
|---|---|
| `bin/{Natron,NatronRenderer,natron-python}` | `GNUInstallDirs` default `CMAKE_INSTALL_BINDIR` |
| `Plugins/OFX/Natron/*.ofx.bundle` | `Engine/OfxHost.cpp:892` — `applicationDirPath()/Plugins/OFX/Natron` |
| `Plugins/` | `Global/PythonUtils.cpp:107` — put on `PYTHONPATH` as `binPath + "/../Plugins"` |
| `Resources/OpenColorIO-Configs/` | `Engine/Settings.cpp:110` |
| `Resources/etc/fonts/` | `Engine/AppManager.cpp:337` — sets `FONTCONFIG_PATH` |
| `lib/python3.X/{,lib-dynload,site-packages}` | `Global/PythonUtils.cpp:99-106` — `pythonHome = binPath + "/.."` |

Two traps in that table. `Settings.cpp:108-110` tries `/usr/share/OpenColorIO-Configs`
and `<prefix>/share/OpenColorIO-Configs` **before** the `Resources/` path, so a
stale system config wins over the bundled one — the smoke test's existing
`unset OCIO` discipline (`DECISIONS/2026-09-01-ci-tests-the-shipped-ocio-default.md`)
is the pattern to keep. And `AppManager.cpp:1708` hardcodes an absolute
`/usr/share/Natron/Plugins` as the global PyPlug path, which no relocatable
artifact can satisfy; it is additive, so it degrades to "not found" rather than
failing, but it must not become the only PyPlug path.

**Nothing needs staging for the bindings.** `NatronEngine` and `NatronGui` are
static libraries baked into the executables — there is no importable
`NatronEngine.so` (`tools/ci/smoke_test.py:21-25`). PySide6 and shiboken6 *are*
runtime dependencies of the embedded interpreter, and the smoke test is exactly
the thing that proves they resolve.

Sequencing: this milestone wants M11's render-through-plugins coverage ahead of
it, since a release artifact that loads plugins but renders wrong is worse than
none. It is not a hard block — M11 is `rescope` — but do not tag a release off
this milestone alone.

## Phase 15.1: Make `cmake --install` produce a runnable tree

- [x] M15.P1.T1 — Install the OFX bundles and OCIO configs into the bundle layout
  - files: `CMakeLists.txt`, `cmake/NatronBundleAssets.cmake` (new)
  - approach: add a `NATRON_BUNDLE_ASSETS` option, default `OFF`, that installs
    `build/assets/Plugins/*.ofx.bundle` to `Plugins/OFX/Natron/` and
    `build/assets/OpenColorIO-Configs/` to `Resources/OpenColorIO-Configs/`.
    Default `OFF` so a developer's `cmake --install` without having run
    `fetch-assets.sh` still succeeds; the release path turns it `ON` and the
    configure step hard-errors if the asset dirs are absent, so a silently
    asset-less release is impossible. Copy with `USE_SOURCE_PERMISSIONS` — the
    `.ofx` binaries must stay executable, and the bundles carry their own
    `Libraries/` with `RUNPATH $ORIGIN/../../Libraries`
    (`M13-full-ofx-plugin-set.md` `## Decisions`, 2026-09-02), so they are
    already self-contained. Do not rewrite their RPATHs.
  - verify: `cmake --install build/release --prefix /tmp/p -DNATRON_BUNDLE_ASSETS=ON`
    yields four bundles under `/tmp/p/Plugins/OFX/Natron/`; `ldd` on each `.ofx`
    reports nothing `not found` with `LD_LIBRARY_PATH` cleared.
  - size: M

- [x] M15.P1.T2 — Install the fontconfig tree and the PyPlugs
  - files: `cmake/NatronBundleAssets.cmake`, `Gui/CMakeLists.txt`
  - approach: install `Gui/Resources/etc/fonts` to `Resources/etc/fonts` and
    `Gui/Resources/PyPlugs` to `Plugins/`. `Fonts/` and `Stylesheets/` need no
    install rule — they are compiled into `Gui/GuiResources.qrc` and reached via
    the `:/Resources/` prefix (`Engine/Plugin.h:274`); only the fontconfig
    *configuration* is read from disk. Record in a comment that
    `AppManager.cpp:1708`'s absolute `/usr/share/Natron/Plugins` is deliberately
    left unsatisfied by a relocatable prefix.
  - verify: a staged prefix has `Resources/etc/fonts/fonts.conf`; running
    `bin/NatronRenderer` from it emits no "Fontconfig configuration file ...
    does not exist" warning (`AppManager.cpp:341`).
  - size: S

- [ ] M15.P1.T3 — Stage the Python runtime and PySide6 under the prefix
  - files: `cmake/NatronBundleAssets.cmake`
  - approach: copy the interpreter's `lib/python3.X/` — including `lib-dynload/`
    and a `site-packages/` carrying PySide6 and shiboken6 — under the prefix, so
    `PythonUtils.cpp`'s `pythonHome = binPath + "/.."` resolves inside the
    bundle. Derive the source from `Python3::Python`'s resolved paths rather
    than hardcoding a container path. Prune what is dead weight in a release
    (`test/`, `idlelib/`, `tkinter/`, `__pycache__`) but keep `encodings/` and
    `lib-dynload/`, without which the interpreter will not initialise at all.
  - verify: with `PYTHONHOME` and `PYTHONPATH` unset and the container's own
    Python unreachable, `bin/NatronRenderer tools/ci/smoke_test.py` passes from
    the staged prefix — the same assertion `test.sh smoke` already makes, now
    against the bundle's interpreter.
  - size: M

**Verification gate:** `cmake --install` with `NATRON_BUNDLE_ASSETS=ON` yields a
prefix satisfying every row of the layout table above, and `test.sh ctest
release` is unchanged at 28/28.

## Phase 15.2: Stage the runtime closure and emit the tarball

- [ ] M15.P2.T1 — Write `tools/release/stage-bundle.sh`
  - files: `tools/release/stage-bundle.sh` (new), `tools/release/excludelist.txt` (new)
  - approach: the one thing CMake cannot do — lift the `/usr/local` dependency
    closure out of the build container. `cmake --install` into a staging dir,
    then walk `ldd` transitively over `bin/*` and every `.ofx`, copy each
    non-baseline `.so` into `lib/`, and set `RUNPATH $ORIGIN/../lib` on the
    binaries with `patchelf`. Skip anything already inside a bundle's own
    `Libraries/` — those are solved. `excludelist.txt` holds the libraries that
    must come from the host, not the bundle: glibc and friends, and critically
    `libGL/libGLX/libEGL/libX11` and the Qt XCB platform plugin's transitive X
    libraries, since bundling a GL stack over the host driver is the classic way
    to break every machine with a real GPU. Start from the AppImage project's
    published excludelist rather than inventing one. Qt also needs its
    `plugins/platforms/` and `plugins/imageformats/` copied and a `qt.conf`
    beside the binary — Qt will not find them from `RUNPATH` alone.
  - verify: `objdump -p` on the two binaries shows the expected direct closure
    (measured on the current tree: `Natron` 27 `DT_NEEDED` entries,
    `NatronRenderer` 21, the delta being exactly `libQt6Gui`, `libQt6Widgets`,
    `libQt6OpenGL`, `libQt6OpenGLWidgets`, `libGLX`, `libOpenGL`); `ldd` on
    everything under `bin/` and `Plugins/` reports nothing `not found` with the
    environment scrubbed.
  - size: L

- [ ] M15.P2.T2 — Emit the portable tarball
  - files: `tools/release/stage-bundle.sh`, `tools/release/make-tarball.sh` (new)
  - approach: `tar Jcf` the staged tree with a single top-level
    `Natron-<version>-linux-x86_64/` directory, version read from
    `CMakeLists.txt:23`'s `PROJECT_VERSION` rather than duplicated. Emit a
    `sha256sum` beside it. No installer, no postinst — extract and run is the
    contract, and it is what makes this the farm artifact.
  - verify: extract to a path unrelated to the build dir, `env -i` scrub, then
    `bin/NatronRenderer tools/ci/smoke_test.py` passes and
    `bin/Natron --version` prints `2.6.0`. Relocation is the property under
    test: the same tarball must work from two different extraction paths.
  - size: M

**Verification gate:** a tarball extracted on a machine that never built Natron,
with no `LD_LIBRARY_PATH`, `PYTHONHOME`, `OCIO` or `OFX_PLUGIN_PATH` set, runs
the smoke test green and loads all four OFX bundles.

## Phase 15.3: AppImage

- [ ] M15.P3.T1 — Build the AppDir and the AppImage from the staged tree
  - files: `tools/release/make-appimage.sh` (new)
  - approach: consume Phase 15.2's staged tree — do not re-derive it. Add
    `AppRun`, and the top-level `.desktop` and icon the AppImage runtime
    requires, plus `usr/share/metainfo/`. `git show
    576c20782^:tools/appimage/make-appimage.sh` is the shape to follow, with one
    correction: it copied icons from `Resources/pixmaps/`, which this tree does
    not produce — the desktop file, appdata and icons now come from the install
    rules at `App/CMakeLists.txt:49-58`. Run `appimagetool` from a pinned
    release, not `latest`. Note in the script header that the runtime needs
    FUSE2 and that `--appimage-extract-and-run` is the documented fallback.
  - verify: the AppImage runs from a directory with no execute bit on anything
    else, prints `2.6.0`, and passes the smoke test via
    `--appimage-extract-and-run` on a FUSE-less host.
  - size: M

- [ ] M15.P3.T2 — Refresh the AppStream metadata
  - files: `Gui/Resources/Metainfo/fr.natron.Natron.appdata.xml`
  - approach: the file is stale and partly malformed — its newest `<release>` is
    2.3.12 (2018), the homepage URL is missing its colon
    (`https//natrongithub.github.io`), and the bugtracker points at
    `NatronGitHub/Natron` rather than this fork. Add a `2.6.0` release entry,
    fix both URLs, and keep the existing `project_license` (`GPL-2.0-or-later`,
    which matches `LICENSE.txt`). This is load-bearing for the AppImage's
    desktop integration and for any later Flathub route, not cosmetic.
  - verify: `appstreamcli validate` passes with no errors; the AppImage shows a
    correct name, icon and summary when installed to `~/Applications`.
  - size: S

**Verification gate:** the AppImage launches on a distro that is not the build
container, resolves its own OCIO config and OFX bundles, and validates clean
under `appstreamcli`.

## Phase 15.4: Release workflow and user-facing docs

- [ ] M15.P4.T1 — Add `release.yml`, triggered on tag
  - files: `.github/workflows/release.yml` (new)
  - approach: mirror `nightly.yml`'s container, ccache and asset-cache setup —
    it is the only workflow that already does a release build — then run the
    Phase 15.2 and 15.3 scripts and attach both artifacts plus their checksums
    to a GitHub Release. Trigger on `push: tags: ['v*']` plus
    `workflow_dispatch`. `concurrency` with `cancel-in-progress: false`, for the
    same reason `nightly.yml` documents: a cancelled run loses the artifact the
    workflow exists to produce. `permissions: contents: write` — the narrowest
    grant that can create a release, and a deliberate widening from the
    `contents: read` every other workflow uses, which the file should say out
    loud. Not a merge gate and not wired into branch protection.
  - verify: `workflow_dispatch` on a throwaway tag produces both artifacts;
    `actionlint` passes (the `lint-ci` job in `checks.yml` will enforce this
    anyway).
  - size: M

- [ ] M15.P4.T2 — Document what each artifact is for and what it will not do
  - files: `README.md`, `INSTALL_LINUX.md`
  - approach: state which artifact to take (AppImage for a workstation, tarball
    for a farm or shared storage), the glibc floor and what it excludes (Ubuntu
    20.04), the FUSE2 requirement and its fallback, and the headless GL caveat
    below — a farm node with no X server gets CPU-only rendering. Also state
    that `NatronRenderer` is the binary a farm should invoke, per this
    milestone's `## Decisions`. Per
    `DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`, the test is
    that this does not describe capability the artifacts lack.
  - verify: every claim is checkable against an artifact produced by
    `release.yml`; no instruction references a deleted upstream installer.
  - size: S

**Verification gate:** a tag produces both artifacts through `release.yml` with
no manual step, and both run on a machine that is not the build container.

## Decisions

- 2026-09-03 — **The headless OpenGL gap is out of scope, and it is not a
  packaging problem.** `AppManager::loadFromArgs` calls
  `initializeOpenGLFunctionsOnce(true)` unconditionally
  (`Engine/AppManager.cpp:321`) with no background-mode check; on Linux that
  reaches `XOpenDisplay(NULL)` (`Engine/OSGLContext_x11.cpp:283`) and throws
  `"GLX: No DISPLAY available"` (line 451). The throw is caught at line 830, but
  `if (!glContext) return false;` at line 844 bails *before* `_imp->initGl()` at
  line 850, and the caller ignores the return — so a headless `NatronRenderer`
  comes up with GL permanently unavailable. This follows the binary into the
  tarball and the AppImage identically; containerising would not have helped.
  The fix is an EGL-surfaceless or OSMesa path in `OSGLContext`, which would
  also un-skip the two `DISABLED_` tests from
  `DECISIONS/2026-09-02-no-glx-under-xvfb.md`. That is a code milestone, and it
  is worth its own — packaging must not absorb it.

- 2026-09-03 — **`Natron -b` still exits 0 on a failed render; `NatronRenderer`
  does not.** M5 (`6be6a49cf`) added the `manager.hasRenderFailed()` check to
  `Renderer/NatronRenderer_main.cpp:112-118` only; the background branch in
  `App/NatronApp_main.cpp:147-159` returns 0 whenever `load()` succeeded, and
  nothing else in the tree calls `hasRenderFailed`. This is the residual half of
  `DECISIONS/2026-09-01-render-failure-signalling-gaps.md`. It is why
  `M15.P4.T2` documents `NatronRenderer` as the binary a farm should invoke, and
  it should be fixed rather than only documented — but not here.

- 2026-09-03 — **No `NatronRenderer`-only container image**, though it was in
  the first sketch of this milestone. The renderer installs beside the GUI under
  the same rules, resolves resources through the same `applicationDirPath()`
  contract, and differs from `Natron` by six `DT_NEEDED` entries. Splitting it
  out would buy a small size reduction and cost a second staging path, a second
  artifact to test, and version skew between artist preview and farm render.
  Full reasoning in `DECISIONS/2026-09-03-package-as-tarball-and-appimage.md`.

**Verification gate:** a tag on `main` produces
`Natron-2.6.0-linux-x86_64.tar.xz` and `Natron-2.6.0-x86_64.AppImage` with
checksums, through `release.yml`, with no manual step; each artifact runs on a
host that never built Natron, with a scrubbed environment, loading all four OFX
bundles and its own OCIO config; and `README.md`/`INSTALL_LINUX.md` describe
only what those artifacts actually do.
