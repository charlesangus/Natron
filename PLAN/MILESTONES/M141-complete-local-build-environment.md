# M141 - Complete Local Build Environment

Make the local dev workflow self-sufficient and current, so a PM or implementer can verify GPU code, run the debug ctest that PR CI runs, and package without waiting on remote builds. Today five gaps force workarounds:
- The `natron-dev` image has no `slangc`. `cmake/FetchSlang.cmake` downloads Slang at configure time, so a container without network cannot configure the GPU targets. Hosts seed `.natron-deps/` by hand and every worktree needs its own symlink.
- The compile-database syntax checker lives only in the gitignored `build/` dir, hard-codes one checkout path and container name, takes `<checkout-root> <base-ref>` (not file paths), and passes no `-I Tests` / `-I libs/xxhash` to test TUs. With no GPU-era tree configured it skips every GPU TU silently, so implementers hand-write stub kernel headers.
- Remote builds offer only `fast` and `release` (both NDEBUG). PR CI runs the `debug` preset with live asserts and `ctest -j 4`, whose parallel output hides the assert text of a failing case.
- `build.sh` runs `git submodule update --init --recursive` on every invocation, which resets a pinned `libs/OpenFX` fork branch.
- Each worktree lane needs its own `fetch-assets`, `.natron-deps` and appimagetool wrapper, assembled by hand.

Deps: none.

Scouted facts (scouted 2026-10-19 on main `d521f111a`; re-check at promotion):
- **Slang resolution:** `cmake/FetchSlang.cmake` honours `SLANG_ROOT` (cmake var, then env), then `NATRON_DEPS_CACHE`, else downloads `slang-${NATRON_SLANG_VERSION}-linux-x86_64.tar.gz` (pinned URL and SHA256 in the same file) into `<repo>/.natron-deps/` (gitignored, `.gitignore:257`). Only `bin/slangc` and `include/slang-cpp-prelude.h` are used.
- **Image:** `tools/ci/local/Dockerfile` is `FROM aswf/ci-vfxall:2027-clang21.1` plus one Mesa relink `RUN`; its header comment says local-only layers belong there, and that only github.com, registry-1.docker.io, auth.docker.io, mirror.gcr.io, ghcr.io and quay.io are reachable from the build environment. CI runs the base image directly (no local layers), so CI keeps downloading Slang; `.github/workflows/ci.yml:146-150` and `remote-build.yml:87-91` already cache `.natron-deps` keyed on `hashFiles('cmake/FetchSlang.cmake')`.
- **Syntax checker:** `build/syntax-check.py` is **not tracked** (`.gitignore:53` ignores `build/`; no reference in `tools/` or `.github/`). It reads `build/fast/compile_commands.json` of the main checkout, rewrites paths for another checkout root, runs `docker exec natron-dev` with 3 workers, and for a TU missing from the DB borrows a sibling's command (so a new GPU TU in an old tree gets `NO-COMMAND`, and test TUs inherit flags that lack the include dirs).
- **Remote build:** `tools/ci/remote/build.sh` accepts `fast|release` (`:78`), dispatches `.github/workflows/remote-build.yml` whose `preset` input lists `fast`, `release` (`:15-17`); CCACHE size is chosen by preset (`:55`). The workflow runs `tools/ci/local/test.sh ctest "$PRESET"` twice (legacy and taskgraph scheduler) with `NATRON_CTEST_JOBS` unset, i.e. serial `ctest -V`. PR CI (`ci.yml:166-173`) sets `NATRON_CTEST_JOBS: 4` (`ctest -j 4 --output-on-failure`) on the `debug` preset.
- **Local scripts:** `tools/ci/local/{Dockerfile,README.md,build.sh,devshell.sh,fetch-assets.sh,package.sh,test.sh}`; `build.sh:186-197` runs the submodule update host-side before entering the container; `package.sh:89-95` prefers `build/appimagetool-wrapper/appimagetool` (an untracked 478-byte shell wrapper beside a 15 MB `appimagetool.real`) when it is executable. README `tools/ci/local/README.md` "Sharing the cache with worktrees" covers ccache only.
- A debug AppImage cannot be packaged in the container (llvmpipe SIGFPE under `-DDEBUG` traps), so a debug remote build must not package one.

