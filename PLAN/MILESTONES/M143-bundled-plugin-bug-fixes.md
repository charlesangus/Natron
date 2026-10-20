# M143 - Bundled Plugin Bug Fixes

Fix the three bundled-plug-in bugs that M100's cache-equivalence sweep found, in our openfx-misc and openfx-arena forks, then bump the bundled assets and retire the sweep's known-plugin-bug list. The bugs sit on `kKnownPluginBugs` in `Tests/Native/CacheEquivalence_Test.cpp:1682`:
- `eu.cimg.EdgeExtend` (openfx-misc, `CImg/Blur/CImgBlur.cpp`): `premult`/`mergeOver`/`unpremult` index RGBA channels and read out of bounds when `processA` is off and the working image has fewer than four channels.
- `net.sf.cimg.CImgExpression` (openfx-misc, `CImg/CImg.h`): the expression parser reads an uninitialised `variable_name` in the math parser.
- `net.fxarena.openfx.Sketch` (openfx-arena, `Magick/Sketch.cpp`): ImageMagick's `sketch` draws from random noise seeded from entropy, so two renders of one state differ.

The first task is a diagnosis. `CacheEquivalenceKnownPluginBugTest.KnownPluginBugsStillCrash` passes in taskgraph mode only because its 60 s child watchdog (`kKnownBugChildSeconds`, `:1705`) fires: 62.9 s there against 3.3 s in legacy. M100 already fixed the segfault handler's `exit(1)` deadlock (`~QThreadPool` waiting on the faulting task), so this is a different hang. Either one of the two crashing plug-ins hangs instead of crashing when rendered on the pool, or the engine deadlocks after a plug-in fault. That distinction decides whether this milestone also owns an engine fix.

Deps: M100 - Content Node Hashing (owns the sweep and the known-bug list).

## Scope

