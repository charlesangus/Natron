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

- UAT: a `fast` AppImage (-O1) for GUI checks.
- Debug: the `debug` preset, the one PR CI runs, with live asserts. It never
  packages an AppImage: llvmpipe raises SIGFPE under the `-DDEBUG` traps in
  the container, so `debug` behaves as `--no-appimage`. Use it with
  `--tests` to reproduce a PR CI failure.
- Performance: a `release` AppImage (-O2), measured on the target host. Never take performance numbers from a GitHub runner.

`stage-bundle.sh` strips the ELF files of every preset's bundle, `fast`
included, not only `release`. Set `NATRON_STAGE_NO_STRIP=1` to opt out.

## Usage

```
tools/ci/remote/build.sh <fast|release|debug> [--tests] [--record-goldens] [--shuffle-seeds <n,n,...>] [--no-appimage]
```

- `--tests` also runs the ctest suite and the Python bindings smoke test on
  the runner. Without it the build skips tests. ctest runs at 4 jobs
  (`NATRON_CTEST_JOBS=4`, one suite per vCPU), as in PR CI, and cases that
  fail are re-run serially with `ctest --rerun-failed -V`, so the logs carry
  the full output and assert text of each failure.
- `--record-goldens` implies `--tests`. It also records the native golden
  renders on the runner and downloads them into
  `Tests/fixtures/native-golden/` of this checkout.
- `--shuffle-seeds <n,n,...>` implies `--tests`, e.g. `--shuffle-seeds 1,2,3`
  (seeds 1 to 99999). Each ctest leg runs once unshuffled, as without the
  option, then once per seed with `GTEST_SHUFFLE=1 GTEST_RANDOM_SEED=<seed>`,
  which shuffles the cases within each suite process to flush out leaks
  between cases. Every seed runs even after a failure, the leg fails if any
  run did, and each shuffled run's log is `ctest-<scheduler>-seed<seed>.log`.
  The ctest step's timeout rises from 30 to a fixed 120 minutes, about five
  seeds on the debug preset, so at most 5 seeds are accepted; `build.sh` and
  the workflow both reject a longer list.
- `--no-appimage` skips packaging and downloading the AppImage. `debug`
  implies it.
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
| `natron-<preset>-<sha>` | the `.AppImage`, made executable |
| `ctest-logs-<preset>-<sha>` | the ctest logs, `ctest-legacy.log` and `ctest-taskgraph.log`, plus `ctest-<scheduler>-seed<seed>.log` per shuffle seed (`--tests` only), uploaded even when the run fails |

Both land in `build/remote/<preset>/`, which is wiped at the start of each
download. The script prints the run URL, the `AppImage:` path and, with
`--tests`, the `ctest logs:` directory. It exits 0 only when the run
succeeded and, if requested, the AppImage was downloaded; otherwise it exits
with the run's failure status (or 1).

A push to `main` also triggers the workflow as a `fast` build with no tests
and no AppImage. It only keeps the ccache warm for branch runs.

## GUI checks in remote mode

Run GUI scripts inside the natron-dev container (through the main
`tools/ci/local/devshell.sh`), against the downloaded AppImage under
`build/`, under Xvfb:

```
Tests/gui/run-gui-test.sh Tests/gui/<script>.py build/remote/fast/<name>.AppImage
```

`run-gui-test.sh` extracts the AppImage once and launches its `AppRun`, so no
FUSE is needed. It needs the container's `/usr/lib64` GL, EGL, Mesa and LLVM
libraries (it exits 2 if one is missing), plus `Xvfb` and `xset`.