## Scope

**In:**
- A Slang layer in the `natron-dev` image, with a drift check against `cmake/FetchSlang.cmake`'s pin.
- A tracked, parameterised syntax-check tool in `tools/ci/local/`, with a file-path CLI, correct include paths for test TUs, and on-demand configure of a GPU-era compile database instead of silently skipping GPU TUs.
- A `debug` preset for `tools/ci/remote/build.sh` and `remote-build.yml`, and `-V` re-run of failed cases after a parallel ctest (local, remote and PR CI all use `test.sh`).
- A worktree bootstrap script, a submodule-update opt-out for `build.sh`, and a tracked appimagetool wrapper generator.
- Documentation in `tools/ci/local/README.md` and `tools/ci/remote/README.md`.

**Out:**
- Changing the pinned Slang version, or making `FetchSlang.cmake` itself offline-capable beyond what `SLANG_ROOT` already allows: none.
- Putting a Vulkan/GPU driver in the image or running GPU tests on a real GPU: none (lavapipe stays the CI path).
- Packaging a debug AppImage: not possible in the container (see scouted facts).
- Changing which tests PR CI runs, or its `-j 4` parallelism: the failure re-run only adds output.
- Reworking the ccache sharing scheme: none.

**Edge cases:**
- The image layer's Slang differs from the `FetchSlang.cmake` pin after a bump: the drift check fails loudly; the layer is rebuilt, and in the meantime configure still works because `SLANG_ROOT` is only a preference when the layer is stale and the check is a separate step.
- A container that has the image layer and also a seeded `.natron-deps/`: `SLANG_ROOT` (env) wins, per the existing resolution order, so nothing changes.
- A worktree under the main checkout: the bootstrap symlinks `.natron-deps` only if the main one exists, else leaves the default download path in place.
- `syntax-check` given a file with no compile-DB entry in a tree that predates the file's directory: it configures (CMake only) a scratch build dir and builds just the generated-header targets for it, and reports a hard `NO-COMMAND` failure if that still has no entry. It never silently skips.
- `.h`-only changes: the owning `.cpp` is checked if it exists, as today; a header with no sibling `.cpp` is reported as `SKIPPED (header, no owner)`, not omitted.
- Parallel ctest failure with no `ctest` log: the re-run is skipped when ctest reports no failures, and its exit status never masks the first run's status.
- Two builds must not run in one container at once; the bootstrap script and syntax-check take no part in building, and syntax-check refuses while a `ninja` process is running in the container.

**Assumptions:**
- Unconfirmed: github.com is reachable at `docker build` time, so the layer can fetch the same tarball and SHA256 that `FetchSlang.cmake` pins.
- Unconfirmed: `git submodule update` on a lane only needs to run once per lane, and a pinned `libs/OpenFX` branch survives while the opt-out is set.
- Unconfirmed: a CMake configure of the Engine GPU targets in a scratch build dir is fast enough (minutes, not tens of minutes) to run on demand.

## Phase 141.1: Slang in the image

- [ ] M141.P1.T1 — Slang layer in the natron-dev Dockerfile
  - files: `tools/ci/local/Dockerfile`
  - approach:
    - Add a `RUN` layer after the Mesa relink that downloads the Slang tarball pinned in `cmake/FetchSlang.cmake` (version, URL and SHA256 as `ARG`s with the current values), verifies the hash, and extracts it to a fixed in-image dir such as `/opt/slang`.
    - Set `ENV SLANG_ROOT=/opt/slang` (honoured by `FetchSlang.cmake` via `$ENV{SLANG_ROOT}`).
    - The build fails if `bin/slangc` or `include/slang-cpp-prelude.h` is absent.
    - Update the file's header comment: this is a second local-only layer, CI still downloads and caches Slang itself.
  - verify: `docker build` of the image succeeds, then in the container `slangc -v` runs and a fresh configure with no network and an empty `.natron-deps/` finds `SLANG_COMPILER` under the image path.
  - size: M

