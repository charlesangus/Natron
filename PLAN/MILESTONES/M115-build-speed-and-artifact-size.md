# M115 - Build Speed And Artifact Size

Full title: Faster local UAT builds, remote builds on GitHub Actions, clearer build
options, leaner release artifacts

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

Development is bottlenecked on build time, and slow hosts can run only one
container build at a time. The repo is public, so GitHub-hosted runners are free
and run about 20 jobs at once. Each runner is 4 vCPU, so remote builds gain
parallelism, not per-build speed. This milestone adds remote builds alongside local
ones, selected per host, with two build kinds: **UAT** (`fast`, `-O1`, for GUI
checks) and **perf** (`release`, `-O2`, stripped at staging, for performance
testing on the target host).

Baseline (2026-10-12, PR run on milestone/m45-native-tab-menu): container init ~2
min, warm-ccache debug build 7 min, ctest legacy 23 min + ctest taskgraph 23 min run
serially (`test.sh` runs `ctest -V`, no `-j`) — ~57 min total. Nightly release
build ~30 min. No workflow produces an AppImage except `release.yml` on tag push.

## Scope

**In:**
- Local preset trimming, discoverability and docs (Phases 115.1–115.2).
- Stripping at staging, so the perf AppImage stays lean (Phase 115.3).
- A dispatch-only `remote-build.yml` plus a local `tools/ci/remote/build.sh` that
  mirrors `build.sh` and fetches the artifacts (Phase 115.4).
- Per-host build-mode selection through `.local/build-mode` (Phase 115.5).
- A shorter PR CI wall time (Phase 115.6).

**Out:**
- Any change to the `/cat-pm` skill. Build-mode guidance for the PM lives in the
  board's Context section only.
- Self-hosted runners.
- Performance benchmarking on GitHub runners: the perf AppImage is built remotely
  but measured on the target host.
- Debug-preset AppImages (llvmpipe SIGFPE under `-DDEBUG`), and M27 - Real Debug
  Build's `NDEBUG` fix.
- Building on every push: remote builds run only when dispatched (plus the
  main-push cache warm-up in M115.P4.T2).

**Edge cases:**
- HEAD not pushed → `tools/ci/remote/build.sh` refuses and says so; it never pushes.
- gh 2.23 on the dev host: `gh workflow run` returns no run ID, so the script finds
  its run by a unique token in the workflow's `run-name`.
- Two lanes dispatching at once: the tokens keep their runs apart; artifacts are
  named by preset and SHA.
- Run cancelled or failed → the script exits non-zero and prints the run URL; it
  downloads the ctest log whenever one was uploaded.
- No FUSE on the host → the GUI runner extracts the AppImage instead of mounting it.

**Assumptions:**
- Free public-repo runners stay at 4 vCPU / 16 GB with ~20 concurrent jobs.
- The 10 GB actions/cache quota fits the debug, release and fast ccache families
  once each family is capped (measured in M115.P4.T2).

## Phase 115.1: Faster local builds for UI/UAT iteration

- [x] M115.P1.T1 — Trim what a UI-only edit rebuilds under the `fast` preset
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

- [x] M115.P2.T1 — Print the resolved build type and flags at the start of every `build.sh` run
  - files: `tools/ci/local/build.sh`
  - approach: before configuring, echo which preset was selected and its
    resolved `CMAKE_BUILD_TYPE` plus the actual compile flags that implies
    (optimization level, `-g` or not, `NDEBUG`/`DEBUG`) — so running `release`
    makes it obvious it ships debug info today, without reading the script.
  - verify: running `build.sh` with no args, `debug`, `release`, and `fast` each
    prints its resolved type/flags before `cmake` runs.
  - size: M

- [x] M115.P2.T2 — Document the three presets in `tools/ci/local/README.md`
  - files: `tools/ci/local/README.md`
  - approach: one short section per preset — what it compiles with, how long it
    roughly takes relative to the others, and when to reach for it (`fast` for
    UI/UAT checkpoints where render performance doesn't matter, `debug` for
    single-file iteration with live assertions, `release` to match CI/ship).
  - verify: README's description of each preset's flags matches
    `CMAKE_BUILD_TYPE_ARGS` in `build.sh` exactly.
  - size: M

## Phase 115.3: Minimize and verify release artifact size

- [x] M115.P3.T1 — Strip debug info when staging a bundle, regardless of build type
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

- [x] M115.P3.T2 — Re-measure AppImage size and record the before/after
  - files: none (measurement and decision log only)
  - approach: build the `release` preset, package it via the normal pipeline
    before and after Phase 115.3's stripping change, and note both sizes.
  - verify: sizes recorded in this file's `## Decisions`.
  - size: S

## Phase 115.4: Remote builds on GitHub Actions

