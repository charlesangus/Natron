# Milestone 7: Local incremental builds

`~1 day` · unblocks everything else. Right now the only way to run the test
suite is to push and wait for a full cold build on GitHub Actions — the last
five commits on this branch are all `ci(temp)` diagnostics bolted onto
`.github/workflows/ci.yml` purely to read a stack trace. That loop is measured
in tens of minutes per iteration and it is the reason M2.P3.T1a has stalled.

This milestone stands up the same environment CI uses, locally, as a
long-lived container with a persistent build tree — so a one-file edit
rebuilds in seconds and `ctest` / `smoke_test.py` / `gdb` run on demand. It
changes no product code and no CI behaviour; it only adds tooling under
`tools/ci/local/`.

**Fidelity is the whole point.** The local container must match
`aswf/ci-baseqt:2027.0` — the exact tag `.github/workflows/ci.yml` pins — and
run the same package-install, OCIO-config, and plugin-download steps. A local
environment that differs from CI cannot reproduce CI failures, which is the
one thing it exists to do.

## Phase 7.1: Stand up the container image

- [ ] M7.P1.T1 — Confirm nested Docker works and pull the CI base image
  - files: none (environment check only; record findings in this file's
    `## Decisions`)
  - approach: this session already runs inside a sysbox container with a
    reachable Docker daemon (`docker version` reports server 29.7.2, default
    runtime `runc`, no images pulled yet). Confirm `docker run --rm
    aswf/ci-baseqt:2027.0 gcc --version` works, then record the image's
    resolved digest so later drift is detectable. Budget disk: ~128 GB free,
    and the aswf Qt base image is large. Note that a bind mount of the repo
    resolves against *this* container's filesystem, since the inner daemon
    shares it — mount `/home/bosley/git/Natron` directly, no host path
    translation.
  - verify: `docker run --rm aswf/ci-baseqt:2027.0 gcc --version` prints gcc
    14.x; `docker images --digests` shows the pinned digest; `df -h` still
    shows headroom for two build trees.
  - size: S

- [ ] M7.P1.T2 — `tools/ci/local/Dockerfile` layering the CI setup steps onto the base
  - files: `tools/ci/local/Dockerfile`
  - approach: `FROM aswf/ci-baseqt:2027.0`, then a single `RUN dnf install -y`
    carrying **exactly** the packages `.github/workflows/ci.yml`'s "Install
    Linux system packages" step installs (`cairo-devel epel-release wget unzip
    clang xorg-x11-server-Xvfb`, then `extra-cmake-modules`), plus the
    local-only additions the CI job does not need: `ccache`, `ninja-build`,
    `gdb`. Baking these into an image layer is what removes the per-run dnf
    cost. Keep the CI package list verbatim and clearly commented as
    "must match ci.yml" so the two do not silently diverge.
  - verify: `docker build -t natron-dev:2027.0 tools/ci/local/` succeeds;
    `docker run --rm natron-dev:2027.0 bash -lc 'ccache --version && ninja
    --version && gdb --version && which Xvfb'` prints all four.
  - size: S

- [x] M7.P1.T3 — One-time fetch of OCIO configs and OFX plugins into a cached dir
  - files: `tools/ci/local/fetch-assets.sh`, `.gitignore`
  - approach: CI re-downloads `OpenColorIO-Configs` (v2.5) and the
    `openfx-io` testing build on every run; locally these need fetching once.
    Script downloads and unpacks both into a gitignored `build/assets/`
    (`OpenColorIO-Configs/` and `Plugins/`), skipping work if already present,
    using the same URLs and `OCIO_CONFIG_VERSION=2.5` as `ci.yml`. Add
    `build/assets/` to `.gitignore` if the existing `build` rule does not
    already cover it.
  - verify: running the script twice — second run is a no-op;
    `build/assets/OpenColorIO-Configs/blender/config.ocio` and
    `build/assets/Plugins/` both exist; `git status` is clean.
  - size: S

## Phase 7.2: The dev loop

- [ ] M7.P2.T1 — `devshell.sh`: long-lived container with persistent caches
  - files: `tools/ci/local/devshell.sh`
  - approach: start (or `exec` into, if already running) a named container
    `natron-dev` from `natron-dev:2027.0`, with the repo bind-mounted at the
    same absolute path it has outside, a named volume for `ccache`, and the
    container kept alive across invocations. Run as the invoking uid/gid so
    build artifacts are not root-owned. Set `CCACHE_DIR` to the volume and
    export the same `CI`/`PYTHON_VERSION`/`OCIO_CONFIG_VERSION` env `ci.yml`
    sets. Long-lived is the point: container start cost is paid once, not per
    build.
  - verify: `tools/ci/local/devshell.sh` drops into a shell; `touch /tmp/x`
    inside, exit, re-run — the file is still there (same container, not a
    fresh one); files created in the repo from inside are owned by the
    invoking user, not root.
  - size: M

