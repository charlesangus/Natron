# M155 - Per-Suite Test Processes

`gtest_discover_tests` registers every gtest case as its own ctest entry, so each scheduler step launches the test binary about 2,689 times. 2,604 of those cases finish in under 1 s and the average is 0.68 s, so nearly all of a ~30 min step is process startup: `Tests/wmain.cpp` builds an `AppManager` and runs `manager.load()` (cold OFX rescan, Python and OCIO init) for every case. This milestone registers one ctest entry per gtest suite instead (~287 suites, roughly 8–10× fewer launches), runs the remote scheduler legs with `-j 4`, and fixes every test that leaks state into its suite-mates once they share a process.

Scouted facts (main `31636d20d`, 2026-10-20; re-check at promotion):
- **Executables.** `Tests` (`Tests/CMakeLists.txt:139`, custom `wmain.cpp`) and `GuiTests` (`:212`, `guimain.cpp`, offscreen `QApplication`) are registered at `:250-252` with `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)`. `GpuTests` (`Tests/Gpu/CMakeLists.txt:46`, stock `gtest_main.cc`, only with `NATRON_GPU`) has three calls at `:88-107`: `TEST_PREFIX Gpu.` with `TEST_FILTER -GlInteropTest.*`; `TEST_PREFIX GpuNoPushDescriptor.` with the same filter and `ENVIRONMENT NATRON_TEST_GPU_NO_PUSH_DESCRIPTOR=1`; `TEST_PREFIX Gpu.` with `TEST_FILTER GlInteropTest.*` and `ENVIRONMENT "${GpuTests_GL_ENV}"`. All three carry `TIMEOUT 600 LABELS gpu`. The two Python `add_test` entries (`:262-266`) are unaffected.
- **Discovery cost.** PRE_TEST discovery launches each binary, which today pays the full `manager.load()` before `--gtest_list_tests` prints (comment at `Tests/CMakeLists.txt:246-249`).
- **Serial list.** `Tests/SerialTests.cmake` (included via `TEST_INCLUDE_FILES`, `Tests/CMakeLists.txt:255`) sets `RUN_SERIAL` on 7 exact case names: 4 in `RenderSchedulerAbort` (`Tests/RenderSchedulerAbort_Test.cpp`), 2 in `GraphScalingTest` (`Tests/GraphScaling_Test.cpp`), 1 in `StagedImageEffectTest` (`Tests/Native/StagedImageEffect_Test.cpp`). Each asserts a wall-clock bound. A name that no longer exists is silently ignored.
- **Callers.** `tools/ci/local/test.sh` runs `ctest -V --timeout 300` serially, or `ctest -j N --timeout 300 --output-on-failure` when `NATRON_CTEST_JOBS` > 1. PR CI (`ci.yml:164-173`) sets `NATRON_CTEST_JOBS: 4`; `remote-build.yml` leaves it unset on both scheduler legs (`:140-163`, `timeout-minutes: 75`, ~35 min each) and records goldens with the case-name regex `ctest -R '^NativeGoldenTest\.'` (`:121`). The scheduler is chosen only by `NATRON_RENDER_SCHEDULER`.
- **Process-state hints.** `Tests/PerProcessUserDirs.h:22-30` gives each process its own cache and XDG dirs (still correct per suite; its comment says "every case as its own process"). `BaseTest` (`Tests/BaseTest.cpp`) has no per-test project reset. Known cold-process assumptions: `GpuResidency_Test.cpp:657` ("project format is set once per process"), and the shader tests that make source unique per test to dodge the process-wide compile memo (`Gpu/GpuRuntimeShader_Test.cpp:87`, `Gpu/GpuPipelineCache_Test.cpp:25`, `Native/StagedPointContext_Test.cpp:95`, `Native/NativeRuntimeShaderGpu_Test.cpp:100`).

## Scope

**In:**
- Cheap `--gtest_list_tests` in the custom mains.
- A per-suite ctest registration module replacing every `gtest_discover_tests` call, keeping the GPU prefixes, filters, environments and labels.
- Suite-level `RUN_SERIAL` and timeouts, failing loudly on a stale name.
- Updating every caller that matches ctest names by case, and `-j 4` on the remote scheduler legs.
- A shuffled sweep over the grouped suites, and a fix for every leak it finds.
- Before/after ctest timings.

