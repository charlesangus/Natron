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
- Disk: the base image is ~13.9 GB (measured), and a debug build tree is
  ~4.1 GB.

## 1. Build the image

`Dockerfile` is `FROM aswf/ci-vfxall:2027-clang21.1` -- the exact tag
`.github/workflows/ci.yml` pins -- and adds no layers, because that base
image already ships everything CI's `dnf install` step provides. See the
comments at the top of `Dockerfile` for the full package-by-package
breakdown, why this is `ci-vfxall` rather than `ci-baseqt`, and the one
documented gap (`extra-cmake-modules`, below).

```
docker build -t natron-dev:2027-clang21.1 tools/ci/local/
```

If you built the old `natron-dev:2027.0` image before this switch, the
running container still points at it -- run
`tools/ci/local/devshell.sh --recreate` after the build above to pick up the
new base. The ccache and `HOME` volumes survive that.

### If the pull fails partway through

In network-restricted environments, Docker Hub's blob CDN
(`production.cloudfront.docker.com`) can be unreachable even when the
registry API works, so `docker pull`/`docker build` authenticates and then
fails on the first blob. Workaround, verified to give a byte-identical image:

```
docker pull mirror.gcr.io/aswf/ci-vfxall:2027-clang21.1
docker tag mirror.gcr.io/aswf/ci-vfxall:2027-clang21.1 aswf/ci-vfxall:2027-clang21.1
```

Then re-run the `docker build` above; it will resolve `FROM` locally.

## 2. Fetch test assets (one time)

Prepares `build/assets/`: downloads the OCIO configs, and **builds** four OFX
plugin bundles from source at pinned SHAs -- `IO.ofx`, `Misc.ofx`, `CImg.ofx`
and `Arena.ofx` -- along with the dependency libraries (lcms2, libzip,
ImageMagick) `Arena.ofx` needs. Idempotent -- a second run is instant, it
skips anything already present and matching the pins.

```
tools/ci/local/fetch-assets.sh
```

The plugins are built rather than downloaded because the only bundle
published upstream is an Ubuntu 22 build that cannot load on Rocky 9 (it
needs `GLIBCXX_3.4.30`, and links OCIO 1 / OIIO 2.2 / OpenEXR 2.5). The
`aswf/ci-vfxall` image ships OCIO, OIIO, OpenEXR, OpenFX and LibRaw, so the
bundles are built against the container's own libraries instead. SeExpr is
built too (ASWF ships none) and linked statically, so the resulting `IO.ofx`
is self-contained. `Arena.ofx` gets there a different way: its dependencies
are staged into a `Libraries/` directory at the bundle root, which its
existing `RUNPATH` already resolves to. Either way, nothing here or in
`test.sh` needs an `LD_LIBRARY_PATH`. See the header of `fetch-assets.sh` for
the full reasoning and the pinned SHAs.

First run costs a few minutes of compiling; CI caches the result (see the
"Cache test assets" step in `ci.yml`), keyed on `fetch-assets.sh`'s hash so
that bumping a pin correctly invalidates it.

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

`ninja` parallelism defaults to `nproc`, overridable via `NATRON_BUILD_JOBS`,
e.g. `NATRON_BUILD_JOBS=2 tools/ci/local/build.sh` -- useful when a runner's
core count shouldn't dictate memory pressure.

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

## 5. Package (tarball + AppImage)

```
tools/ci/local/package.sh [debug|release]
```

Stages the built tree (`stage-bundle.sh`) and packages it into a `.tar.xz`
and an AppImage, landing both -- plus their `.sha256` files -- in
`<build-dir>/artifacts/`, e.g. `build/release/artifacts/`. This is the local
equivalent of `release.yml`'s "Stage bundle" / "Create tarball" / "Create
AppImage" steps, which only run on a tag push and drop their output in
`/tmp/artifacts` instead of the build tree. Defaults to `release` (packaging
a debug build is rarely useful, but supported).

