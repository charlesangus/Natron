# M115 - Build Speed And Artifact Size

Full title: Faster local UAT builds, clearer build options, leaner release artifacts

Three build presets already exist in `tools/ci/local/build.sh` (`debug`: `-O0 -g`;
`release`: no explicit type, inherits `CMakeLists.txt`'s `RelWithDebInfo` default
— i.e. `-O2 -g` — to match CI exactly; `fast`: explicit `Release` overridden to
`-O1`, `-DNDEBUG`, no `-g`). Nothing currently staged into a bundle
(`tools/release/stage-bundle.sh`, `tools/release/make-appimage.sh`) strips debug
info, so whichever preset built the tree determines the shipped artifact's size —
this is the confirmed cause of AppImages ranging from ~100MB to ~400MB depending
on which build type produced them. Separate from M27 - Real Debug Build, which
fixes a correctness bug (`NDEBUG` leaking into the debug preset) rather than
speed or artifact size.

## Phase 115.1: Faster local builds for UI/UAT iteration

- [ ] M115.P1.T1 — Trim what a UI-only edit rebuilds under the `fast` preset
  - files: `tools/ci/local/build.sh`, `CMakeLists.txt` (only if a new build option is needed)
  - approach: touch a `Gui/`-only file, rebuild with `fast`, and check ninja's
    build graph (`ninja -t targets` / `ninja -n`) for anything unnecessary
    pulled in for a UI-only change — rebuilding `Tests/`, re-fetching/rebuilding
    OFX plugin assets, or unrelated `Engine/` translation units. Cut what's safe
    to skip (e.g. gate `NATRON_BUILD_TESTS` off for `fast`) without breaking the
    preset for anyone who does need tests built.
  - verify: time a `Gui/`-only incremental rebuild under `fast` before and after;
    record the improvement in this file's `## Decisions`; `fast` still produces
    a binary that starts (`check-startup.sh`).
  - size: M

## Phase 115.2: Make build options discoverable and correct

- [ ] M115.P2.T1 — Print the resolved build type and flags at the start of every `build.sh` run
  - files: `tools/ci/local/build.sh`
  - approach: before configuring, echo which preset was selected and its
    resolved `CMAKE_BUILD_TYPE` plus the actual compile flags that implies
    (optimization level, `-g` or not, `NDEBUG`/`DEBUG`) — so running `release`
    makes it obvious it ships debug info today, without reading the script.
  - verify: running `build.sh` with no args, `debug`, `release`, and `fast` each
    prints its resolved type/flags before `cmake` runs.
  - size: S

- [ ] M115.P2.T2 — Document the three presets in `tools/ci/local/README.md`
  - files: `tools/ci/local/README.md`
  - approach: one short section per preset — what it compiles with, how long it
    roughly takes relative to the others, and when to reach for it (`fast` for
    UI/UAT checkpoints where render performance doesn't matter, `debug` for
    single-file iteration with live assertions, `release` to match CI/ship).
  - verify: README's description of each preset's flags matches
    `CMAKE_BUILD_TYPE_ARGS` in `build.sh` exactly.
  - size: S

## Phase 115.3: Minimize and verify release artifact size

- [ ] M115.P3.T1 — Strip debug info when staging a bundle, regardless of build type
  - files: `tools/release/stage-bundle.sh`
  - approach: after the ldd-closure walk and before (or as part of)
    `check-relocatable.sh`, strip every staged ELF (binaries, `lib/*.so*`, Qt
    plugins, OFX bundle libraries) — e.g. `strip --strip-unneeded`, or split
    debug info out with `objcopy --only-keep-debug` into a sidecar tree that is
    never copied into the AppDir — so a `release`-preset (`RelWithDebInfo`)
    bundle is stripped at packaging time instead of at compile time, keeping
    developer builds debuggable while the shipped artifact stays lean.
  - verify: stage a `release`-preset build end to end and confirm the resulting
    AppImage is a small fraction of its unstripped size; `check-relocatable.sh`
    and `check-startup.sh` still pass on the stripped tree.
  - size: M

- [ ] M115.P3.T2 — Re-measure AppImage size and record the before/after
  - files: none (measurement and decision log only)
  - approach: build the `release` preset, package it via the normal pipeline
    before and after Phase 115.3's stripping change, and note both sizes.
  - verify: sizes recorded in this file's `## Decisions`.
  - size: S

**Verification gate:** all three `build.sh` presets print their resolved
build type and flags; `tools/ci/local/README.md` documents them accurately;
a `release`-preset AppImage built end-to-end through the normal pipeline is
stripped and its size recorded as a clear improvement over the pre-milestone
baseline; `check-relocatable.sh`, `check-startup.sh`, and the ctest suite
stay green throughout.