**Out:**
- A per-case opt-in mode: none (user decision, see Decisions).
- Sharding within a suite, or one process per binary: none.
- M141 - Complete Local Build Environment's debug remote preset and `-V` re-run of failed cases.
- Build (compile) time: none.
- Isolating a leaking suite instead of fixing it, except a case that genuinely needs a fresh process, which gets its own suite (see Edge cases).

**Edge cases:**
- Parameterized (`Prefix/Suite.`) and typed (`Suite/0.  # TypeParam = ...`) suites: the registration groups on the listed suite line, strips the `#` comment, and filters `<suite>.*` with the exact listed suite name.
- A suite whose every case is `DISABLED_`, or a `DISABLED_` suite: not registered.
- A suite mixing passed and `GTEST_SKIP` cases reports pass. No `SKIP_REGULAR_EXPRESSION` is set, because one skipped case would mark the whole suite skipped and hide the passes.
- A case that crashes the process takes its suite down: ctest reports the suite failed, and the gtest output shows the case that was running.
- A case that truly needs a fresh process (e.g. it asserts first-init behaviour): move it into its own suite, so it still gets one, and record why in Decisions.
- The two `Gpu.` registrations share a prefix: a suite name may appear in only one of them (`GlInteropTest` vs the rest), so names stay unique.

**Assumptions:**
- Listing tests needs only static registration, not `manager.load()` or the test-only built-in plugins registered in `main`.
- gtest reads `GTEST_SHUFFLE`, `GTEST_RANDOM_SEED` and `GTEST_REPEAT` from the environment.

## Phase 155.1: Per-suite registration

