# M175 - Build Script Jobs And Test Legs

Three build-script fixes from the gpu PM's build and test efficiency review (inbox, 2026-10-24). First, `tools/ci/local/build.sh` on main drops its arguments since M154 - Fresh Buffer Page Zeroing (#79): the parse loop shifts `$@` empty before the host-side re-exec into the dev container (`exec "${SCRIPT_DIR}/devshell.sh" env ... build.sh "$@"`), so every host invocation builds `debug` without `--tests`. Second, the local job count is hardcoded to 3 (`JOBS="${NATRON_BUILD_JOBS:-3}"`), sized for a 4-core 15 GB host. Third, every remote `--tests` run executes the whole ctest suite and smoke test twice, once per scheduler leg (`ctest` job matrix `scheduler: [legacy, taskgraph]` in `.github/workflows/remote-build.yml`), which doubles the wait on intermediate fix builds.

## Scope

**In:**
- Re-exec `build.sh` in the container with the original arguments.
- Derive the default local `JOBS` from the host, always leaving at least one core free (user).
- A single-scheduler `--tests` option for intermediate builds in `tools/ci/remote/build.sh` and `remote-build.yml`. Both legs stay the default, and milestone gates keep running both.

**Out:**
- PR CI (`ci.yml`) and nightly scheduler coverage. They keep both legs.
- Any change to ctest's own suite parallelism (`NATRON_CTEST_JOBS`).

**Edge cases:**
- `NATRON_BUILD_JOBS` set: it wins over the derived value, unchanged.
- A 1- or 2-core host: the derived value is never below 1.
- A low-memory host: MemAvailable caps the job count (about 2 GB per compile job), so the memory cap can be below `nproc - 1`.
- The single-scheduler option names an unknown scheduler: the script exits with a usage error before dispatching.

## Phase 175.1: Local build script

- [ ] M175.P1.T1 — Re-exec the local build script with its original arguments
  - files: `tools/ci/local/build.sh`
  - approach: Save `ORIG_ARGS=("$@")` before the argument-parsing `while` loop, and pass `"${ORIG_ARGS[@]}"` to the host-side `exec "${SCRIPT_DIR}/devshell.sh" env ...` re-exec in place of `"$@"`. The gpu PM's uncommitted diff in `.worktrees/m110` is this change.
  - verify: From the host, `tools/ci/local/build.sh fast --tests` prints the `fast` banner and runs ctest inside the container. `tools/ci/local/build.sh` with no arguments still builds `debug`.
  - size: S

- [ ] M175.P1.T2 — Derive the default local job count from the host
  - files: `tools/ci/local/build.sh`
  - approach: Replace the hardcoded default 3 with `min(nproc - 1, MemAvailable / 2 GiB)`, floored at 1, computed on the host before the re-exec and forwarded like `NATRON_BUILD_JOBS` (the container's `nproc` can differ). An explicit `NATRON_BUILD_JOBS` still wins. Update the comment above `JOBS=` to state the rule, and print the chosen value with its source in the existing `ninja -j` banner line.
  - verify: On the 4-core dev host, `tools/ci/local/build.sh fast` reports `-j3` or lower (memory-capped). `NATRON_BUILD_JOBS=2 tools/ci/local/build.sh fast` reports `-j2`. `bash -n tools/ci/local/build.sh` passes.
  - size: M

## Phase 175.2: Remote single-scheduler tests

- [ ] M175.P2.T1 — Add a scheduler input to the remote build workflow
  - files: `.github/workflows/remote-build.yml`
  - approach: Add a `workflow_dispatch` input `schedulers` (string, default `legacy,taskgraph`). Build the `ctest` job's `matrix.scheduler` from it with `fromJSON` over a small step that splits the string into a JSON array, and reject names other than `legacy` and `taskgraph`. The golden-recording step keeps its fixed `legacy` scheduler.
  - verify: `actionlint` (or the repo's `checks.yml` workflow lint) passes. A dispatched run with `schedulers=taskgraph` shows one `ctest (taskgraph)` job; a default dispatch shows both.
  - size: M

- [ ] M175.P2.T2 — Expose the scheduler choice in the remote build script
  - files: `tools/ci/remote/build.sh`
  - approach: Add `--scheduler <legacy|taskgraph>`, which implies `--tests` and dispatches with `-f schedulers=<name>`. Without it, dispatch the default (both legs). Document it in the header usage block and the `usage` line, and say there that milestone gates must run without it.
  - verify: `tools/ci/remote/build.sh fast --scheduler taskgraph --no-appimage` runs one ctest leg and its log download finds `ctest-taskgraph.log`. `--scheduler bogus` exits non-zero with the usage line before dispatching.
  - size: M

- [ ] M175.P2.T3 — Point PMs at the single-leg option for fix builds
  - files: `tools/ci/remote/README.md` (or the remote section of `docs/` that documents `build.sh`)
  - approach: State the rule: intermediate fix builds may use `--scheduler taskgraph`, while a milestone's verification gate runs both legs.
  - verify: reviewed at the gate.
  - size: S

**Verification gate:** `bash -n` passes on both scripts and the workflow lint passes. From the host, `tools/ci/local/build.sh fast --tests` honours both arguments and reports the derived job count. One remote run with `--scheduler taskgraph` shows a single ctest leg, and one default `tools/ci/remote/build.sh fast --tests` run shows both legs green.
