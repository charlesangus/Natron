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

- [x] M7.P1.T1 — Confirm nested Docker works and pull the CI base image
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

- [x] M7.P1.T2 — `tools/ci/local/Dockerfile` layering the CI setup steps onto the base
  - files: `tools/ci/local/Dockerfile`
  - approach: **Re-planned 2026-08-30 — see the sealed-network decision below.**
    No `dnf` is possible here: every distro repo (Rocky, EPEL, NVIDIA CUDA) is
    unreachable. It turns out almost none is needed — the base image already
    provides CI's entire package list. `cairo-devel`, `wget`, `unzip`,
    `xorg-x11-server-Xvfb`, `wayland-devel`, `ninja-build`, and `gdb` are
    installed as RPMs, and `clang` and `ccache` are present at
    `/usr/local/bin` (Conan-provided, so `rpm -q` does not see them). CI's
    `dnf install` line is therefore very nearly a no-op against this image.
    The one real gap is `extra-cmake-modules`: `CMakeLists.txt:114` calls
    `find_package(ECM NO_MODULE)` to put `FindWayland.cmake` on
    `CMAKE_MODULE_PATH` for the `find_package(Wayland COMPONENTS Client Egl)`
    on line 118. Neither call is `REQUIRED`, so without ECM the build silently
    configures *without* Wayland — a fidelity divergence exactly of the kind
    this milestone exists to avoid. Install ECM from the KDE GitHub mirror
    (github.com is reachable) at an explicitly pinned tag; it is pure CMake
    modules and builds with no dependencies. Comment the whole file with what
    CI installs, what the base image already satisfies, and why ECM is fetched
    from source rather than `dnf`.
  - verify: `docker build -t natron-dev:2027.0 tools/ci/local/` succeeds;
    `docker run --rm natron-dev:2027.0 bash -lc 'ccache --version && ninja
    --version && gdb --version && which Xvfb && which clang'` prints all five;
    and `/usr/local/share/ECM` is absent, matching the documented divergence.
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

- [x] M7.P2.T1 — `devshell.sh`: long-lived container with persistent caches
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
- 2026-08-30 — **Docker Hub's blob CDN is unreachable from this environment;
  pull `aswf/ci-baseqt:2027.0` through `mirror.gcr.io` and retag.** Every
  `docker pull aswf/ci-baseqt:2027.0` fails with `no route to host` against
  `production.cloudfront.docker.com` — all four resolved edges (`65.8.70.12`,
  `.25`, `.86`, `.110`) are egress-filtered here, while `registry-1.docker.io`
  and `auth.docker.io` are reachable, so pulls authenticate and then die on the
  first blob. `mirror.gcr.io`, `ghcr.io`, and `quay.io` all connect.
  `docker pull mirror.gcr.io/aswf/ci-baseqt:2027.0` followed by
  `docker tag mirror.gcr.io/aswf/ci-baseqt:2027.0 aswf/ci-baseqt:2027.0`
  succeeds and leaves the `Dockerfile`'s `FROM` line pinned to the same tag CI
  uses — the mirror is a transport detail, not a different image. Verified
  identical content: digest
  `sha256:9df58e9cc6831773bad261596a317ea6019006423ad73ed91652e1762a5a68f7`.
- 2026-08-30 — M7.P1.T1 findings: the base image provides gcc 14.2.1
  (Red Hat 14.2.1-13), Qt 6.8.3 with its CMake package at
  `/usr/local/lib/cmake/Qt6`, CMake 4.3.3, and **Python 3.13.14**. Image is
  12.5 GB on disk, leaving 116 GB free — enough for two build trees.
- 2026-08-30 — **`ci.yml` sets `PYTHON_VERSION: '3.10'` but the image ships
  Python 3.13.14.** The workflow is also still named "Test Ubuntu Python 3.10"
  while running on Rocky 9. The env var appears to be vestigial from the
  pre-ASWF CI, but the gap is worth flagging to M2: `M2.P3.T1a` is debugging a
  PySide6/Shiboken6 binding failure, and a stale assumption about which Python
  the bindings are built against is exactly the kind of thing that would cause
  it. Not acted on here — M7 changes no CI behaviour — but it is the first
  thing M2 should check once the local loop can reproduce the failure.
- 2026-08-30 — **this environment has no reachable distro package repository at
  all; the local image must be built from what the base image already
  contains.** Beyond Docker Hub's CDN, egress filtering also blocks
  `mirrors.rockylinux.org`, `mirrors.fedoraproject.org`,
  `dl.fedoraproject.org`, `developer.download.nvidia.com`, and every EPEL
  mirror probed (`mirrors.kernel.org`, `mirror.math.princeton.edu`,
  `ftp.osuosl.org`, `mirror.us.leaseweb.net`, `epel.mirror.constant.com`).
  Reachable: `github.com`, `registry-1.docker.io`, `auth.docker.io`,
  `mirror.gcr.io`, `ghcr.io`, `quay.io` — an allowlist, not a blocklist. So
  `dnf install` cannot run in a `RUN` layer, and the original M7.P1.T2 plan
  (mirror CI's `dnf install` line) is not executable as written.
  This costs less fidelity than it first appears: an inventory of
  `aswf/ci-baseqt:2027.0` shows it already ships every package CI installs —
  `cairo-devel`, `wget`, `unzip`, `xorg-x11-server-Xvfb`, `wayland-devel`,
  `ninja-build` and `gdb` as RPMs, plus `clang` and `ccache` under
  `/usr/local/bin` from Conan. CI's "Install Linux system packages" step is
  therefore close to a no-op on this image, and the local-only additions the
  original plan wanted (`ccache`, `ninja-build`, `gdb`) are already there.
  Only `extra-cmake-modules` is genuinely absent, and it is fetched from
  GitHub instead. The package-list-must-match-`ci.yml` comment stays in the
  Dockerfile as documentation of what CI asks for and why each item is already
  satisfied, so the two still cannot silently diverge.
- 2026-08-30 — **`extra-cmake-modules` is deliberately NOT installed; the aswf
  image is used as the build engine as-is.** Project owner's call, taken after
  the sealed-network finding above: ECM was the single package in CI's list the
  base image lacks, and a working from-source build of it (KDE GitHub,
  `v5.116.0`, verified to give `Wayland_FOUND=TRUE`) was implemented and then
  cut rather than carried. The Dockerfile consequently adds no layers — it is
  `FROM aswf/ci-baseqt:2027.0` plus the comment block that documents what CI
  installs and how the base image satisfies each item, which is what keeps a
  `ci.yml` diff meaningful.
  **Known divergence, recorded deliberately:** `CMakeLists.txt:114` calls
  `find_package(ECM NO_MODULE)` to put `FindWayland.cmake` on
  `CMAKE_MODULE_PATH` for line 118's `find_package(Wayland COMPONENTS Client
  Egl)`. Neither is `REQUIRED`, so locally the build configures with **Wayland
  support off** while CI has it **on**. This does not break the build and does
  not affect the `NatronRenderer` smoke test M7 exists to debug, but it is the
  one place local and CI differ, and it must be ruled out first if a local
  result ever fails to match CI. Closing the gap later is a one-line change
  once a package repo is reachable.