- [ ] M7.P2.T2 — `build.sh`: incremental CMake+Ninja build with ccache
  - files: `tools/ci/local/build.sh`
  - approach: configure once into `build/debug` (or `build/release` via an
    argument) with `-G Ninja`, `-DCMAKE_BUILD_TYPE=Debug`,
    `-DCMAKE_PREFIX_PATH=/usr/local` (matching `ci.yml`),
    `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`,
    and `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. Re-running skips configure when
    the cache exists. Build with `ninja -j$(nproc)` (4 here, versus CI's
    `make -j2`). Runs inside the dev container via `devshell.sh`, so a bare
    `tools/ci/local/build.sh` from the repo root Just Works.
  - verify: first invocation configures and builds; second invocation with no
    edits finishes in seconds and reports zero recompiles; `ccache -s` shows
    a growing hit rate.
  - size: M

- [ ] M7.P2.T3 — `test.sh`: ctest and the Python smoke test, with a gdb mode
  - files: `tools/ci/local/test.sh`
  - approach: wrap the two things CI runs, with the env `ci.yml` uses —
    `OFX_PLUGIN_PATH` and `OCIO` pointing at `build/assets/` from M7.P1.T3,
    everything under `xvfb-run --auto-servernum`. Subcommands: `ctest` (runs
    `ctest -V` in the chosen build dir) and `smoke` (runs `smoke_test.py`
    through the built `NatronRenderer`, locating the binary the same way
    `ci.yml` does). A `--gdb` flag runs the smoke target under
    `gdb -batch -ex run -ex bt` — replacing the `ci(temp)` gdb commit on this
    branch with something runnable in seconds.
  - verify: `test.sh ctest` produces the same pass/fail set as the last CI
    run; `test.sh smoke --gdb` on the current tree prints a backtrace rather
    than requiring a push.
  - size: M

## Phase 7.3: Prove it and write it down

- [ ] M7.P3.T1 — Measure cold vs. incremental, record the numbers
  - files: this milestone file's `## Decisions`
  - approach: time a cold build (empty ccache), then a warm rebuild after
    touching one `.cpp` in `Gui/`, then a no-op rebuild. Record all three
    against the CI job's wall-clock time for comparison. If the incremental
    rebuild is not dramatically faster than a CI round-trip, something is
    misconfigured — investigate before closing the milestone.
  - verify: three timings recorded; the one-file rebuild is under a minute.
  - size: S

- [ ] M7.P3.T2 — Reproduce the M2.P3.T1a smoke-test failure locally
  - files: none (validation only; findings go to M2's `## Decisions`)
  - approach: this is the milestone's real acceptance test. Run
    `test.sh smoke --gdb` against the current branch and confirm it exhibits
    the same failure the CI diagnostics were chasing. If it reproduces, the
    local loop is trustworthy and M2 can resume against it. If it does *not*
    reproduce, the local environment differs from CI somewhere — find where
    before declaring the milestone done, since a loop that cannot reproduce
    CI failures does not solve the problem this milestone exists for.
  - verify: the failure reproduces locally with a usable backtrace, or the
    environment divergence is identified and fixed.
  - size: M

- [ ] ~~M7.P3.T3 — Strip the `ci(temp)` diagnostics from the CI workflow~~
  - **Cancelled 2026-08-30 — moved to `M2.P3.T3`.** The diagnostics were added
    for `M2.P3.T1a` and are that milestone's cleanup, not this one's. M7 only
    has to make the local loop work well enough that they stop being needed.

- [ ] M7.P3.T4 — `tools/ci/local/README.md` documenting the loop
  - files: `tools/ci/local/README.md`
  - approach: short and operational — prerequisites (a Docker daemon; sysbox
    on the host if nesting), the three commands in order
    (`fetch-assets.sh` → `build.sh` → `test.sh`), how to get a shell, where
    the build tree and ccache live, how to reset them, and an explicit note
    that the package list in the `Dockerfile` must be kept in sync with
    `.github/workflows/ci.yml`.
  - verify: a reader following the README from a clean clone reaches a
    passing `test.sh ctest` without consulting any other file.
  - size: S

**Verification gate:** from a clean clone, `fetch-assets.sh` → `build.sh` →
`test.sh ctest` completes with no manual steps; a one-file edit rebuilds in
under a minute; and `test.sh smoke --gdb` reproduces the M2.P3.T1a failure with
a usable backtrace.

## Decisions

- 2026-08-30 — pin the local image to `aswf/ci-baseqt:2027.0`: match the tag
  `.github/workflows/ci.yml` actually uses, not the `2027.1` recorded in
  `PLAN/DECISIONS/2026-08-29-pin-exact-aswf-tag.md`. A local environment that
  differs from CI cannot reproduce CI failures. The mismatch between the two
  is real and tracked under the board's `# Open questions`; resolving it is
  deliberately out of scope here, since changing the toolchain mid-debug would
  confound the very failure this milestone exists to reproduce.
- 2026-08-30 — tooling is committed to the repo under `tools/ci/local/`, not
  kept as untracked local scaffolding: it makes the fast loop reproducible for
  anyone cloning the fork, and keeps the local setup steps visibly adjacent to
  the CI ones they must mirror.
- 2026-08-30 — the local tooling targets the **union** of the two live `ci.yml`
  variants, not one branch's copy. This milestone branches off `RB-2.6`, whose
  `ci.yml` installs `wayland-devel`, configures without
  `-DCMAKE_PREFIX_PATH=/usr/local`, and has no Python-bindings smoke step;
  `ci-smoke-test-m2p3t1a`'s copy drops `wayland-devel`, adds the prefix path,
  and adds the smoke step. The container therefore installs the superset of
  packages, `build.sh` passes `-DCMAKE_PREFIX_PATH=/usr/local` (a no-op where
  CI omits it), and `test.sh` implements `smoke` regardless — M7.P3.T2 needs it
  even though `RB-2.6`'s CI does not yet run it. Superset is safe in both
  directions; a per-branch local environment would defeat the purpose.
