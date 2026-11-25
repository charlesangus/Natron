# Remote builds

`tools/ci/remote/build.sh` runs a Natron build on GitHub Actions through the
dispatch-only `.github/workflows/remote-build.yml` workflow and downloads the
results into `build/remote/<preset>/`. It is the remote twin of
`tools/ci/local/build.sh`; the presets (`fast`, `release` and `debug`) are the
ones documented in `tools/ci/local/README.md`.

The jobs run in the published dev image,
`ghcr.io/charlesangus/natron-dev:<content hash>`, which the workflow's
`dev-image` job builds and pushes first when that tag is not yet in the
registry. The tag is a hash of the `Dockerfile` and the Slang and glslang
pins, so it is the same image `tools/ci/local/devshell.sh` pulls (or builds
locally when the pull fails). Slang and glslang come from the image
(`SLANG_ROOT`, `GLSLANG_ROOT`), so a run downloads neither. See "Published
image" in `tools/ci/local/README.md`.

## Local or remote

Remote is the default. GitHub-hosted runners are free for this public repo and
run about 20 jobs concurrently, 4 vCPU each. Remote therefore gains
parallelism (several lanes building at once, a slow host stays idle), not
per-build speed.

Use local builds on a powerful host, or for tight single-file iteration where
a persistent build tree rebuilds in seconds (see `tools/ci/local/README.md`).

In both modes, batch one build per phase rather than building after every edit.

## Selecting the mode per host

`.local/build-mode` at the repo root holds `local` or `remote`. The first
non-blank line that does not start with `#` is the value; a missing file means
`remote`. Any other value is an error. `.local/` is gitignored.

```
tools/ci/remote/build.sh --mode    # prints the resolved mode
```

`--mode` takes no other arguments.

## Two build kinds

- UAT: a `fast` AppImage (-O1) for GUI checks, built with `--appimage`.
- Debug: the `debug` preset, the one PR CI runs, with live asserts. It never
  packages an AppImage: llvmpipe raises SIGFPE under the `-DDEBUG` traps in
  the container, so `debug` takes neither `--appimage` nor `--gui`. Use it with
  `--tests` to reproduce a PR CI failure.
  PR CI (`ci.yml`) runs only for a PR that is ready for review: a draft gets
  no CI until it is marked ready (`gh pr ready`), which starts the full run.
  On a draft only the `Build and test` check runs, and it fails on purpose,
  because a skipped required check would count as passing.
- Performance: a `release` AppImage (-O2), measured on the target host. Never take performance numbers from a GitHub runner.

`stage-bundle.sh` strips the ELF files of every preset's bundle, `fast`
included, not only `release`. Set `NATRON_STAGE_NO_STRIP=1` to opt out.

## Usage

```
tools/ci/remote/build.sh <fast|release|debug> [--tests] [--ctest-regex <re>] [--record-goldens] [--shuffle-seeds <n,n,...>] [--appimage] [--no-remote-ccache] [--debug-symbols] [--cmake-arg <arg>]... [--gui "<script> ..."]
tools/ci/remote/build.sh <fast|release> --from-run <run-id> --gui "<script> ..."
```

- `--tests` also runs the ctest suite and the Python bindings smoke test on
  the runner. Without it the build skips tests. ctest runs at 4 jobs
  (`NATRON_CTEST_JOBS=4`, one suite per vCPU), as in PR CI, and cases that
  fail are re-run serially with `ctest --rerun-failed -V`, so the logs carry
  the full output and assert text of each failure.
- `--scheduler <legacy|taskgraph>` implies `--tests` and runs only that
  scheduler's ctest leg and smoke test; without it both legs run, as before.
  Intermediate fix builds may use `--scheduler taskgraph` to halve the wait,
  but a milestone's verification gate runs without it, so both legs are
  green. The cross-scheduler smoke render comparison runs only when both legs
  did, and an unknown name exits with the usage line before dispatching.
  The workflow takes it as the `schedulers` input, a comma-separated list
  that defaults to `legacy,taskgraph`.
- `--ctest-regex <re>` implies `--tests` and makes each ctest leg run only the
  ctest entries matching `<re>` (`ctest -R`, through `test.sh`'s
  `NATRON_CTEST_REGEX`; entries are whole suites, e.g. `'^NativeBlurTest$'`).
  A regex matching no entry fails the leg, and the smoke test still runs.
  It is for intermediate fix builds only: a milestone's final verification
  gate must not use it, so the full suite is green. The workflow takes it as
  the `ctest_regex` input, sent only when given.
- `--record-goldens` implies `--tests`. It also records the native golden
  renders of `NativeGoldenTest`, `NativeIDistortAmountTest` and
  `NativeIWarpTest` on the runner and downloads them into
  `Tests/fixtures/native-golden/` of this checkout. The resampling parity
  references of six cases, `NativeTransformTest` (`CubicMinifyingAndRotated`,
  `MaskedMixHalf`, `SkewYX`) and `NativeReformatTest` (`FitIntoAFixedBox`,
  `ToFormatWithAPixelAspectRatio`, `ToProjectFormatFromTheSourceRoD`), record
  the same way into `Tests/fixtures/native-parity/`; the other parity
  references are left alone.
  Every `Native*Test.Regression*` case also records its parity reference, which
  comes down under `Tests/fixtures/native-golden/native-parity/`; copy a
  node's directory to `Tests/fixtures/native-parity/` to commit it.