- [x] M115.P4.T1 — Add a dispatch-only `remote-build.yml` workflow
  - files: `.github/workflows/remote-build.yml`
  - approach: `workflow_dispatch` with inputs `preset` (`fast`|`release`), `tests`
    (bool), `appimage` (bool, default true) and `token` (string), and a `run-name`
    that includes the token. Run in the same `aswf/ci-vfxall` container as
    `ci.yml`, with the same asset/Slang caching. Then `build.sh <preset>` (`fast` with
    tests off unless `tests` is set — see M115.P1.T1). If `tests` is set, run
    `test.sh ctest` under both schedulers and `smoke`, and upload the ctest log. If
    `appimage` is set, run `stage-bundle.sh`, `make-appimage.sh` and
    `check-startup.sh` exactly as `release.yml` does. Upload
    `natron-<preset>-<sha>` with 7-day retention.
  - verify: dispatch on a pushed branch with each preset; both runs produce an
    AppImage artifact that passed `check-startup.sh`; record warm `fast` wall time
    in `## Decisions`.
  - size: M

- [x] M115.P4.T2 — Give each preset its own capped ccache family
  - files: `.github/workflows/remote-build.yml`, `.github/workflows/ci.yml`, `.github/workflows/nightly.yml`
  - approach: key `ccache-linux-<preset>-<branch>-<run_id>`, restoring the branch's
    own entry first, then `ccache-linux-<preset>-main-`. `release` shares the
    family `nightly.yml` already maintains. Add a `push: main` trigger that builds
    `fast` with no tests and no AppImage, so main's `fast` cache stays warm. Measure
    entry sizes from `ccache -s` and the cache list, then set a `CCACHE_MAXSIZE`
    per family so that all three fit in the 10 GB quota with room for per-branch
    entries.
  - verify: a second dispatch on the same branch reports a high ccache hit rate;
    measured sizes and chosen caps recorded in `## Decisions`.
  - size: M

- [x] M115.P4.T3 — Add `tools/ci/remote/build.sh`, the remote twin of `build.sh`
  - files: `tools/ci/remote/build.sh`
  - approach: `build.sh <fast|release> [--tests] [--no-appimage]`. Refuse unless
    HEAD equals the branch's `origin` tip. Generate a token, `gh workflow run
    remote-build.yml --ref <branch>` with the inputs, then poll `gh run list
    --workflow remote-build.yml --json databaseId,displayTitle,headSha` until the
    token's run appears (gh 2.23 returns no run ID). `gh run watch --exit-status`,
    then `gh run download` into `build/remote/<preset>/`. Print the AppImage path,
    and exit with the run's status, printing its URL on failure.
  - verify: run it for `fast` and for `release --tests` on a pushed branch; the
    AppImages land locally and start; a deliberately failing test makes the script
    exit non-zero with the ctest log downloaded.
  - size: M

- [x] M115.P4.T4 — Let the GUI test runner use a downloaded AppImage
  - files: `Tests/gui/run-gui-test.sh`
  - approach: when the binary argument ends in `.AppImage`, run it with
    `--appimage-extract-and-run` (or extract once to a temp dir) so it works
    without FUSE inside the dev container or on a bare host. Keep the existing
    build-tree path unchanged.
  - verify: `run-gui-test.sh Tests/gui/m75_read.py build/remote/fast/<AppImage>`
    passes under Xvfb in the container.
  - size: M

## Phase 115.5: Per-host build-mode selection

- [x] M115.P5.T1 — Define `.local/build-mode` and ignore `.local/` in the repo
  - files: `.gitignore`, `tools/ci/remote/build.sh`
  - approach: `.local/build-mode` holds `local` or `remote` (first non-blank,
    non-`#` line). A missing file means `remote`. Add `.local/` to `.gitignore`;
    today only this clone excludes it, via `.git/info/exclude`. Add a
    `tools/ci/remote/build.sh --mode` that prints the resolved mode, so a PM can
    query it with one command.
  - verify: `--mode` prints `remote` with no file, and `local`/`remote` per the file;
    `git status` stays clean with a `.local/` present.
  - size: M

- [x] M115.P5.T2 — Document local vs remote builds and the two build kinds
  - files: `tools/ci/remote/README.md`, `tools/ci/local/README.md`
  - approach: when to use remote (default, slow hosts, parallel lanes) vs local
    (powerful host, tight single-file iteration). UAT = `fast` AppImage, perf =
    stripped `release` AppImage measured on the target host. Batch one build per
    phase in both modes. In remote mode, run GUI checks locally against the
    downloaded AppImage via `run-gui-test.sh`. Cross-link from the local README.
  - verify: every command and flag in the README matches the scripts.
  - size: M

## Phase 115.6: Faster PR CI

