# Milestone 9: Drop the vendored OFX plugin dependency

> **CANCELLED, 2026-08-31 — do not execute.** The premise did not survive
> contact with a different container. See
> `PLAN/DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md`.
>
> The plugins now build from pinned source against `aswf/ci-vfxall`'s own
> OIIO/OCIO/OpenEXR, and the suite is 28/28 green with `BaseTest` intact —
> so there is nothing here worth cutting. Two specific claims below are
> **wrong** and must not be carried forward:
>
> - `SeNoise` is **not** in openfx-misc. It is `openfx-io/SeExpr/SeNoise.cpp`,
>   in the same bundle as ReadOIIO/WriteOIIO, and the bundle CI already
>   downloaded exports it.
> - "can never be made to work in the target container" was true of
>   `ci-baseqt:2027.0` only, and only for the *prebuilt* Ubuntu 22 binary.
>
> Kept for the record; the work landed in `fetch-assets.sh` instead.

The build and test path currently depends on a prebuilt third-party OFX plugin
bundle that is fetched at CI time. That dependency is the sole cause of every
current test failure, it can never be made to work in the target container, and
it tests somebody else's code rather than ours. Cut it out entirely; plugin
loading returns later as a pre-release integration test (M11).

## Background: why this has to go

Two independent, pre-existing breakages, both confirmed by reproducing the
failure locally in `aswf/ci-baseqt:2027.0` on `milestone/m2-qt6-migration`:

1. **`SeNoise` is never fetched by anything.** `Tests/BaseTest.cpp`'s
   `registerTestPlugins()` requires `PLUGINID_OFX_SENOISE`, which lives in
   **openfx-misc**. `tools/ci/local/fetch-assets.sh` only fetches openfx-io,
   and so did the pre-M4 upstream workflow (`git show
   204738ddd~1:.github/workflows/ci.yml`). openfx-misc publishes no
   `natron_testing` release at all, so there is nothing to fetch. `ASSERT_TRUE`
   aborts on the first ID — SeNoise — so all three `BaseTest` cases fail on
   this alone.
2. **`IO.ofx` cannot be loaded in the target container.** The asset is
   `openfx-io-build-ubuntu_22-testing.zip` (a 2023 pre-release, GCC 12 → needs
   `GLIBCXX_3.4.30`). `aswf/ci-baseqt:2027.0` is Rocky 9 and its only runtime
   `libstdc++.so.6` is `6.0.29`, capping at `GLIBCXX_3.4.29`; gcc-toolset-14
   compiles against a newer libstdc++ but ships no matching runtime `.so.6`.
   So Natron's own binaries are fine and the vendored plugin is not:
   `couldn't open library .../IO.ofx because /lib64/libstdc++.so.6: version
   'GLIBCXX_3.4.30' not found`. No EL9 build exists upstream, and building
   openfx-io from source in-container is not viable — `ci-baseqt` ships no
   OIIO or OCIO.

Result today: 25 of 28 ctest cases pass; the 3 failures are all `BaseTest`,
all from vendored plugins. This milestone makes the suite green by removing
what was never ours to test.

## Phase 9.1: Make the test surface plugin-free

- [ ] M9.P1.T1 — Delete `BaseTest` and its three plugin-dependent cases
  - files: `Tests/BaseTest.cpp`, `Tests/BaseTest.h`, `Tests/CMakeLists.txt`
  - approach: `BaseTest.cpp`'s only cases are `GenerateDot`, `SetValues`, and
    `SimpleNodeConnections` (lines 243/286/312) — every one of them dies in
    `registerTestPlugins()` at `BaseTest.cpp:99` before reaching its own
    assertions. Delete both files and drop `BaseTest.cpp` from `Tests_SOURCES`
    in `Tests/CMakeLists.txt`. Do **not** try to salvage the cases against
    built-in nodes: what they nominally cover (node creation, knob set/get,
    graph connections) comes back properly in M11, and a half-ported version
    here would just be a second thing to throw away.
  - verify: `tools/ci/local/test.sh ctest debug` reports 25 passed / 0 failed
    and exits 0; `grep -rn BaseTest Tests/` returns nothing.
  - size: S