- `--shuffle-seeds <n,n,...>` implies `--tests`, e.g. `--shuffle-seeds 1,2,3`
  (seeds 1 to 99999). Each ctest leg runs once unshuffled, as without the
  option, then once per seed with `GTEST_SHUFFLE=1 GTEST_RANDOM_SEED=<seed>`,
  which shuffles the cases within each suite process to flush out leaks
  between cases. Every seed runs even after a failure, the leg fails if any
  run did, and each shuffled run's log is `ctest-<scheduler>-seed<seed>.log`.
  The ctest step's timeout rises from 30 to a fixed 120 minutes, about five
  seeds on the debug preset, so at most 5 seeds are accepted; `build.sh` and
  the workflow both reject a longer list.
- `--appimage` packages an AppImage and downloads it. A run packages only
  with `--appimage`, `--gui` or `--debug-symbols` (both imply it), so a gate
  that only runs ctest skips packaging. Pass it for UAT builds and for a run
  meant as a later `--from-run` source. `--no-appimage` names the default and
  is rejected beside any of the three. `debug` takes none of them.
  Packaging runs in its own `package` job off the uploaded build tree, beside
  the ctest legs, so ctest does not wait for it.
- `--cmake-arg <arg>` (repeatable) adds an argument to the cmake configure,
  e.g. `--cmake-arg -DNATRON_GPU=OFF`. `tools/ci/local/build.sh` takes the same
  flag and then builds in its own `build/<preset>-<hash>` tree instead of the
  preset's. Remotely the args travel as the workflow's space-separated
  `cmake_args` input, so `build.sh` rejects an arg containing whitespace, and
  the run name gains a `cmake:<args>` marker.
- The ctest suite has one entry per gtest suite, not per case, so `ctest -R`
  matches suite names (`'^NativeGoldenTest$'`). To run a single case, call the
  test binary with `--gtest_filter=Suite.Case`.

HEAD must already be pushed: the workflow builds the pushed commit, not the
working tree. The script refuses on a detached HEAD, or when HEAD differs from
`origin/<current branch>`, and it never pushes. Uncommitted changes only
produce a warning.

The script dispatches the workflow, finds its run by a token echoed into the
run name, watches it, then downloads the artifacts (kept 7 days on GitHub):

