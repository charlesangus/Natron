# Local CI reproduction

Before this directory existed, the only way to run Natron's test suite was to
push a commit and wait for a full cold build on GitHub Actions -- tens of
minutes per iteration. Debugging a crash meant pushing `ci(temp)` diagnostic
commits just to read a stack trace.

These scripts reproduce the CI build environment locally with a persistent
build tree, so a one-file edit rebuilds in seconds and `ctest`/`smoke_test.py`/
`gdb` run on demand instead of round-tripping through Actions.

Follow this file top to bottom from a clean clone and you'll reach a passing
`test.sh ctest`.

## Prerequisites

- A working Docker daemon. If you are inside a container yourself, the host
  needs something like sysbox so Docker can nest.
- Disk: the base image is ~12.5 GB, and a debug build tree is ~4.1 GB.

## 1. Build the image

`Dockerfile` is `FROM aswf/ci-baseqt:2027.0` -- the exact tag
`.github/workflows/ci.yml` pins -- and adds no layers, because that base
image already ships everything CI's `dnf install` step provides. See the
comments at the top of `Dockerfile` for the full package-by-package
breakdown and the one documented gap (`extra-cmake-modules`, below).

```
docker build -t natron-dev:2027.0 tools/ci/local/
```

### If the pull fails partway through

In network-restricted environments, Docker Hub's blob CDN
(`production.cloudfront.docker.com`) can be unreachable even when the
registry API works, so `docker pull`/`docker build` authenticates and then
fails on the first blob. Workaround, verified to give a byte-identical image:

```
docker pull mirror.gcr.io/aswf/ci-baseqt:2027.0
docker tag mirror.gcr.io/aswf/ci-baseqt:2027.0 aswf/ci-baseqt:2027.0
```

Then re-run the `docker build` above; it will resolve `FROM` locally.

## 2. Fetch test assets (one time)

Downloads the OCIO configs and OFX plugins CI also downloads on every run,
into `build/assets/`. Idempotent -- safe to re-run, it skips anything already
present.

```
tools/ci/local/fetch-assets.sh
```

## 3. Build

```
tools/ci/local/build.sh [debug|release] [--reconfigure]
```