- [ ] M141.P1.T2 — Slang pin drift check
  - files: `tools/ci/local/check-slang-pin.sh` (new), `.github/workflows/checks.yml`
  - approach:
    - The script parses `NATRON_SLANG_VERSION` and `NATRON_SLANG_SHA256` out of `cmake/FetchSlang.cmake` and the `ARG` defaults out of `Dockerfile`, and exits non-zero naming both values when they differ.
    - Add it as a cheap step to `checks.yml` so a pin bump without a Dockerfile bump fails PR checks.
  - verify: run it on main (exit 0), then on a copy with a one-character SHA change (non-zero, message names both values); `checks.yml` still parses (`python3 -c 'import yaml'` load).
  - size: M

## Phase 141.2: Syntax checker in the repo

- [ ] M141.P2.T1 — Move syntax-check into tools/ and give it a file-path CLI
  - files: `tools/ci/local/syntax-check.py` (new), `build/syntax-check.py` (left alone: untracked)
  - approach:
    - Port the untracked script. Resolve the main checkout and the container name (`NATRON_DEV_CONTAINER`, default as in `devshell.sh`) from the script's location and the environment, with no hard-coded host paths.
    - CLI: `syntax-check.py [--root <checkout>] [--base <ref>] [--compdb <path>] [<file> ...]`. Explicit files (relative to `--root`, default the current checkout) are checked as given. With no files it uses `git diff --name-only <base> HEAD` plus the working-tree diff, `--base` defaulting to the merge base with `origin/main`. Reject `<root> <ref>` positional misuse with a usage error that shows the right form.
    - Keep the header-to-owning-`.cpp` mapping, and print `SKIPPED (header, no owner)` for a header it cannot map.
    - Exit non-zero on any `FAIL` or `NO-COMMAND`.
  - verify: run with an explicit `.cpp` in `Engine/` (OK), a file with a deliberate syntax error in a scratch worktree (FAIL with the error line), no arguments on a branch with a changed file, and `<root> <ref>` positionals (usage error).
  - size: M

- [ ] M141.P2.T2 — Include paths for test and third-party TUs
  - files: `tools/ci/local/syntax-check.py`
  - approach:
    - For a `Tests/**.cpp` TU with no DB entry, take the flags of a real `Tests` target entry and add the target's include dirs (`Tests`, `libs/xxhash`, gtest/gmock) by reading them from that same entry's `-I`/`-isystem` set, not from a hand list, so it follows `Tests/CMakeLists.txt:160`.
    - If a TU has no entry and no sibling can supply one, fail with `NO-COMMAND` and the directory searched; stop borrowing an Engine command for non-Engine code.
  - verify: syntax-check a new test file that includes `xxhash.h` and `CountingTestEffect.h` (not in the DB): OK; a test that includes a missing header: FAIL showing the header name.
  - size: M