`appimagetool` needs a route to `upload.wikimedia.org` to validate the
AppData screenshot URL; sandboxes without one can drop a wrapper at
`build/appimagetool-wrapper/appimagetool` that execs the real tool with
`-n`/`--no-appstream` (see that wrapper's own header for the full story) --
`package.sh` puts it ahead of `PATH` when present, and does nothing when it
isn't (e.g. CI, which has real network access).

## 6. Verify a release bundle starts outside the dev container

`tools/release/stage-bundle.sh` already runs `check-relocatable.sh` and
`check-startup.sh` on every bundle it stages, and `make-appimage.sh`/
`make-tarball.sh` refuse to package a bundle that fails either. All three of
those, though, run inside whatever container staged the bundle -- your
`natron-dev` container locally, `aswf/ci-vfxall` in `release.yml` -- and that
container has the entire ASWF/Conan VFX stack, Qt's xcb dependencies, and a
matching Python installed system-wide. A bundle that is missing a RUNPATH, or
that should have shipped a library itself but didn't, can therefore still
start there and pass both checks, because the container silently stands in
for whatever the bundle failed to ship. That is how missing RUNPATHs, an
absent xcb stack, no Python stdlib and no OFX plugins each reached a built
artifact before anyone thought to check; the worst of them shipped a bundle
whose own older `libstdc++.so.6` shadowed the host's, breaking the host's
Mesa driver in a way that only ever showed up on a real desktop.

```
tools/release/check-startup-container.sh <staging-dir>
```

This runs the same `check-startup.sh`, completely unmodified, inside a
throwaway `debian:12` container (`tools/release/container/Dockerfile`) that
has never had Natron's dependencies installed -- only `Xvfb` and the small
set of libraries `tools/release/excludelist.txt` says the bundle is entitled
to expect from the host (fontconfig, X11/GLX, Mesa's software rasteriser,
glib, D-Bus). It fails there the same way it would on a user's machine, and
passes only when the bundle is genuinely self-contained. Needs Docker on the
host; the image is ~a few hundred MB and builds once, then is reused.

Run it against `/tmp/natron-stage` after `stage-bundle.sh`, or against
`squashfs-root`/the extracted tarball after `make-appimage.sh`/
`make-tarball.sh` -- anywhere `check-startup.sh` itself would accept.

## Getting a shell

```
tools/ci/local/devshell.sh
```

With no arguments this drops you into an interactive shell in the persistent
dev container, creating it on first use. With arguments it runs a command
inside the container and propagates its exit code -- that's how `build.sh`
and `test.sh` reach the container themselves; you don't need to run
`devshell.sh` before them.

## Running the GUI (X11 forwarding)

`devshell.sh` forwards `DISPLAY` and mounts `/tmp/.X11-unix` into the
container, and passes through `/dev/dri` (with the host's `render`/`video`
group GIDs) when present, so `./build/debug/App/Natron` can open a window on
your host's X server. On the host you may need to allow local Docker clients
to connect first: `xhost +local:docker`.

GPU passthrough only covers Mesa (Intel/AMD) via `/dev/dri`; an NVIDIA GPU
needs `nvidia-container-toolkit`'s `--gpus`, which this script doesn't set up.

**Known issue: `undefined symbol: LLVMInitializeAMDGPUTargetInfo`.** This
image's `LD_LIBRARY_PATH` puts `/usr/local/lib` (the Conan-installed
toolchain LLVM, used for compiling) ahead of `/usr/lib64` (the system LLVM
Mesa's `libgallium` was actually built against). The Conan build is missing
AMDGPU codegen, so loading Mesa's GLX driver for real rendering fails with
that symbol error -- confirmed via `nm -D --defined-only
/usr/local/lib/libLLVM.so.21.1 | grep LLVMInitializeAMDGPUTargetInfo`
(nothing) vs. the same against `/usr/lib64/libLLVM.so.21.1` (present).
Reordering `LD_LIBRARY_PATH` wholesale just trades this failure for another
(`libQt6Gui.so.6: undefined symbol: FT_Get_Paint` -- Qt needs the Conan
FreeType, not the system one). The fix is to force only the correct LLVM via
`LD_PRELOAD`, leaving the rest of the search order untouched:

```
tools/ci/local/devshell.sh env LD_PRELOAD=/usr/lib64/libLLVM.so.21.1 ./build/debug/App/Natron
```

This is a genuine image-level library mismatch (`aswf/ci-vfxall:2027-clang21.1`'s
Conan LLVM package vs. its system Mesa package), not something fixable from
`devshell.sh` flags alone -- `LD_PRELOAD` here is a targeted workaround, not
a real fix. It isn't baked into the container's default environment because
`LD_PRELOAD`-ing a ~125 MB library into every process (including the
thousands spawned during a build) would add unnecessary overhead there for
no benefit -- this only matters for GUI/GL launches.

## Where things live, and how to reset them

| What | Where | Reset |
|---|---|---|
| Build tree | `build/debug`, `build/release` (gitignored) | delete the directory |
| Packaged artifacts | `build/<type>/artifacts` (gitignored) | delete the directory |
| Test assets | `build/assets` (gitignored) | delete, then re-run `fetch-assets.sh` |
| ccache, `HOME` | Docker named volumes `natron-dev-ccache`, `natron-dev-home` | `docker volume rm` |
| Container | `natron-dev` | `docker rm -f natron-dev`, or see below |

`CCACHE_MAXSIZE` defaults to `40G`, overridable via env var. The container
name is overridable via `NATRON_DEV_CONTAINER` -- use this to give a second
worktree its own container and caches, e.g.
`NATRON_DEV_CONTAINER=natron-dev-wt2 tools/ci/local/devshell.sh`.

## Running build.sh/test.sh/package.sh from inside a container already (e.g. CI)

`build.sh`, `test.sh` and `package.sh` normally re-exec themselves through
`devshell.sh` so they can be invoked directly from the host. If something
already runs them inside a container -- `devshell.sh` itself, or a CI job
whose container *is* the dev image -- they need to detect that and run
directly instead of trying (and failing, for lack of a Docker daemon) to
nest another container.

- `NATRON_IN_CONTAINER=1` is the explicit signal: `devshell.sh` sets it on
  every container it creates, and setting it yourself forces `build.sh`/
  `test.sh`/`package.sh` to skip the re-exec and run directly -- e.g.
  `tools/ci/local/devshell.sh env NATRON_IN_CONTAINER=1 tools/ci/local/build.sh`.
- Failing that, both scripts also infer "already in a container" from `CI`
  (checked case-insensitively, so `CI=true` as set by GitHub Actions and
  `CI=True` as set by `devshell.sh` both count -- a strict compare against
  one spelling would have silently mis-detected Actions' lowercase value).
  `/.dockerenv` was considered as a further, runtime-level fallback but
  rejected: it is not specific to the `natron-dev` container, and on hosts
  that are themselves already inside some unrelated container (e.g. certain
  sandboxed dev environments) its presence would falsely report "already in
  the dev container" and run cmake/ninja directly on the wrong filesystem.

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
- `package.sh`'s stage/tarball/AppImage sequence must stay in sync with
  `release.yml`'s "Stage bundle" / "Create tarball" / "Create AppImage"
  steps -- it exists to reproduce them locally, and both call the same
  `tools/release/*.sh` scripts, so a step added, reordered or reflagged in
  one place needs the same change in the other.
- `fetch-assets.sh`'s pinned plugin SHAs are the test fixture. Bumping one
  changes what `BaseTest` loads; re-run `fetch-assets.sh` (it rebuilds when
  the stamp no longer matches) and expect CI's asset cache to miss once.

If a local result disagrees with CI and you haven't touched either file
recently, check here before assuming the local environment is broken.

## Known divergences from CI

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