`debug` (default) builds `build/debug`; `release` builds `build/release`
with no explicit `-DCMAKE_BUILD_TYPE`, matching CI's release job exactly.
Configure only runs once, when `CMakeCache.txt` doesn't exist yet; pass
`--reconfigure` after editing `CMakeLists.txt` or similar to force it again.
This is also what re-execs you into the dev container (see "Getting a
shell" below) -- you don't need to start one yourself first.

## 4. Test

```
tools/ci/local/test.sh <ctest|smoke> [debug|release] [--gdb]
```

- `ctest` runs `ctest -V` against the build dir, exactly as CI does.
- `smoke` locates the built `NatronRenderer` and runs
  `tools/ci/smoke_test.py` through it. This script only exists on branches
  that carry the M2-era `tools/ci/smoke_test.py` addition -- it is not on
  `RB-2.6`, and `test.sh` will tell you plainly and exit if it's missing on
  your checkout.
- `--gdb` applies only to `smoke` (`ctest` runs many binaries, not one): it
  runs the target under
  `gdb -batch -ex run -ex "thread apply all bt" -ex bt --args <binary> ...`
  so a crash gives you an immediate backtrace instead of a bare exit code.
  This is the whole point of this script -- it replaces a
  `ci(temp)`-diagnostic-commit-and-push cycle with something that runs
  locally in seconds. Worked example, produced in seconds where this
  previously took a push-and-wait cycle:

  ```
  $ tools/ci/local/test.sh smoke --gdb
  ...
  Program received signal SIGSEGV, Segmentation fault.
  0x00000000007d60a9 in Natron::getFBConfigAttrib (glxInfo=0xa865960,
      fbconfig=<error reading variable: Cannot access memory at address 0x0>,
      attrib=32785) at Engine/OSGLContext_x11.cpp:433
  ```

## Getting a shell

```
tools/ci/local/devshell.sh
```

With no arguments this drops you into an interactive shell in the persistent
dev container, creating it on first use. With arguments it runs a command
inside the container and propagates its exit code -- that's how `build.sh`
and `test.sh` reach the container themselves; you don't need to run
`devshell.sh` before them.

## Where things live, and how to reset them

| What | Where | Reset |
|---|---|---|
| Build tree | `build/debug`, `build/release` (gitignored) | delete the directory |
| Test assets | `build/assets` (gitignored) | delete, then re-run `fetch-assets.sh` |
| ccache, `HOME` | Docker named volumes `natron-dev-ccache`, `natron-dev-home` | `docker volume rm` |
| Container | `natron-dev` | `docker rm -f natron-dev`, or see below |

`CCACHE_MAXSIZE` defaults to `40G`, overridable via env var. The container
name is overridable via `NATRON_DEV_CONTAINER` -- use this to give a second
worktree its own container and caches, e.g.
`NATRON_DEV_CONTAINER=natron-dev-wt2 tools/ci/local/devshell.sh`.

## Two gotchas

- **Editing `devshell.sh` does not affect an already-running container.**
  Environment (image, mounts, env vars) is only applied at container
  creation. After changing `devshell.sh` or the `Dockerfile`, run
  `tools/ci/local/devshell.sh --recreate` to pick it up. Caches (ccache,
  `HOME`) survive a recreate; for a full teardown, also
  `docker volume rm natron-dev-ccache natron-dev-home`.

- **`bash -l` inside the container prints noise you should ignore.** An
  interactive `devshell.sh` shell prints a large NVIDIA/CUDA banner on
  stdout (once, at container start) and `id: cannot find name for user ID
  1000` on stderr (because the image has no passwd entry for the invoking
  uid). Both are harmless artifacts of the base image, but they will confuse
  anyone trying to parse script output.

## Keeping this in sync with CI

The entire point of this directory is that it reproduces CI closely enough
to trust locally reproduced failures. That only holds if it's kept in sync:

- `Dockerfile`'s documented package list must be kept in sync with
  `.github/workflows/ci.yml`'s "Install Linux system packages" step.
- `test.sh`'s environment (`OFX_PLUGIN_PATH`, `OCIO`, running everything
  under `xvfb-run`) must stay in sync with what `ci.yml` sets for the test
  steps.

If a local result disagrees with CI and you haven't touched either file
recently, check here before assuming the local environment is broken.

## Known divergences from CI

- **Wayland support is off locally, on in CI.** `extra-cmake-modules` (ECM)
  is not installed in this image -- no distro package repository is
  reachable in the environment this image was built in. Without it,
  `CMakeLists.txt:114`'s `find_package(ECM NO_MODULE)` fails, so
  `CMakeLists.txt:118`'s `find_package(Wayland COMPONENTS Client Egl)` never
  resolves (ECM ships the `FindWayland.cmake` module it needs). Neither
  `find_package` call is `REQUIRED`, so nothing breaks the build -- it's a
  silent difference, not a failure. Rule this out first if a local result
  disagrees with CI in anything Wayland-related. See `Dockerfile` for how to
  close the gap (a from-source ECM build was prototyped and confirmed to
  work).
- **Ninja locally, make in CI.** Deliberate, for build speed.
- **`build.sh` passes `-DCMAKE_PREFIX_PATH=/usr/local`.** `RB-2.6`'s
  `ci.yml` omits this entirely; the `ci-smoke-test-m2p3t1a` branch's
  `ci.yml` passes it too. Where CI omits it, it's a no-op locally.
- **`test.sh smoke` needs `tools/ci/smoke_test.py`**, which only exists on
  branches carrying the M2-era smoke test addition -- not on `RB-2.6`.

## Measured performance

- Full cold build: ~28 minutes on 4 cores.
- No-op rebuild: 0 seconds.
- One-file rebuild (touching `Gui/SpinBox.cpp`): 13 seconds.