| Artifact | Contents |
| --- | --- |
| `natron-<preset>-<sha>` | the `.AppImage`, made executable (`--appimage`, `--gui` or `--debug-symbols` only) |
| `ctest-logs-<preset>-<sha>` | the ctest logs, `ctest-legacy.log` and `ctest-taskgraph.log` (only the chosen leg's with `--scheduler`), plus `ctest-<scheduler>-seed<seed>.log` per shuffle seed (`--tests` only), uploaded even when the run fails |

Both land in `build/remote/<preset>/`, which is wiped at the start of each
download. The script prints the run URL, the `AppImage:` path and, with
`--tests`, the `ctest logs:` directory. It exits 0 only when the run
succeeded and, if requested, the AppImage was downloaded; otherwise it exits
with the run's failure status (or 1).

A push to `main` also triggers the workflow as a `fast` build with no tests
and no AppImage. It only keeps the ccache warm for branch runs.

## Polling and the shared run watcher

The core API quota is 5,000 calls an hour, shared by every gate on the
account, so the script polls sparingly:

- `NATRON_REMOTE_WATCH_INTERVAL_SECS` (default 60): seconds between status
  polls of the run, one API call each. A line is printed when the run's status
  changes (`queued`, `in_progress`, `completed`).
- `NATRON_REMOTE_FIND_INTERVAL_SECS` (default 10): seconds between looks for
  the dispatched run, and between artifact download retries.

With many gates in flight, set `NATRON_REMOTE_RUN_STATE_DIR` to a directory
shared by all of them (an absolute path, for example `$HOME/git/Natron/build/remote/run-state`; `build.sh` resolves a relative one against the directory it was started from, so every worktree would otherwise get its own watcher). The first
`build.sh` that finds no watcher starts `watch-runs.sh <dir>` detached. It
polls every recent `remote-build.yml` run with one `gh run list` call per
watch interval, whatever the number of gates, and writes `<dir>/<run-id>`
(`<status> <conclusion>`), `<dir>/titles.tsv` (`<run-id>`, head sha, run name)
and `<dir>/heartbeat`. Each `build.sh` finds its run in `titles.tsv`, waits on
its state file and registers in `<dir>/clients/<pid>` while it runs. A flock
on `<dir>/watcher.lock` keeps one watcher per directory; its log is
`<dir>/watcher.log`. The watcher exits after 10 minutes with no unfinished run
and no live client (`NATRON_REMOTE_WATCHER_IDLE_SECS` changes this). If the
heartbeat is older than three intervals, or the run has left the watcher's
list, `build.sh` polls the run itself and tries to restart the watcher.
The heartbeat is written only after a successful poll (the `gh` call has a
timeout of twice the interval) and removed when the watcher exits; state files
of runs that left the list and stale temp files are pruned each poll. Clients
and the watcher must share a host and PID namespace, and one state dir serves
one repo. While waiting, `build.sh` prints "still <status>" every 10 minutes.
Without the variable, each `build.sh` polls its own run.

## Shared remote ccache

The build job layers a shared ccache remote storage server on top of the
`actions/cache` ccache restore. The repo secret `CCACHE_REMOTE_STORAGE` holds
the URL only, in the form `https://USER:PASS@cache.example.com`, without a
`url=` key or quotes. The workflow tolerates a leading `url=`, quotes and
surrounding whitespace, but a value that is not an `http://` or `https://`
URL turns the remote cache off for that run (the log says only
`remote ccache: unsupported value, off`), as does a value ccache rejects. The
whole value and its host are masked in the log. The secret may also be the
whole local `.local/ccache-remote` file (blank and `#` lines are skipped, only
`url=` is used, and `network=`, `docker_url=` and other keys are ignored). The workflow
replaces any `layout=` attribute on it with the bazel layout the local builds
use (`@layout=bazel` for the dev image's `ccache-storage-https` helper, which
handles the HTTPS), so both spellings work and CI shares keys with local
builds. It is on for pushes and for
dispatches by default; ccache stats at the end of the build show a "Remote
storage" block.

To build without it, dispatch with `--no-remote-ccache`, or set the
`remote_ccache` workflow input to false:

```
tools/ci/remote/build.sh fast --no-remote-ccache
gh workflow run remote-build.yml --ref <branch> -f remote_ccache=false
```

Runs where the secret is empty, such as fork PRs, and runs where the server is
down or slow, build with the local ccache only; the build never fails because
of the cache.

A branch whose history predates remote storage support never writes to it, so
builds of objects only such branches have compiled find nothing remotely (0
remote hits) until those changes reach a branch that has remote storage.
Merging main into the branch fixes it. Presets differ too: most gates build
`fast`, so a first `release` build of a change mostly misses.

## GUI checks in remote mode

`--gui "<script> ..."` runs `Tests/gui/` scripts on GitHub Actions against the
run's AppImage, in at most 4 parallel jobs (`GUI · group <i> of <n>`) that each
run a round-robin share of the scripts one after another, so a gate leaves
runner slots free for others. It implies `--appimage`, so not with `debug`:

```
tools/ci/remote/build.sh fast --gui "Tests/gui/m55_node_graph.py Tests/gui/m45_tab_menu.py"
tools/ci/remote/build.sh release --from-run 12345678901 --gui "Tests/gui/m55_node_graph.py"
```

Each job runs `Tests/gui/run-gui-test.sh` per script in the dev image with a
1920x1080 screen, a 900 s Natron timeout and a 25-minute limit on the whole
script, each on its own Xvfb display, and uploads each script's
`NATRON_GUI_TEST_OUT` (screenshots, `results.txt`, `natron.log`, plus the
runner's output in `run.log` and its exit code in `exit-status`) as
`gui-<name>-<preset>-<sha>`, even when the script fails. A failing or
timed-out script does not stop the rest of its group; the job fails if any of
its scripts did. A job has 16 upload steps, so a list of more than 64 scripts
is spread over more than 4 jobs. Git LFS fixtures are fetched only when a
script of the group names them (by file name), so most groups download few.

`--from-run <run-id>` skips the build and uses the `natron-<preset>-<sha>`
AppImage of an earlier `remote-build` run (artifacts last 7 days). Only a run
dispatched with `--appimage` or `--gui` has one; `build.sh` checks before
dispatching and fails with the AppImages the run does have. The GUI
jobs check out that run's commit, so the scripts and fixtures, the
`run-gui-test.sh` and the dev image are the ones of the branch under test,
which need not carry this workflow itself: dispatch from a branch that does.
`--gui-ref <ref>` (only with `--from-run`) checks out that branch or sha for the
scripts, fixtures and `run-gui-test.sh` instead of the run's commit, still
testing the run's AppImage under the dev image of the run's commit.
`--from-run` takes no build or test options, and replaces only
`build/remote/<preset>/gui/`.

The outputs land in `build/remote/<preset>/gui/<name>/`. The script prints a
`gui: PASS|FAIL <script> (<n> passed, <n> failed): <dir>` line per script,
judged by its `exit-status` with the counts from `results.txt`, and exits
non-zero if any script failed.

To run a script locally instead, run it inside the natron-dev container
(through the main `tools/ci/local/devshell.sh`), against the downloaded AppImage under
`build/`, under Xvfb:

```
Tests/gui/run-gui-test.sh Tests/gui/<script>.py build/remote/fast/<name>.AppImage
```

`run-gui-test.sh` extracts the AppImage once and launches its `AppRun`, so no
FUSE is needed. It needs the container's `/usr/lib64` GL, EGL, Mesa and LLVM
libraries (it exits 2 if one is missing), plus `Xvfb` and `xset`.