- [ ] M155.P1.T1 — Make `--gtest_list_tests` skip app startup
  - files: Tests/wmain.cpp, Tests/guimain.cpp
  - approach: After `InitGoogleTest`, when `GTEST_FLAG_GET(list_tests)` (or the bundled gtest's equivalent) is set, return `RUN_ALL_TESTS()` before constructing `PerProcessUserDirs`, the `AppManager`/`QApplication`, or calling `manager.load()`. Listing must print exactly what it printed before.
  - verify: With a built tree, diff `Tests --gtest_list_tests` and `GuiTests --gtest_list_tests` output against main's binaries (identical), and time each listing (well under 1 s, against several seconds before).
  - size: M

- [ ] M155.P1.T2 — Per-suite ctest registration module
  - files: cmake/NatronGtestSuites.cmake, cmake/NatronGtestSuitesAddTests.cmake, Tests/CMakeLists.txt, Tests/Gpu/CMakeLists.txt
  - approach: `natron_add_gtest_suites(<target> [TEST_PREFIX p] [TEST_FILTER f] [PROPERTIES ...])` mirrors CMake's `GoogleTestAddTests.cmake` PRE_TEST mode: a `TEST_INCLUDE_FILES` script runs `<bin> --gtest_list_tests [--gtest_filter=f]` at ctest time (re-listing only when the binary is newer than its cached output), groups cases by suite per Scope's edge cases, and emits `add_test(<p><suite> <bin> --gtest_filter=<suite>.*)` — combining with a negative `TEST_FILTER` as `<suite>.*:-<neg>` — plus `set_tests_properties` with the given properties. Listing failure or a non-zero exit is a `message(FATAL_ERROR)`, never zero tests. Replace all five `gtest_discover_tests` calls, keeping each one's prefix, filter, `ENVIRONMENT` and `LABELS`; put `TIMEOUT` on `Tests`/`GuiTests` at 900 and `GpuTests` at 1200 until T3 sets measured values.
  - verify: `ctest -N` in a configured tree lists one entry per non-disabled suite of each binary (count matches the distinct suites in `--gtest_list_tests`), `ctest -N -R '^GpuNoPushDescriptor\.'` and `-L gpu` list the GPU suites, and `ctest -R '^Gpu\.GlInteropTest$' -V` shows the GL env set. One grouped run of `Tests` passes the same number of cases (`[  PASSED  ]` totals summed over the log) as main's per-case run.
  - size: M

- [ ] M155.P1.T3 — Suite timeouts from measured durations
  - files: Tests/SerialTests.cmake, Tests/CMakeLists.txt, Tests/Gpu/CMakeLists.txt
  - approach: From a per-case run on main (`Testing/Temporary/CTestCostData.txt` or `LastTest.log`, both scheduler modes), sum case durations per suite. Set the default suite `TIMEOUT` to 300 (each case already had to finish inside 300) and give any suite whose summed duration exceeds 100 s an explicit `TIMEOUT` of 3× its slower-mode sum, in the same include file as the serial list. Record the per-suite sums table in Decisions.
  - verify: `ctest -N --show-only=json-v1` shows the timeout per suite; the slowest suite runs under its timeout in both modes.
  - size: M

## Phase 155.2: Serial list and callers

- [ ] M155.P2.T1 — Move wall-clock cases into their own serial suites
  - files: Tests/SerialTests.cmake, Tests/RenderSchedulerAbort_Test.cpp, Tests/GraphScaling_Test.cpp, Tests/Native/StagedImageEffect_Test.cpp
  - approach: Move the 7 `RUN_SERIAL` cases into dedicated suites (e.g. `RenderSchedulerAbortTiming`, `GraphScalingTiming`, `StagedImageEffectTiming`; fixture aliases if they use `TEST_F`), so the rest of each suite still runs in parallel. `SerialTests.cmake` lists those suite names, and `message(FATAL_ERROR)` when any listed name is not a registered test (`if(NOT TEST ...)`), so a rename can't silently drop serialization.
  - verify: `ctest -N --show-only=json-v1` shows `RUN_SERIAL` on exactly those suites; renaming one in a scratch edit makes ctest fail at load; the moved cases pass under `ctest -j 4`.
  - size: M

- [ ] M155.P2.T2 — Update case-name callers and parallelise remote legs
  - files: .github/workflows/remote-build.yml, Tests/PerProcessUserDirs.h, tools/ci/local/README.md, tools/ci/remote/README.md
  - approach: Change the golden-record regex to `'^NativeGoldenTest$'`. Set `NATRON_CTEST_JOBS: 4` on both scheduler legs and lower their `timeout-minutes` to 30. Grep `.github`, `tools` and docs for any other case-level `ctest -R` and fix it. Update `PerProcessUserDirs.h`'s comment to say one process per suite, and document per-suite entries (and `--gtest_filter` for running one case) in the READMEs.
  - verify: `grep -rn "ctest -R" .github tools` shows no case-level regex; a remote `fast --tests` run records goldens and both legs pass with `-j 4`.
  - size: M

- [ ] M155.P2.T3 — Name failing cases in the ctest summary
  - files: tools/ci/local/test.sh
  - approach: After a failing ctest, print each failing suite's gtest `[  FAILED  ]` case lines from `Testing/Temporary/LastTest.log` (deduplicated, before the folded log dump), so a per-suite failure still names its case without opening the log. Leave M141's re-run logic alone if it has landed; this sits after it.
  - verify: Add a deliberately failing case in a scratch build, run `test.sh ctest fast` with `NATRON_CTEST_JOBS=4`, and see the `Suite.Case` name in the summary; remove the case.
  - size: M

## Phase 155.3: Shared-process leaks

- [x] M155.P3.T1 — Shuffled sweep and triage
  - files: tools/ci/local/test.sh, PLAN/MILESTONES/M155-per-suite-test-processes.md
  - approach: Forward `GTEST_SHUFFLE`, `GTEST_RANDOM_SEED` and `GTEST_REPEAT` into the container. On the debug preset (live asserts), run the grouped suites unshuffled and with `GTEST_SHUFFLE=1` at 3 fixed seeds, in both scheduler modes, with `NATRON_CTEST_JOBS=4`. For each failing suite record in Decisions: the case, seed, mode, and a root-cause hypothesis (start with the Scouted suspects: project format set once per process, the process-wide shader compile memo, no project reset in `BaseTest`). The PM then inserts one fix task per root cause as M155.P3.T2 onward, each sized M or larger.
  - verify: The Decisions entry lists every failure with its seed, or states the sweep was clean in both modes.
  - size: M

## Phase 155.4: Measure

- [x] M155.P4.T1 — Before/after ctest timings
  - files: PLAN/MILESTONES/M155-per-suite-test-processes.md
  - approach: Read the ctest step wall time of both scheduler legs from a remote `fast --tests` run and a PR CI run on main before this milestone and on the milestone branch after P3 (`gh api .../jobs/<id>` step timings), plus the ctest entry count. Record the table in Decisions.
  - verify: The table exists with both modes and both workflows.
  - size: S

**Verification gate:** PR CI green on both scheduler legs; a remote `fast --tests` run green on both legs with `-j 4`, goldens recorded; `ctest -N` shows one entry per suite, and the summed `[  PASSED  ]` case count equals main's per-case count in each binary and mode; the 3-seed shuffled sweep is clean in both modes on the debug preset; the `RUN_SERIAL` suites exist and a stale name fails ctest; the before/after timing table is recorded, with each scheduler leg's ctest step at least 5× faster than before.

## Decisions

- 2026-10-21 — **Claimed despite the file-overlap test (user-directed):** the user asked to run this milestone in parallel with M141 - Complete Local Build Environment. Overlapping files: `Tests/wmain.cpp` and `Tests/CMakeLists.txt` with M120 - Native Node Port Kit (merging now); `Tests/wmain.cpp` with M108 - Retire Engine GL Host Path; `Tests/CMakeLists.txt` and `test.sh` with the parked M64 - Tiled Rendering; `README.md` with M107 - GPU Viewer Hand-off; and `test.sh`, both READMEs and `remote-build.yml` with M141. Conflicts are resolved at merge.
- 2026-10-20 — **One ctest entry per suite, with no per-case opt-in** (user): running a single case stays available through `--gtest_filter` by hand. A second registration mode would be code nobody exercises.
- 2026-10-20 — **Fix leaks rather than isolate them** (user): a suite that fails when grouped gets its leak fixed. Only a case that truly needs a fresh process moves into its own suite.
- 2026-10-21 — **A cached Read not setting the project format stays as is (user).** The leak work found that a Read served from the cache never sets the project format. The user ruled that auto-setting the format from a Read is not a desired feature, so no milestone follows from it.
- 2026-10-21 — **Shuffled sweep (P3.T1).** Remote debug run 39645578802 (e3f31748e): live asserts, `-j 4`, unshuffled plus `GTEST_SHUFFLE` seeds 1, 2 and 3, in both scheduler modes. All 324 suites passed in all eight runs except `FrameGraphBuild`. It timed out at 300 s in each run and passed when rerun alone (153–221 s), so this is suite length under load, not a leak. Its timeout was raised to 900 s in `SuiteProperties.cmake`. Leaks found and fixed earlier, on the remote fast runs:
  - **Seven suites:** DeepNodes, DeepRenderPipeline, DeepReshape, DeepFlattenLayers, GpuResidency, RotoLayer and MetadataExpressionDependency. Fixed by `BaseTest::SetUp` resetting the project, clearing the caches and resetting `GpuCostModel` (f148fc2f2). That commit also fixed two Engine bugs, in `AppInstance::addInvalidExpressionKnob` and `KnobHelper::setExpressionInvalid`.
  - **GLScheduler:** a race in the test, where the four branches shared one cache entry (52b63c835).
- 2026-10-21 — **Before/after ctest step timings (P4.T1).** 324 ctest entries after, 2753 before. 2751 gtest cases plus 2 standalone tests pass in each mode, matching main's per-case count.

  | ctest step | main before | M155 after | speed-up |
  |---|---|---|---|
  | PR CI debug, legacy | 11.1 min (39611666939) | 8.2 min (39656817357) | 1.4× |
  | PR CI debug, taskgraph | 16.9 min | 8.6 min | 2.0× |
  | Remote fast, legacy | 7.0 min (39620482877) | 1.9 min (39645408967) | 3.7× |
  | Remote fast, taskgraph | 10.4 min | 3.2 min | 3.3× |
- 2026-10-21 — **Gate restated: no 5× against today's main (user).** The 5× target was set against the serial ~33-minute legs. Since then main went to `-j 4` and a single build (PR #73), so the baseline moved. The user chose to merge with the measured speed-ups recorded. On debug, the long single suites now set the minimum time (FrameGraphBuild, about 3 minutes alone).