- [x] M115.P6.T1 — Run the two scheduler ctest passes as parallel matrix jobs
  - files: `.github/workflows/ci.yml`, `.github/workflows/beta-release.yml` (only if its `workflow_run` check needs the new job names)
  - approach: a matrix over `NATRON_RENDER_SCHEDULER` (`legacy`, `taskgraph`). Each
    job builds debug from the warm ccache and runs one ctest pass and one smoke
    test. Only the `legacy` job saves the ccache. Keep the required check names
    branch protection and `beta-release.yml` rely on working (add an aggregating
    job if needed). Do not pass a build tree between jobs.
  - verify: a PR run passes with both jobs; wall time recorded before/after in
    `## Decisions` (target ~33 min from ~57); a merge to main still triggers the
    beta.
  - size: M

- [x] M115.P6.T2 — Evaluate `ctest -j` under Xvfb
  - files: `tools/ci/local/test.sh`
  - approach: try `ctest -j$(nproc)` (each test needs its own display — check that
    `xvfb-run --auto-servernum` per-ctest, or one shared server, is safe). Adopt it
    only if three consecutive CI runs pass with no new flakes; otherwise revert and
    record why.
  - verify: three green runs with the timing recorded, or a recorded decision not
    to adopt.
  - size: M

**Verification gate:** all three `build.sh` presets print their resolved
build type and flags, and both READMEs document local and remote builds
accurately. `tools/ci/remote/build.sh` returns a `fast` AppImage and a stripped
`release` AppImage, both passing `check-startup.sh`. A GUI script passes under Xvfb
against the downloaded `fast` AppImage. `--tests` reports ctest pass/fail
correctly, and `--mode` resolves `.local/build-mode` with `remote` as the default.
The stripped `release` AppImage's size is recorded as a clear improvement over the
pre-milestone baseline, and PR CI wall time is recorded before and after.
`check-relocatable.sh`, `check-startup.sh` and the ctest suite stay green
throughout.

