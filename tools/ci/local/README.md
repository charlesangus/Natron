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

To build on GitHub Actions instead, and to choose local or remote per host
(`.local/build-mode`), see [`../remote/README.md`](../remote/README.md).

## Prerequisites

- A working Docker daemon. If you are inside a container yourself, the host
  needs something like sysbox so Docker can nest.
- Disk: the base image is ~13.9 GB (measured), and a debug build tree is
  ~4.1 GB.

## Git hooks

Install the project's pre-commit hook to auto-format staged C/C++ changes locally:

```shell
tools/install-git-hooks.sh
pip install --user --break-system-packages clang-format==21.1.8
```

This prevents formatting issues from reaching CI, using the same `clang-format==21.1.8` the `format` job uses.

## 1. Build the image

`Dockerfile` is `FROM aswf/ci-vfxall:2027-clang21.1`, and that base image
already ships everything CI's `dnf install` step provides. It adds three
layers: the Mesa relink (see "Mesa is relinked against the system LLVM"
below), the Slang toolchain and the glslang source tree (see "Slang and
glslang layers" below). See the comments at the top of `Dockerfile` for the
full package-by-package breakdown, why this is `ci-vfxall` rather than
`ci-baseqt`, and the one documented gap (`extra-cmake-modules`, below).

You rarely need to build it yourself: `devshell.sh` pulls the published
image (next section) and only builds locally when the pull fails.

### Published image

CI publishes the image to GitHub Container Registry as
`ghcr.io/charlesangus/natron-dev:<content hash>`. The package is public, so
pulling needs no login. The tag is the first 16 hex characters of a SHA-256
over `tools/ci/local/Dockerfile`, `cmake/FetchSlang.cmake`,
`cmake/FetchGlslang.cmake` and `tools/ci/local/install-git-lfs.sh`, the files
that decide the image's contents, so
an unchanged image is published once and every checkout with the same files
resolves to the same tag. `tools/ci/local/dev-image-tag.sh` is the single
definition of that tag; `devshell.sh` and the workflows all call it:

```
tools/ci/local/dev-image-tag.sh
```

`.github/workflows/dev-image.yml` ensures the content-hash tag exists on
every run, building and pushing the image when it is missing. Only a run whose
caller ref is `main` (and that is not a pull request) moves `:latest`, and it
does so whether or not that run built the hash tag; any other run leaves
`:latest` alone. The workflow is reusable through `workflow_call`, which
`ci.yml` and `remote-build.yml` do first, so CI and remote builds run inside the published image
(`container:` jobs), and it can be dispatched by hand with
`gh workflow run dev-image.yml --ref <branch>`.

`devshell.sh` resolves the image through `dev-image-tag.sh`. When the
container does not exist yet and the image is not present locally, it runs
`docker pull`; if the pull fails (offline, or a Dockerfile change that is
not yet published) it falls back to `docker build` of
`tools/ci/local/Dockerfile` under the same tag. It then also tags the image
`natron-dev:2027-clang21.1`, the name older commands use. The image is only
resolved when the container is created, and a running container is never
touched. To force the fallback by hand:

```
docker build -f tools/ci/local/Dockerfile -t "$(tools/ci/local/dev-image-tag.sh)" tools/ci/local/
```

After a Dockerfile change (or a pull of a newer tag), run
`tools/ci/local/devshell.sh --recreate` to pick the new image up, and only
when no build is running in that container (see "Checking syntax before a
build" for why). The `.ccache` dir and `HOME` volume survive that.

### Slang and glslang layers

The GPU targets need `slangc` and the glslang sources at configure time. The
`Dockerfile` bakes both in, at the releases and SHA-256 sums that
`cmake/FetchSlang.cmake` and `cmake/FetchGlslang.cmake` pin:

- `SLANG_ROOT=/opt/slang` holds the Slang release; only `bin/slangc` and
  `include/slang-cpp-prelude.h` are used.
- `GLSLANG_ROOT=/opt/glslang` holds the glslang source tree, which the
  Natron build compiles itself.

Both environment variables are set in the image, and the Fetch files use a
tree that `SLANG_ROOT` / `GLSLANG_ROOT` points at instead of downloading into
`.natron-deps/`. So a container with no network access can configure the GPU
targets, and neither the local image nor the CI jobs that run in it download
anything. Outside the image (a bare host, say) the Fetch files still download
the pinned releases into `.natron-deps/`, which is gitignored.

The pins therefore live in two places, each Fetch file and the `Dockerfile`'s
`ARG NATRON_SLANG_VERSION` / `NATRON_SLANG_SHA256` /
`NATRON_GLSLANG_VERSION` / `NATRON_GLSLANG_SHA256`. The drift check compares
them and exits non-zero, naming the variable, when they differ. It reads
files only and needs no network or Docker:

```
tools/ci/local/check-image-pins.sh
```

It runs as the "Slang and glslang pins match local image" step of
`.github/workflows/checks.yml`, so a PR
that bumps one side without the other fails. Bump both together; the
content-hash tag then changes and the next CI run publishes a new image.

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

It also builds OpenFX metadata example plug-ins. `metadataCompare`,
`metadataContribute` and `metadataTimeCode` have no native equivalent and
ship in `build/assets/Plugins/`. `metadataPrint` is a test fixture only: it
goes to `build/assets/TestPlugins/`, which `test.sh ctest` appends to
`OFX_PLUGIN_PATH`, while `test.sh smoke` and the release bundle see
`build/assets/Plugins/` alone. Running a test binary by hand with
`OFX_PLUGIN_PATH=build/assets/Plugins` leaves the Print-based metadata cases
without their fixture; append `:build/assets/TestPlugins`.

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
tools/ci/local/build.sh [debug|release|fast] [--tests] [--reconfigure] [--no-ccache-stats]
```

`build.sh` prints the resolved preset, `CMAKE_BUILD_TYPE`, compile flags and
whether debug info is on at the start of every run, so you can confirm what
you are about to build.

**`debug`** (default) -- `build/debug`, `-DCMAKE_BUILD_TYPE=Debug`, compiled
with `-O0 -g -DDEBUG`. Assertions are live: `NDEBUG` is not defined, and
`CMakeLists.txt` strips the `NDEBUG` that the installed Shiboken6 package
exports as an interface definition, which would otherwise silence `assert()`
in every target linking it. Unoptimised, so it runs slowest. Reach for it when
iterating on a single file or chasing a bug where you want assertions and a
debugger-friendly binary.

**`release`** -- `build/release`, no explicit `-DCMAKE_BUILD_TYPE`, so
`CMakeLists.txt` defaults it to `RelWithDebInfo`: `-O2 -g -DNDEBUG`. This
matches CI's release job exactly and is the slowest to build. Use it for
anything you'd ship, profile, or compare against CI.

**`fast`** -- `build/fast`, `-DCMAKE_BUILD_TYPE=Release` with
`CMAKE_C_FLAGS_RELEASE` and `CMAKE_CXX_FLAGS_RELEASE` overridden to
`-O1 -DNDEBUG`: no `-g`, no `-DDEBUG`. Release semantics, so it packages and
runs under llvmpipe like `release` does, at roughly half `release`'s compile
time. Use it for UI/UAT checkpoints where render performance doesn't matter.
It is not a CI configuration.

`fast` builds without the gtest suite (`NATRON_BUILD_TESTS=OFF`) unless
`--tests` is given, so a UI edit does not relink the large Tests binary.
`--tests` is for `fast` only; `debug` and `release` always build tests. The
requested value is passed on every configure, and a cached value that differs
forces a reconfigure.

`--no-ccache-stats` skips the `ccache -s` summary printed at the end.

Configure only runs once, when `CMakeCache.txt` doesn't exist yet; pass
`--reconfigure` after editing `CMakeLists.txt` or similar to force it again.
This is also what re-execs you into the dev container (see "Getting a
shell" below) -- you don't need to start one yourself first.

`ninja` parallelism defaults to `-j3`, overridable via `NATRON_BUILD_JOBS`,
e.g. `NATRON_BUILD_JOBS=2 tools/ci/local/build.sh` -- useful when a runner's
core count shouldn't dictate memory pressure.

Edits to `Global/Enums.h` or `Global/GlobalDefines.h` recompile essentially
every translation unit (~380 objects); batch such changes rather than
touching them mid-iteration. When a rebuild looks bigger than it should,
`ninja -d explain -n` in the build directory names the input that dirtied
each target.

### Checking syntax before a build (`syntax-check.py`)

```
tools/ci/local/syntax-check.py [--root <checkout>] [--base <ref>] [--compdb <path>] [<file> ...]
```

Runs `-fsyntax-only` on translation units inside the dev container (three at
a time) with the flags from the main checkout's `build/fast` compile DB, so
it needs one `build.sh fast` first. Use it before a remote build, where one
failing TU costs a full build cycle. Files are relative to `--root` (default:
the current checkout, e.g. a worktree). With no files it checks what changed
since `--base` (default: the merge base with `origin/main`), including
uncommitted and untracked files. A header is checked through its `.cpp`.
Results are `OK`, `FAIL` (with the error lines), `NO-COMMAND` or
`SKIPPED (header, no owner)`. The exit status is non-zero on any `FAIL` or
`NO-COMMAND`.

A TU missing from the DB, such as a new test, borrows the command of a DB
entry in the same CMake directory (its own directory or a parent that
shares the nearest `CMakeLists.txt`), never across a top-level directory or
into a nested CMake project. A new `Tests/*.cpp` gets the `Tests` target's
flags, including `-I Tests` and `-I libs/xxhash`.

The compile DB can be stale for a TU: its CMake directory may be new to the
DB, or its `CMakeLists.txt` may differ from the checkout's, so the target
that owns the TU is not in the DB. Borrowing a sibling's flags would then be
wrong, so the script instead configures `build/syntax` in the checked
checkout (same `fast` flags, tests on) and checks the TU against that
database. This reconfigure is also refused while `ninja` runs in the
container.

TUs under `Engine/Gpu/`, or that include a generated kernel header
(`GpuKernels.inc`, `*_spirv.h`), need a DB that knows the current kernels. If
the DB predates the last change to `Engine/Gpu/Kernels/` or
`Engine/Gpu/CMakeLists.txt`, the script configures `build/syntax` in the
checked checkout with the `fast` flags and builds only the kernel-header
targets (about 30 s cold, a few seconds after that). It refuses while `ninja`
runs in the container, and fails, naming the header, if a generated header is
still missing. A GPU-era tree is therefore checked without a prior
`build.sh` of that tree and without hand-written stub kernel headers. Inside
the dev image the Slang and glslang trees come from `SLANG_ROOT` and
`GLSLANG_ROOT`; elsewhere a worktree reuses the main checkout's
`.natron-deps` instead of downloading a second copy. `NATRON_DEV_CONTAINER`
picks the container.

The dev container is single-tenant: run one build per container at a time. A
`build.sh` or `ninja` running there while another build, a second
`syntax-check.py` reconfigure or a `--recreate` starts gives slow, flaky
results that look exactly like compile errors. Give a second lane its own
container with `NATRON_DEV_CONTAINER` (see "Where things live").

## 4. Test

```
[NATRON_RENDER_SCHEDULER=legacy|taskgraph] [NATRON_CTEST_JOBS=<n>] \
  [GTEST_SHUFFLE=1 GTEST_RANDOM_SEED=<n> GTEST_REPEAT=<n>] \
  tools/ci/local/test.sh <ctest|smoke> [debug|release|fast] [--gdb]
```

- `ctest` runs against the build dir. By default (`NATRON_CTEST_JOBS` unset)
  that is the serial `ctest -V`; PR CI sets `NATRON_CTEST_JOBS=4`, which runs
  `ctest -j 4 --output-on-failure`. There is one ctest entry per gtest suite
  (named `<Suite>`), not per case; to run one case, call the test binary with
  `--gtest_filter=Suite.Case`.
- `smoke` locates the built `NatronRenderer` and runs
  `tools/ci/smoke_test.py` through it. This script only exists on branches
  that carry the M2-era `tools/ci/smoke_test.py` addition -- it is not on
  `RB-2.6`, and `test.sh` will tell you plainly and exit if it's missing on
  your checkout.
- `NATRON_RENDER_SCHEDULER` selects the render scheduler for `ctest` and
  `smoke`; unset leaves Natron's default. It is forwarded into the container
  when set.
- `NATRON_CTEST_JOBS` (`ctest` only) is how many cases run at once, all as
  clients of the one Xvfb server. Unset or 1 is the serial `ctest -V` run.
  Above 1 only failing cases' output is printed as each finishes; the full
  output of every case is in `Testing/Temporary/LastTest.log`. Cases that
  assert a wall-clock bound run alone (`Tests/SuiteProperties.cmake`). It is
  forwarded into the container when set. When the parallel run fails,
  `test.sh` then re-runs just the failed cases serially with
  `ctest --rerun-failed -V`, so their complete output (including assert
  text that parallel output hides) is in the log. The re-run is diagnostic
  only; the script's exit status stays that of the parallel run.
- `GTEST_SHUFFLE`, `GTEST_RANDOM_SEED` and `GTEST_REPEAT` (`ctest` only) are
  read by gtest itself; ctest passes its environment on to every suite
  process. `GTEST_SHUFFLE=1 GTEST_RANDOM_SEED=<n>` runs the cases within each
  suite in a fixed shuffled order, to flush out leaks between cases;
  `GTEST_REPEAT=<n>` runs each suite n times. Each ctest entry is one suite,
  so the shuffle never crosses suites. All three are forwarded into the
  container when set.
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
tools/ci/local/package.sh [debug|release|fast]
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

`make-appimagetool-wrapper.sh` creates that wrapper:

```
tools/ci/local/make-appimagetool-wrapper.sh <dir> [<real-appimagetool>]
```

`<dir>` receives `appimagetool` (the wrapper) and `appimagetool.real` (a
copy of the real tool). The real tool is the path given, else the first
`appimagetool` on `PATH`, else the `appimagetool.real` already in the main
checkout's `build/appimagetool-wrapper/`; it exits 1 when none is found.
`package.sh` calls it itself when the wrapper is missing but one of those
sources exists, so you normally never run it by hand.

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

**Mesa is relinked against the system LLVM.** The base image's
`ld.so.conf` and `LD_LIBRARY_PATH` both put `/usr/local/lib` (the Conan
clang toolchain's `libLLVM.so.21.1`, built without the AMDGPU target) ahead
of `/usr/lib64` (the system `libLLVM.so.21.1` Mesa was built against), so
unmodified, RADV and radeonsi fail with `undefined symbol:
LLVMInitializeAMDGPUTargetInfo` and only llvmpipe/lavapipe work. Neither
reordering the search path (Qt then picks up the wrong FreeType) nor a
global `LD_PRELOAD` (clang itself links the `/usr/local` copy, so it would
swap the compiler's LLVM) fixes this cleanly. Instead, the `Dockerfile`
rewrites the `NEEDED libLLVM.so.21.1` entry of every Mesa library that has
one (`libgallium`, `libvulkan_radeon`, `libvulkan_lvp`, `libOSMesa`) to the
absolute path `/usr/lib64/libLLVM.so.21.1` with `patchelf`. The dynamic
linker opens a slash-containing `NEEDED` by that exact path, so Mesa always
gets the system LLVM and nothing else's search order changes. CI and remote
build jobs run in the same published image, so they carry the relink too;
llvmpipe (under `xvfb-run`) runs on the system LLVM 21.1.8 rather than the
toolchain one.

A `natron-dev` image built from an older checkout lacks the relink; get the
current image (step 1) and `--recreate` the container. `readelf -d
/usr/lib64/libvulkan_radeon.so | grep LLVM` inside it shows the absolute
path when the relink is present.

## GPU container (Vulkan on the host GPU)

```
tools/ci/local/devshell.sh --gpu                      # shell in natron-dev-gpu
tools/ci/local/devshell.sh --gpu <cmd> [args...]
NATRON_DEV_GPU=1 tools/ci/local/build.sh release     # build there
NATRON_DEV_GPU=1 tools/ci/local/test.sh ctest release
```

`--gpu` (or `NATRON_DEV_GPU=1`, which is what `build.sh`/`test.sh` pick up,
since they re-exec through `devshell.sh` without flags) uses a separate
container, `natron-dev-gpu` (still overridable via `NATRON_DEV_CONTAINER`),
from the same image, sharing the repo bind mount and `.ccache`, with its
own `natron-dev-gpu-home` volume. On top of the normal container it:

- refuses to start unless some `/dev/dri/renderD*` is readable and writable
  by the invoking user, rather than silently falling back to lavapipe;
- on a host with `/dev/nvidiactl` whose Docker daemon has the `nvidia` runtime
  (nvidia-container-toolkit), creates the container with `--runtime=nvidia`
  and `NVIDIA_VISIBLE_DEVICES=all`,
  `NVIDIA_DRIVER_CAPABILITIES=compute,graphics,utility,display`, so the host
  driver, its Vulkan ICD and its EGL vendor file are injected alongside the
  `/dev/dri` passthrough. Inside, `nvidia-smi -L` lists the NVIDIA GPUs and the
  Vulkan device list shows them ahead of llvmpipe. No `VK_*` or
  `__EGL_VENDOR_LIBRARY_DIRS` override is needed.

A container keeps the runtime it was created with, so an existing `--gpu`
container must be recreated to pick up the NVIDIA runtime: pass `--recreate`
(caches are kept) or use a fresh name, e.g.
`NATRON_DEV_CONTAINER=natron-dev-m91-gpu`.

On an AMD RX 7900 XTX host, the Vulkan device list inside it is:

```
[0] AMD Radeon RX 7900 XTX (RADV NAVI31)   driver radv, Mesa 25.2.7
    queue family 0: graphics|compute|transfer|sparse
    queue family 1: compute|transfer|sparse (x4)
    queue family 2: video-decode
    queue family 3: transfer|sparse [transfer-only] (x2)
    queue family 4: sparse
[1] llvmpipe (LLVM 21.1.8, 256 bits)
```

To force lavapipe in that container, select its ICD alone:
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`.
The image has no `vulkaninfo`; list devices with a small Vulkan program.

**Host requirement: render node permissions after a reboot.** The render
node must be openable by the uid the container runs as. With a
user-namespaced Docker daemon, the host's `render`/`video` groups do not map
into the namespace, so the node appears as `nobody:nogroup` (uid 65534) and
the `--group-add` GIDs do not help; only the "other" permission bits count.
Make it world-accessible with a udev rule on the host, e.g.
`/etc/udev/rules.d/70-dri-render.rules`:

```
KERNEL=="renderD*", SUBSYSTEM=="drm", MODE="0666"
```

then `sudo udevadm control --reload && sudo udevadm trigger -s drm` (or
reboot). Without it, a manual `chmod 666 /dev/dri/renderD128` works until
the next reboot, after which `--gpu` refuses to start again. Note this
grants every local user access to the GPU's render node.

## Memory cap

Every container `devshell.sh` makes (normal, `--gpu`, lane) is capped at 75%
of the Docker daemon's memory, keeping at least 4 GiB back for the host. Swap
is capped at the same value, so a build, test or render that outgrows the cap
is OOM-killed inside the container (exit 137, `docker inspect -f
'{{.State.OOMKilled}}'` says `true`) instead of swapping the host until it
stops responding. `tools/ci/local/memcap.sh` holds the rule:

```
tools/ci/local/memcap.sh --mib --docker     # the cap on this host, in MiB
NATRON_DEV_MEMORY=12g tools/ci/local/devshell.sh   # a different cap
NATRON_DEV_MEMORY=0 tools/ci/local/devshell.sh --recreate   # no cap
```

`NATRON_DEV_MEMORY` takes a whole number with an optional `b`/`k`/`m`/`g`
suffix and nothing else; a literal `0` turns the cap off, and a nonzero value
that rounds to 0 MiB (such as `512k`) is rejected. On a host with under 8 GiB
the default is half its memory rather than 75%, since the 4 GiB reserve would
leave next to none.

The cap follows the host, so no host needs its own setting. An existing
container picks up a new or changed cap the next time `devshell.sh` runs,
without `--recreate`. Removing the cap does need `--recreate`.

Work that does not go through `devshell.sh` gets the same cap this way:

- a hand-rolled container: add `$(tools/ci/local/memcap.sh --docker-args)`
  to the `docker run` line (`tools/gpu-spike/run-bench.sh` does this);
- a host-native run, such as an AppImage render or `GpuTests` on the host:
  prefix it with `tools/ci/local/memcap.sh`, which runs it in a systemd
  user scope with `MemoryMax` and `MemorySwapMax=0`. Where there is no
  systemd user session (macOS, inside a container) it runs the command
  uncapped and prints a warning.

## Worktree lanes

A git worktree under `.worktrees/<id>/` has none of the untracked state the
main checkout has built up. `worktree-init.sh` bootstraps it from the host:

```
tools/ci/local/worktree-init.sh [--skip-assets] [<worktree-path>]
```

`<worktree-path>` defaults to the current checkout. Each step does nothing
when its result is already there, so it is safe to re-run:

1. `git submodule sync` and `git submodule update --init --recursive`
   (nested submodules included), only when one is missing or out of date.
2. `fetch-assets.sh` into the worktree's own `build/assets/`, skipped when
   the plugin stamp matches the pins and the bundles exist. `--skip-assets`
   leaves this step out, because `fetch-assets.sh` compiles plugins in the
   shared dev container and a caller may not want to start a build there.
3. `.natron-deps` linked to the main checkout's copy (not needed in the main
   checkout itself).
4. `build/appimagetool-wrapper/` copied from the main checkout, or
   regenerated with `make-appimagetool-wrapper.sh`.

It prints a summary of what it did, what was already present, and notes. It
never builds Natron.

`build.sh` runs `git submodule update --init --recursive` on the host on every
invocation, which resets each submodule to the commit the superproject
records and so discards a pinned `libs/OpenFX` fork branch. After checking
that branch out, set `NATRON_SKIP_SUBMODULE_UPDATE=1`:

```
NATRON_SKIP_SUBMODULE_UPDATE=1 tools/ci/local/build.sh fast
```

`build.sh` then skips the update and only checks that `Tests/google-mock`,
`Tests/google-test`, `libs/OpenFX` and `libs/SequenceParsing` are checked
out, failing and naming the first one that is not.

## RADV GL display for GUI checks

`xvfb-run` only provides llvmpipe for GL. Its device and driver UUIDs never
match the RADV Vulkan device, so the GL/Vulkan hand-off can only reach the
Readback rung there; the zero-copy rungs need a display whose GL is also
served by RADV. `GlInteropQt` (in `tools/gpu-spike`) prints which rung a given
display reaches: `best`, the `missing` extensions, and a checksum line
(`RESULT best=... checksum=... PASS`). Candidate recipes, in the order to try
them:

1. An existing desktop session on the AMD GPU (`DISPLAY` or
   `WAYLAND_DISPLAY` already set, `QT_QPA_PLATFORM` unset or `xcb`).
2. A headless compositor with a GL renderer:
   `weston --backend=headless --renderer=gl --socket=natron-gl &`, then
   `QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=natron-gl <binary>`.
3. Xorg with a dummy or modesetting config bound to the AMD card, then
   `DISPLAY=:N <binary>`.

On hosts whose `LD_LIBRARY_PATH` carries another application's Qt, run
AppImages and binaries as `env -u LD_LIBRARY_PATH <binary>`, otherwise that Qt
shadows Natron's.

To check a real desktop session in one step, run
`tools/gpu-spike/desktop-handoff-check.sh --glinterop <GlInteropQt or build dir>`
on the host from a terminal in that session. It runs the probe under xcb and/or
wayland, records the GL renderer and Vulkan device, and writes
`handoff-report.txt` (first line `VERDICT ...`) plus raw logs into `--out`
(default: a new temp directory). It warns when the GL renderer is not radeonsi.

Confirmed recipe: not yet recorded.

## Where things live, and how to reset them

| What | Where | Reset |
|---|---|---|
| Build tree | `build/debug`, `build/release`, `build/fast` (gitignored) | delete the directory |
| Packaged artifacts | `build/<type>/artifacts` (gitignored) | delete the directory |
| Test assets | `build/assets` (gitignored) | delete, then re-run `fetch-assets.sh` |
| ccache | `.ccache` (repo root, gitignored, host bind mount) | delete the directory |
| `HOME` | Docker named volume `natron-dev-home` | `docker volume rm` |
| Container | `natron-dev` | `docker rm -f natron-dev`, or see below |
| GPU container | `natron-dev-gpu`, home volume `natron-dev-gpu-home` | `docker rm -f natron-dev-gpu` |

`CCACHE_MAXSIZE` defaults to `80G`, overridable via env var (`devshell.sh`
passes it on every exec, so a changed value needs no `--recreate`). The container
name is overridable via `NATRON_DEV_CONTAINER` -- use this to give a second
worktree its own container and caches, e.g.
`NATRON_DEV_CONTAINER=natron-dev-wt2 tools/ci/local/devshell.sh`.

### Sharing the cache with worktrees

`build.sh` sets `CCACHE_BASEDIR` to the main checkout's root and
`CCACHE_NOHASHDIR=1`, so a worktree build is served from entries the main
tree built (and vice versa) when the worktree sits under the main checkout
and builds in the same `build/<type>` layout relative to its own root, as
`.worktrees/<id>/build/<type>` does. Debug info from a cache hit
can name the other tree's source paths. Under `base_dir`, `__FILE__`
expands to a path relative to the build dir, so code must not locate files
from `__FILE__`.

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
  `tools/ci/local/devshell.sh --recreate` to pick it up. Caches (`.ccache`,
  `HOME`) survive a recreate; for a full teardown, also
  `rm -rf .ccache && docker volume rm natron-dev-home`.

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