**In:**
- Finding out which plug-in hangs and why, with a stack of the child (plug-in UB versus engine deadlock reached after a plug-in fault).
- The engine fix, only if the stack shows an engine deadlock.
- The three plug-in fixes, as commits in the fork repos (`charlesangus/openfx-misc`, `charlesangus/openfx-arena`).
- Bumping `OPENFX_MISC_REF` and `OPENFX_ARENA_REF` in `tools/ci/local/fetch-assets.sh` (and the comment block above each).
- Emptying the known-bug list, with the mechanism built around it (enum, struct, `KnownPluginBugsStillCrash`, the watchdog, `diedAbnormally`, the sweep's skip branch) removed because nothing uses it.

**Out:**
- Native replacement of EdgeExtend: M123 - Native Sharpen And Glow retires `eu.cimg.EdgeExtend`. This milestone still fixes it, because it is bundled until then.
- Any other plug-in bug or hash gap the sweep may find later: the sweep stays strict, none.
- Upstreaming the fixes to NatronGitHub or CImg: none.
- Changing what the sweep tests or its allowlist.

**Edge cases:**
- If M123 has landed and EdgeExtend is no longer bundled when this runs, drop that fix. `KnownPluginBugsStillCrash` already fails on a listed plug-in that is not bundled, so the list entry is removed with it.
- If the hang is the engine's, fixing the plug-ins makes the watchdog never fire, hiding the bug again. The engine fix needs its own test that does not depend on a faulting plug-in.
- Fixing EdgeExtend must leave four-channel output bit-identical; the sweep and any existing EdgeExtend render test cover it.
- Sketch must stay deterministic when several render threads run the effect at once; a process-global seed set per render is not enough if ImageMagick's random state is shared across threads.
- The sweep's two cold renders of the same state must now match for Sketch, and its own check ("no longer renders non-deterministically") is removed along with the list.

**Assumptions:**
- Unconfirmed: the CImg expression hang is the one in the 62.9 s run. The stack in T1 settles it.
- Unconfirmed: a local patch for each bug may exist under `build/ceq/` (`cimg-undefined-variable.patch`, `openfx-misc-edgeextend-spectrum.patch`, `openfx-arena-sketch-seed.patch`); it is not on every host. Each task describes its fix independently and treats a patch as a hint, to be reviewed, not applied blindly.
- Unconfirmed: the fork repos accept direct pushes by the PM, as M100 and earlier fork bumps did (see the comments in `fetch-assets.sh`).

## Phase 143.1: The taskgraph hang

- [x] M143.P1.T1 — Capture a stack of the hanging known-bug child and name the cause
  - files: none modified; findings recorded in this file's `## Decisions` (a dated bullet) and in the PR body
  - approach:
    - Build or reuse a release taskgraph build of `d521f111a` or later, then run `KnownPluginBugsStillCrash` in taskgraph mode. Run the child directly: `--gtest_filter=*KnownPluginBugsStillCrash* --gtest_death_test_style=threadsafe`, or the threadsafe child's own command line.
    - While the child runs past ~10 s, take stacks of all threads with `eu-stack -p <pid>` inside the container via `docker exec -u root --privileged` (plain `docker exec` cannot ptrace). Also record `/proc/<pid>/task/*/stat` states to see which threads are blocked. Repeat with each of the two crashing plug-ins alone (temporarily limit the loop), to learn which one hangs.
    - Classify: **plug-in UB** (a thread spinning or blocked inside CImg/EdgeExtend code, or in `std::terminate`/`abort` handling with nothing waiting on it in the engine) versus **engine deadlock** (the faulting thread is gone or parked, and pool or scheduler threads wait on a task, condition or mutex it held). Name the lock or wait.
    - Write the answer into `## Decisions`. If it is an engine deadlock, state the exact waiting chain, so T2 is scoped.
  - verify: the Decisions entry names the plug-in, the blocked threads and the classification, with the stack excerpts the claim rests on. A second run reproduces the same stack shape.
  - size: M

- [x] M143.P1.T2 — Fix the engine deadlock (only if T1 classifies it as one)
  - files: determined by T1 (likely `Engine/RenderScheduler.cpp`, `Engine/AppManager.cpp`, or the crash/signal handler files); `Tests/` for the new test
  - approach: remove the wait chain T1 found, without a bounded timeout as the "fix". Add a deterministic test that reproduces the chain without a crashing plug-in, for example a task that raises a controlled fault or throw on the pool while another thread waits on it. If T1 says the hang is plug-in UB only, mark this task `n/a - plug-in UB` in the Decisions entry and tick it without code.
  - verify: the new test fails before and passes after; the `KnownPluginBugsStillCrash` child on the unfixed plug-ins now dies in ~3 s in taskgraph mode, as in legacy.
  - size: L

## Phase 143.2: Fork fixes

- [x] M143.P2.T1 — EdgeExtend: stop `mergeOver` and `premult` indexing absent channels
  - files: openfx-misc fork: `CImg/Blur/CImgBlur.cpp` (around the EdgeExtend branch of `render`, ~`:1727`, and the `premult`, `mergeOver` and `unpremult` helpers it calls)
  - approach:
    - The three helpers assume `cimg.spectrum() == 4`. When the process-channel toggles disable A (or other channels), the working image holds fewer channels, and they read past the end.
    - Make them channel-count aware, or make the EdgeExtend branch skip them when the image is not RGBA. Pick the behaviour that keeps four-channel output bit-identical and defines the fewer-channel output explicitly. A bare early `return` from `render` can leave the destination unwritten, so check what the host sees and write the destination.
    - Commit to a branch of the openfx-misc fork with a message naming the sweep finding.
  - verify: in the fork, build the plug-in and render EdgeExtend with `processA` off and with every combination of R/G/B/A toggles under valgrind or ASan (no invalid read); the four-channel render's pixels are unchanged against the previous fork commit. Record the commit hash for P2.T4.
  - size: M

- [x] M143.P2.T2 — CImg expression parser: initialise `variable_name` before use
  - files: openfx-misc fork: the bundled `CImg/CImg.h` (the `variable_name.assign(ss, ...)` statement near the `cimg_mp_operator_dollar` block, ~`:22320`)
  - approach:
    - In the parser's `mp` function, `variable_name` is assigned inside `#ifdef cimg_mp_operator_dollar` but read afterwards by code compiled whether or not that macro is defined. Without the macro the buffer is uninitialised. Move the `assign(...).back() = 0` out of the `#ifdef`, before it, so every later read sees a terminated name.
    - Keep the change a one-statement move; do not reformat or upgrade CImg.h.
    - Commit to the openfx-misc fork branch.
  - verify: run the CImgExpression plug-in with a variable-name expression and a plain one under valgrind (`--track-origins=yes`) or MSan: no use of an uninitialised value. Record the commit hash.
  - size: M

- [x] M143.P2.T3 — Sketch: fixed noise seed
  - files: openfx-arena fork: `Magick/Sketch.cpp` (render, before `image.sketch(...)`, ~`:193`)
  - approach:
    - ImageMagick's `SketchImage` draws strokes from random noise seeded from entropy unless a random seed is set. Set a fixed seed before `image.sketch(...)`, via `Magick::SetRandomSeed(<constant>)` or the per-image random-state setting that applies to that image only.
    - Prefer a per-call mechanism over a process-global one. If only the global call exists, set it inside the same critical section as the `sketch` call, so parallel Sketch renders cannot interleave.
    - Commit to the openfx-arena fork branch.
  - verify: render Sketch twice on the same input, same parameters, in one process and in two processes: byte-identical. Render two Sketch nodes concurrently on different threads and compare each against a single-threaded render. Record the commit hash.
  - size: M

- [x] M143.P2.T4 — Bump the bundled asset refs
  - files: `tools/ci/local/fetch-assets.sh` (`OPENFX_MISC_REF`, `OPENFX_ARENA_REF` and their comment blocks)
  - approach: set the two refs to the fork commits from P2.T1-T3 (one openfx-misc commit holding both CImg fixes, or the last of two). Add one sentence to each comment block saying what the commit fixes. Run `bash -n` on the script, then confirm the new refs resolve with `git ls-remote` on each fork.
  - verify: `bash -n tools/ci/local/fetch-assets.sh`; both refs exist on their fork remotes; a fresh asset fetch in the phase build picks them up (the plug-ins' `.ofx` bundles are rebuilt).
  - size: S

## Phase 143.3: Retire the known-bug list

- [x] M143.P3.T1 — Remove the known-plugin-bug mechanism from the sweep
  - files: `Tests/Native/CacheEquivalence_Test.cpp`
  - approach:
    - Delete `KnownBugKind`, `KnownPluginBug`, `kKnownPluginBugs`, `findKnownPluginBug`, `kKnownBugChildSeconds`, `kKnownBugChildHungExitCode`, `exitIfStillRunningAfter`, `diedAbnormally`, the `CacheEquivalenceKnownPluginBugTest` fixture and its `KnownPluginBugsStillCrash` test, and the skip/known-bug branches in `CacheEquivalenceSweepTest` (`:1769-1790`). The empty array would not compile (`sizeof` of a zero-length array) and nothing else would use the rest.
    - Let the shards sweep all three plug-ins; the strict "hash gap" and "non-deterministic" assertions then apply to them like any other.
    - Drop now-unused includes (`<thread>`, `<sys/wait.h>`) only if nothing else needs them.
  - verify: the file builds; in the phase build, all 16 sweep shards pass in legacy and taskgraph, EdgeExtend, CImgExpression and Sketch included, and none logs a skip.
  - size: M

## Decisions

- 2026-10-20 — P2.T4: refs bumped to openfx-misc `6ed7a959` and openfx-arena `e9d1565c` (Texture shares Sketch's lock; `973ca93` + `e9d1565`) in c05418b48. Locally, Texture alone and beside Sketch show 0/8 differing frames over 8 runs per mode (previously 1–4 of 8); Sketch stays byte-identical; all 16 shards pass in taskgraph. Main (M109) merged into the branch.
- 2026-10-20 — P2 verified locally (fork branch `m143/bundled-plugin-bug-fixes`, openfx-misc `6ed7a959`, openfx-arena `ed63abf`): all 16 sweep shards pass in both schedulers with no skips; taskgraph shard 4 (EdgeExtend) 40/40, while the old `CImg.ofx` aborts 6/6 on heap corruption. Valgrind clean for EdgeExtend over R/G/B/A, RGB, R+A and A-only, and for CImgExpression with variable names (the old plug-ins show invalid writes in `mergeOver` and an invalid read in `compile`). Four-channel EdgeExtend output is exactly unchanged. Sketch is byte-identical across renders, processes and 4-way parallel frames. Full ctest 2581/2581 in both modes. P3.T1 committed as 8ed920fb6.
- 2026-10-20 — Scope addition: Texture (openfx-arena) sets the same process-global ImageMagick seed without a lock, so parallel Texture frames differ (1–4 of 8 per run) and a concurrent Sketch perturbs it. Sketch and Texture now share one seed-and-draw lock. The sweep could not see this: it renders one plug-in at a time per shard.
- 2026-10-20 — P1.T1: the taskgraph hang is **plug-in UB in EdgeExtend**, not an engine deadlock; P1.T2 is `n/a - plug-in UB`. `mergeOver` (openfx-misc `CImg/Blur/CImgBlur.cpp:1083-1104`) writes channel 3 of a 3-channel image when `processA` is off, 16 KB past the buffer. In the hung child (3 of ~190 local taskgraph runs; the CI hangs are the first child, EdgeExtend) all 4 threads sleep on futex: main in `FrameFuture::wait()` (`Engine/RenderScheduler.cpp:89-95`) waiting on the render task; one pool thread in `gomp_team_barrier_wait_end <- gomp_team_end <- CImgBlurPlugin::render` (return address of the `GOMP_parallel` for `mergeOver._omp_fn.0`, a one-thread team at 64×64), whose barrier state the overflow corrupted; the other pool thread idle; the watchdog. No engine mutex is held. Same stack shape in two captures. CImgExpression always segfaults in `_cimg_math_parser::compile` and never hangs. Repro scripts and captures live under the m143 worktree's `build/m143t1-*`.
- 2026-10-20 — P2.T1: EdgeExtend passes the image through unchanged when the working image is not RGBA with alpha at channel 3 (fork commit `94487064`), replacing a debug `assert` that would abort in PR CI. Four-channel arithmetic is untouched. Partial-matte cases such as RG+A get no edge extension rather than a partial one.
- 2026-10-20 — P2.T2: `CImg.h` is not tracked in the openfx-misc fork; `fetch-assets.sh` downloads it at the fork's `CIMGVERSION`. The fix ships as a `CImg/CImg.h.patch` in the fork, applied by `fetch-assets.sh` beside the existing `inpaint.h.patch`.
- 2026-10-20 — P2.T3: ImageMagick 7.1.1-6 has only a process-global seed, so Sketch seeds and sketches under one static mutex (fork commit `ed63abf`). Open: `Texture.cpp` also sets the global seed without that lock, so a concurrent Texture render could still perturb a Sketch; the P2 verification checks whether the sweep sees it.
- 2026-10-19 — Remove the known-bug mechanism, do not leave it empty: M100 designed the list to fail once a plug-in stops showing its bug, so an empty list is dead code (and a zero-length array does not compile). A new plug-in bug can add the mechanism back from git history. The user may veto this and keep the skeleton.

**Verification gate:** remote release build with the bumped asset refs: all 16 `CacheEquivalenceSweep` shards plus the scenario tests pass in legacy and taskgraph with no plug-in skipped; `KnownPluginBugsStillCrash` no longer exists; the full ctest passes in both schedulers, and no test run takes a 60 s watchdog path. PR CI (debug asserts) is green. T1's classification is recorded in `## Decisions`.