## Decisions
- 2026-10-13 — **Claimed by takeover as a second lane** (`--claim --parallel 2`, `.worktrees/m115`, branch `milestone/m115-build-speed-and-artifact-size`) while M27 - Real Debug Build awaits CI. The claim's file-overlap test failed against M27 - Real Debug Build (`CMakeLists.txt`, `tools/ci/local/README.md`) and M64 - Tiled Rendering (`.github/workflows/ci.yml`, `tools/ci/local/test.sh` via M64.P6.T2); the user waived both — run every task now and rebase/resolve when those land. Freshness check passed.
- 2026-10-13 — **Resized S → M:** P2.T1, P2.T2, P4.T4, P5.T1, P5.T2 — each changes script logic, keeps docs consistent with scripts, or touches two files. P3.T2 stays S (measurement only).
- 2026-10-13 — **Docker down after a host reboot**, so verification runs remotely: the dispatch-only workflow is registered on the milestone branch by a temporary `push` trigger (reverted in the next commit; a workflow absent from `main` can't be dispatched until it has run once).
- 2026-10-13 — **Stripping must precede patchelf** (P3.T1): stripping after patchelf segfaulted the non-PIE executables at startup, because the container's old patchelf leaves `.dynsym` straddling the page it adds below the image base and strip then moves it to an unmapped offset. Each ELF is now stripped as it lands in the stage, before its RUNPATH is set (0605f6f85). Unstripped `release` AppImage artifact: 419 MB (run 38505842066).
- 2026-10-13 — **P6.T2 also gives each test process its own disk-cache, config and data dirs** (`Tests/PerProcessUserDirs.h`), since `--clear-cache` wiped caches a concurrent case was using; seven wall-clock-bound cases are `RUN_SERIAL`. Knob `NATRON_CTEST_JOBS` (CI: 4; 1 = old serial run).
- 2026-10-13 — **Release AppImage size (P3.T2):** unstripped 419 MB (run 38505842066) → stripped 88 MB (run 38507504893, 0605f6f85), artifact sizes; staging stripped 268 ELFs, 1.38 GB → 257 MB. `check-relocatable.sh` and `check-startup.sh` pass on the stripped tree.
- 2026-10-13 — **ccache families (P4.T2):** measured entries before the change — debug 1.31–2.12 GB, release 1.52–2.02 GB, no fast entry; deps 0.16 GB, assets 0.01 GB. Caps: debug 3G, release 3G, fast 2G (8G of the 10 GB quota; raise fast first if it thrashes). Second `release` dispatch on the branch restored the branch's own entry and hit 871/876 compiles (99.4%), build ~4 min; release entry 1.93 GB. The old unprefixed debug key is a last restore-key so main's first post-merge run stays warm.
- 2026-10-14 — **Remote builds (P4.T1):** both presets produced an AppImage that passed `check-startup.sh` (release run 38507504893; fast runs 38505839868 and 38508837067). Cold `fast` 34 min (empty fast cache); warm `fast` 5 min 23 s wall time, 656/657 hits, stripped fast AppImage 86 MB. The local download step can't be exercised from this host until `*.blob.core.windows.net` is reachable.
- 2026-10-14 — **PR CI matrix (P6.T1):** run 38506076648 (751c9331a) green, 40.5 min wall time vs ~57 min serial baseline; short of the ~33 min target, and P6.T2's `ctest -j4` is the next cut. `build-and-test` is now an aggregating job because branch protection requires that exact name (required: `format`, `lint-ci`, `build-and-test`); it also diffs both legs' smoke renders to keep cross-scheduler parity. `beta-release.yml` keys off the `Tests` workflow's conclusion, so it needs no change — confirm the beta fires on the first merge to main.
- 2026-10-14 — **`ctest -j4` adopted (P6.T2):** first three runs at 0605f6f85 took ~20 min each but one hit a new flake — `SchedulerEquivalence.TwoWritersOnTwoBranchesRenderedTogether` aborted with "QThread: Destroyed while thread is still running". Root cause was a product race, not test isolation: `removeQuitRenderThreadsInternal` `deleteLater()`'d a retired render thread on `hasQuit` while it could still be blocked on `renderThreadsMutex`; it now also requires `isFinished()` (cb85aa3d9). Three consecutive runs at cb85aa3d9 green: 18.2, 21.1, 19.6 min (vs 40.5 min matrix-only, ~57 min serial baseline).
- 2026-10-14 — **Build READMEs (P5.T2):** a read-only audit against the scripts found the ctest-logs artifact described as also holding smoke logs, `run-gui-test.sh` presented as host-runnable (it needs the natron-dev container's GL/LLVM libs, Xvfb and xset), and the local README missing `--tests`, `--no-ccache-stats`, `fast`'s tests-off default, `NATRON_CTEST_JOBS` and `NATRON_RENDER_SCHEDULER`; all fixed (ad8429225). The branch was rebased onto main after M69 - Minor Cleanup merged (7ae110e9c).
- 2026-10-14 — **UI-only rebuild under `fast` (P1.T1):** no further cut. A `ninja -t query` read of fresh `fast` trees: tests ON has 1487 targets / 172 custom commands, tests OFF 1108 / 134; a Gui `.cpp` edit recompiles one TU, re-archives `libNatronGui.a` and relinks `App/Natron` (plus `Tests/GuiTests` with tests ON). No always-dirty custom command; the link is ~1 s (GNU ld), so a faster linker isn't worth it. Measured in the container, a real one-line `Gui/NodeGraph.cpp` change rebuilds in 9 s with tests ON and 9 s OFF — tests-off (34042b643) pays off on full and reconfigured builds, not on incremental ones. Header fan-out (`Gui/Gui.h` 101 objects, `GuiApplicationManager.h` 110) is the remaining cost and needs header work, out of scope. The `fast` build-tree binary passes `m75_read.py` under Xvfb.
- 2026-10-14 — **AppImage GUI runs (P4.T4, 25a6afeaa):** `run-gui-test.sh` against the downloaded remote `fast` AppImage first hung on the first-launch "check for updates?" dialog; it now runs the AppImage under a throwaway HOME seeded with `checkForUpdates=false` (as `check-startup.sh` does), adds the container's matching `PySide6.QtTest` to the extracted bundle (the bundle ships only QtCore/QtGui/QtWidgets by design), and wraps `oiiotool` with the container's library path. `m75_read.py`: 88 checks pass against the AppImage (remote fast run 38569591962, 87 MB, downloaded by `tools/ci/remote/build.sh`) and against the build tree.
- 2026-10-14 — **Remote build script verified (P4.T3):** `fast` run 38569591962 downloaded an 87 MB AppImage; `release --tests` run 38579449008 (677e7c445) downloaded an 89 MB stripped AppImage that passed `check-startup.sh`, with 2155/2155 ctest passing under both schedulers; a throwaway branch with a deliberately failing test (run 38569625483) made the script exit 1, print the run URL and download both ctest logs.
- 2026-10-14 — **Release-only golden failure fixed in this milestone (677e7c445):** the first `release --tests` run (38569625667) failed `GoldenXmlTest.SyntheticRotoPaintAndTracker` under both schedulers — also the cause of the red nightly on main since M93 added the golden from a debug build. `NATRON_TRACKER_ENABLE_TRACKER_PM` was defined under `#ifdef DEBUG`, adding a secret `pmScoreType` knob to debug-saved Tracker nodes; it is now an explicit opt-in in every build type and the golden drops the knob, so debug and release serialize the same 37 Tracker knobs.