- [ ] M9.P1.T2 — Make the Python bindings smoke test plugin-free
  - files: `tools/ci/smoke_test.py`
  - approach: `check_pyside6_bindings()` is already self-contained and is the
    part that actually proves PySide6/Shiboken6 works — keep it as-is.
    `check_app_render_with_task_list()` is not: `app.createReader()` /
    `app.createWriter()` resolve to openfx-io's ReadOIIO/WriteOIIO and will
    return `None` once the bundle is gone. Re-express that check against
    Natron's own built-ins (`Engine/EffectInstance.h:88-105` — e.g.
    `PLUGINID_NATRON_READQT` / `PLUGINID_NATRON_WRITEQT` for a Qt-backed PNG
    round trip, or `PLUGINID_NATRON_DOT` plus a knob round trip if the Qt
    read/write nodes turn out to be disabled in this build). The property under
    test is the *binding surface* — that `app.render([(node, first, last)])`
    accepts a list of tuples and returns without a `TypeError` — not OIIO's
    decoders. If no built-in path can drive `app.render()`, drop the render
    check, keep the node-creation and knob round trip, and record why in this
    milestone's `## Decisions`. Update the `_write_solid_png` docstring, which
    currently cites the deleted "Download Plugins" step.
  - verify: `tools/ci/local/test.sh smoke debug` exits 0 with
    `build/assets/Plugins` absent; `grep -n "createReader\|createWriter"
    tools/ci/smoke_test.py` returns nothing.
  - size: M

## Phase 9.2: Remove the vendored assets and their plumbing

- [ ] M9.P2.T1 — Drop the openfx-io fetch from `fetch-assets.sh`
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: delete the "Plugins (openfx-io testing build)" block and every
    reference to it in the script's header comment (the `build/assets/Plugins/`
    result line and the "openfx-io testing build" wording). The script's
    contract narrows to fetching the OpenColorIO configs; M9.P2.T3 decides
    whether even that survives. Leave the in-container re-exec logic alone.
  - verify: with `build/assets` deleted, `tools/ci/local/fetch-assets.sh`
    exits 0 and creates only `build/assets/OpenColorIO-Configs/`;
    `grep -rn "openfx-io\|IO.ofx" tools/ .github/` returns nothing.
  - size: S

- [ ] M9.P2.T2 — Strip `OFX_PLUGIN_PATH` out of `test.sh`
  - files: `tools/ci/local/test.sh`
  - approach: remove the `OFX_PLUGIN_PATH` assignment (line 119), its half of
    the assets precondition check (lines 122-127), the `export` (line 129), and
    every occurrence in the four echoed command lines (140, 172, 179). Narrow
    the precondition to whatever `OCIO` still needs, and update the script's
    header comment, which currently promises "the exact environment ci.yml uses
    (OFX_PLUGIN_PATH/OCIO pointed at ...)". The workflow files never set
    `OFX_PLUGIN_PATH` themselves, so nothing under `.github/` changes here.
  - verify: `grep -rn OFX_PLUGIN_PATH tools/ci/` returns nothing; both
    `tools/ci/local/test.sh ctest debug` and `... smoke debug` still exit 0.
  - size: S

- [ ] M9.P2.T3 — Establish whether the OCIO config asset is still needed, and
      remove it if not
  - files: `tools/ci/local/test.sh`, `tools/ci/local/fetch-assets.sh`,
    `.github/workflows/ci.yml`, `.github/workflows/nightly.yml`
  - approach: with the plugins gone, the only remaining consumer of
    `build/assets/OpenColorIO-Configs/blender/config.ocio` may be Natron's own
    startup colour management. Determine it empirically: run
    `tools/ci/local/test.sh ctest debug` and `... smoke debug` with `OCIO`
    unset and `build/assets` absent. If both stay green, delete the OCIO fetch,
    the `OCIO` plumbing, `fetch-assets.sh` in its entirety, and the "Fetch test
    assets" step from both workflows. If Natron warns or fails without it, keep
    exactly the OCIO half and replace the stale header comments with a one-line
    statement of what actually needs it. Either outcome gets recorded in this
    milestone's `## Decisions` — the next reader should not have to re-derive it.
  - verify: whichever branch is taken, a clean tree with no `build/assets`
    runs `build.sh debug` → `test.sh ctest debug` → `test.sh smoke debug` green,
    and no script or workflow references an asset it no longer fetches.
  - size: M

**Verification gate:** on a tree with `build/assets/` deleted,
`tools/ci/local/build.sh debug` → `test.sh ctest debug` → `test.sh smoke debug`
all exit 0, with ctest reporting 25 passed / 0 failed; `grep -rn
"openfx-io\|OFX_PLUGIN_PATH\|BaseTest" tools/ .github/ Tests/` returns nothing;
and the same three steps are green in a CI run on the milestone branch.