- [ ] M141.P2.T3 — GPU-era compile database on demand
  - files: `tools/ci/local/syntax-check.py`, `tools/ci/local/README.md`
  - approach:
    - When a changed TU lives under `Engine/Gpu/` or includes a generated kernel header (`GpuKernel*`, `slang_add_kernel` outputs), and the compile DB is older than the newest `Engine/Gpu/Kernels/*` or `Engine/Gpu/CMakeLists.txt` change, run `cmake -S . -B build/syntax -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` (flags matching the `fast` preset in `build.sh`) inside the container and build only the Slang kernel-registry generation targets (`ninja -t targets` to name them), then use `build/syntax/compile_commands.json`.
    - Never write stub kernel headers; if the generated header is missing after that, fail with the missing path.
    - Refuse with a clear message if a `ninja` process is already running in the container (one build per container).
    - Document the flag-free usage in the README (see P5.T1).
  - verify: in a worktree whose tree predates M85, syntax-check an `Engine/Gpu/*.cpp` TU: it configures, generates the registry header and reports OK, with no stubs created (`git status` clean aside from the worktree's `build/`). With a deliberate error in a kernel-using TU it reports FAIL.
  - size: M

## Phase 141.3: Debug in remote builds, readable ctest failures

- [ ] M141.P3.T1 — Re-run failed ctest cases with -V
  - files: `tools/ci/local/test.sh`
  - approach:
    - When `NATRON_CTEST_JOBS` is greater than 1 and the parallel `ctest` run fails, run `ctest --rerun-failed -V` serially on the same build dir after it (same env, Xvfb and plugin paths), print a header line, and exit with the first run's non-zero status.
    - Do nothing extra for the serial path or for a passing run.
    - Update the usage comment block at the top of `test.sh`.
  - verify: run `NATRON_CTEST_JOBS=4 tools/ci/local/test.sh ctest debug` against a tree with one case forced to `assert()`-fail (temporary local edit, not committed): the output ends with the failing case's full `-V` log including the assert text, and the exit status is non-zero. A passing run prints no re-run section.
  - size: M

- [ ] M141.P3.T2 — `debug` preset for remote builds
  - files: `tools/ci/remote/build.sh`, `.github/workflows/remote-build.yml`
  - approach:
    - Add `debug` to the `preset` choice in the workflow and to the accepted values in `build.sh` (`:78`, the usage text, the error text), keeping `fast` the default.
    - `debug` forces `appimage=false` (the workflow ignores the input, and `build.sh` behaves as `--no-appimage` and says so), because a debug AppImage cannot be packaged in the container.
    - Choose the ccache size and key by preset so debug matches PR CI's `ccache-linux-debug-<branch>-` restore keys (read `ci.yml:59-114` for the exact scheme) instead of the fast/release sizes.
    - With `--tests`, set `NATRON_CTEST_JOBS: 4` for the `debug` preset to match PR CI, relying on P3.T1 for readable failures.
  - verify: dispatch a `debug --tests` remote build of a pushed branch: it configures `-DCMAKE_BUILD_TYPE=Debug`, runs ctest with `NATRON_CTEST_JOBS=4`, uploads `ctest-logs-debug-<sha>`, and downloads no AppImage; `build.sh fast` and `release` still work unchanged (dry-read of the dispatch arguments).
  - size: M

## Phase 141.4: Worktree bootstrap

- [ ] M141.P4.T1 — Track the appimagetool wrapper generator
  - files: `tools/ci/local/make-appimagetool-wrapper.sh` (new), `tools/ci/local/package.sh`
  - approach:
    - Read the existing wrapper's behaviour (`build/appimagetool-wrapper/appimagetool`, the 478-byte script that execs `appimagetool.real` with `-n`) and write a script that creates it in a given `build/appimagetool-wrapper/` dir from a real `appimagetool` (path argument or the copy already fetched by `release/make-appimage.sh`; read that script to find where it comes from).
    - `package.sh` calls it when the wrapper is missing and a real tool is available, otherwise falls back as today.
  - verify: remove a scratch copy of the wrapper dir (a literal path below `build/`), run `package.sh fast` on an existing fast tree: the wrapper is regenerated and an AppImage is produced; with no real tool present it falls back to the existing behaviour.
  - size: M

- [ ] M141.P4.T2 — Opt-out for build.sh's submodule update
  - files: `tools/ci/local/build.sh`
  - approach:
    - When `NATRON_SKIP_SUBMODULE_UPDATE=1`, skip the host-side `git submodule update --init --recursive` at `build.sh:188-196` and instead verify that each required submodule (`Tests/google-mock`, `Tests/google-test`, `libs/OpenFX`, `libs/SequenceParsing`) is checked out, failing with a clear message if one is not.
    - Default behaviour (update) is unchanged. Document the variable in the script header and in P5.T1.
  - verify: with the variable set and `libs/OpenFX` on a non-recorded branch commit, `build.sh` does not move it (`git -C libs/OpenFX rev-parse HEAD` unchanged); unset, it behaves as before; with the variable set and a submodule removed, it exits non-zero naming it.
  - size: M

- [ ] M141.P4.T3 — Worktree bootstrap script
  - files: `tools/ci/local/worktree-init.sh` (new)
  - approach:
    - `worktree-init.sh [<worktree-path>]` (default the current checkout), run from the host, does the following in order, each step idempotent:
      1. `git submodule update --init --recursive` in the worktree (nested submodules such as tinydir included);
      2. `tools/ci/local/fetch-assets.sh` for that worktree (its own `build/assets/`; skip when the pins already match, as `fetch-assets.sh` does);
      3. links `.natron-deps` to the main checkout's directory when the worktree is not the main checkout and the main one exists;
      4. copies or regenerates `build/appimagetool-wrapper/` from the main checkout or via P4.T1;
      5. prints a summary of what was done or already present.
    - It finds the main checkout with `git rev-parse --git-common-dir`, never a literal path, and never builds.
    - Pin the `libs/OpenFX` fork branch (the worktree's recorded commit) and tell the user to export `NATRON_SKIP_SUBMODULE_UPDATE=1` for lane builds.
  - verify: create a scratch worktree under `.worktrees/`, run the script, run it again (second run reports everything present, no downloads), then `tools/ci/local/build.sh fast --reconfigure` configures there without network use for Slang. Remove the scratch worktree with `git worktree remove`.
  - size: M

## Phase 141.5: Documentation

- [ ] M141.P5.T1 — Document the local environment
  - files: `tools/ci/local/README.md`, `tools/ci/remote/README.md`
  - approach:
    - `local/README.md`: add sections for the Slang layer (what `SLANG_ROOT` is, the drift check, that CI still downloads), "Checking syntax before a build" (the new tool's CLI, GPU-era trees, one build per container), "Worktree lanes" (`worktree-init.sh`, `NATRON_SKIP_SUBMODULE_UPDATE`), and the failed-case `-V` re-run in the `test.sh` section.
    - `remote/README.md`: list `debug` among the presets, say it never packages an AppImage, and say its ctest runs at PR CI's parallelism with failed cases re-run `-V`. Replace the "presets (`fast`, `release`)" sentence.
    - Machine-neutral wording only: no host names or home paths.
  - verify: every command line in the new sections runs as written (copy-paste each in a scratch worktree) and every named file exists; `git grep -n '/home/'` over the two READMEs finds nothing.
  - size: S

## Decisions

- 2026-10-21 — **Claimed despite the file-overlap test (user-directed):** the user asked to run this milestone and M155 - Per-Suite Test Processes in parallel lanes. Overlapping files: `tools/ci/local/README.md` with M107 - GPU Viewer Hand-off; `tools/ci/local/test.sh` with the parked M64 - Tiled Rendering; and `tools/ci/local/test.sh`, both READMEs and `remote-build.yml` with M155. Conflicts are resolved at merge.

**Verification gate:**
- A fresh container from the rebuilt image has `slangc` on the path and configures with no network and an empty `.natron-deps/`; `check-slang-pin.sh` passes on main and fails on a deliberate pin mismatch.
- `tools/ci/local/syntax-check.py` is tracked, takes file paths, checks a test TU and a GPU TU in a pre-M85 worktree correctly without stub headers, and never silently skips.
- `NATRON_CTEST_JOBS=4 tools/ci/local/test.sh ctest debug` shows the assert text of a deliberately failing case, with a non-zero exit.
- A pushed branch builds through `tools/ci/remote/build.sh debug --tests` with Debug flags, `-j 4` ctest and no AppImage; `fast` and `release` still work.
- `worktree-init.sh` brings a new worktree to a configurable, packageable state (submodules, assets, `.natron-deps`, wrapper) and is idempotent on a second run.
- The READMEs contain no machine-local paths or hostnames.
