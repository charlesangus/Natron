# Milestone 23: Make release bundles actually relocatable

`tools/release/stage-bundle.sh` produces a bundle that only starts on a machine
that already has the ASWF VFX libraries and the Qt xcb dependencies installed
system-wide. Two independent defects, both found by building an AppImage from
M17's branch and trying to run it on an ordinary desktop:

1. RUNPATH is set on the three `bin/` executables but on none of the libraries
   staged into `lib/` — 62 of 82 carry no RPATH or RUNPATH. `DT_RUNPATH` is not
   transitive, so a bundled library cannot find another bundled library beside it:
   `libOpenColorIO.so.2.5` fails to resolve `libImath-3_2.so.30` despite both being
   in the same directory.
2. The dependency walk covers only the `bin/` executables. Qt plugins are copied in
   but their own dependencies never are, so `plugins/platforms/libqxcb.so` ships
   with no xcb libraries at all — no `libxcb-cursor` (required since Qt 6.5), no
   `libxcb-icccm`, `libxcb-image`, `libxcb-keysyms`, `libxkbcommon-x11`. The app
   aborts at startup with "Could not load the Qt platform plugin xcb".

M15's gate passed on this because packaging was only ever exercised inside the dev
container, which has all of it installed system-wide. **The container structurally
cannot detect this class of bug**, so a fix that is only verified there is not a fix.

See `DECISIONS/2026-09-07-staged-bundle-runpath-bug.md`.

## Phase 23.1: Stage a complete, self-contained tree

- [x] M23.P1.T1 — Walk plugin dependencies, not just executables
  - files: `tools/release/stage-bundle.sh`
  - approach: the ldd-closure walk must seed from every binary the bundle ships that can be `dlopen`'d — the Qt plugin tree under `plugins/` as well as `bin/`. Iterate to a fixed point so a dependency pulled in by a plugin has its own dependencies staged too. Keep honouring `excludelist.txt` for libraries that must come from the host (glibc, GL). Qt 6.5+ requires `xcb-cursor`; do not special-case it, fix the walk so it falls out.
  - verify: the staged tree contains the xcb libraries `libqxcb.so` needs; no staged binary reports a missing direct dependency when resolved against the bundle alone.
  - size: M

- [x] M23.P1.T2 — Set RUNPATH on every staged library
  - files: `tools/release/stage-bundle.sh`
  - approach: the existing step patches only `bin/*`. Every library staged into `lib/` needs its own RUNPATH (`$ORIGIN`, and `$ORIGIN/../lib` where nesting requires it), and so does every staged plugin, since `DT_RUNPATH` does not chain. Set it as part of staging each file rather than as a separate pass over a hardcoded list, so nothing added later is missed.
  - verify: no staged `.so` is left without a RUNPATH; the resolved dependency of a bundled library is the bundled copy, not a system one.
  - size: S

## Phase 23.2: Verify somewhere the libraries are absent

- [x] M23.P2.T1 — A relocatability check that cannot pass falsely
  - files: `tools/release/` (new check script), `tools/ci/local/README.md`
  - approach: a check that resolves every staged binary's dependencies **against the bundle alone**, ignoring system paths and `ld.so.cache`, and fails on anything unresolved that is not on the deliberate host-provided excludelist. This is what makes the gate meaningful: it must give the same verdict inside the dev container as on a bare desktop. A container without the VFX libraries installed is the stronger check if it can be arranged cheaply; the dependency-resolution check is the minimum.
  - verify: run against a bundle staged before M23.P1's fixes — it must fail, naming the unresolved libraries; run against one staged after — it must pass.
  - size: M

- [x] M23.P2.T2 — Wire the check into the release path
  - files: `tools/release/make-appimage.sh` or the packaging entry point, `.github/workflows/` as appropriate
  - approach: run the check as part of producing a release artifact, so a bundle that is not self-contained cannot be published. Match how the existing release workflow is structured rather than adding a parallel one.
  - verify: the packaging path fails loudly on a deliberately broken bundle.
  - size: S

## Phase 23.3: Runtime data, not just libraries

Added 2026-09-07, after the ELF fixes let the bundle get far enough to fail on the
next layer. `check-relocatable.sh` validates `DT_NEEDED` closure and by construction
cannot see data dependencies. The bundle ships `bin lib plugins share` and no
`Resources/` at all, so with libraries finally resolving it dies on:

```
Fontconfig configuration file .../bin/../Resources/etc/fonts does not exist
Could not find platform independent libraries <prefix>
Fatal Python error: Failed to import encodings module
```

`libpython3.13.so.1.0` is staged because it is an ELF dependency; the Python
standard library it needs is not, and nothing sets a Python home. Natron expects
`<bin>/../Resources/etc/fonts` (`Engine/AppManager.cpp:342`) and resolves its Python
home per `Global/PythonUtils.h`.

- [x] M23.P3.T1 — Stage the Python standard library and set its home
  - files: `tools/release/stage-bundle.sh`
  - approach: stage the interpreter's stdlib into the bundle and make the shipped binaries resolve it from there rather than from the host. Read `Global/PythonUtils.h` for the home/path resolution the app already implements and satisfy that contract rather than inventing a parallel one. Do not bundle the host's `site-packages` wholesale; stage what the interpreter needs to start and what Natron's own Python layer imports.
  - verify: the bundle starts with Python initialised on a machine with no matching Python installed.
  - size: M

- [x] M23.P3.T2 — Stage the fontconfig configuration and any other expected Resources
  - files: `tools/release/stage-bundle.sh`
  - approach: create `Resources/etc/fonts` with a working configuration, and audit what else the app resolves relative to its own binary — grep for `applicationDirPath()` in `Engine/` and `Gui/` and satisfy every path it expects, rather than fixing only the one that happened to warn. OCIO configs are a likely second case.
  - verify: no "does not exist" warnings about bundle-relative resources at startup.
  - size: M

- [x] M23.P3.T3 — Start the app offscreen as part of the packaging gate
  - files: `tools/release/` (extend the check or add a smoke script), packaging entry points
  - approach: `--version` returns before Python or any platform plugin initialises, which is why every failure so far reached the user instead of the gate. Run the packaged binary with `QT_QPA_PLATFORM=offscreen` far enough to initialise Python, fontconfig and the plugin layer, and fail packaging on any error output. This is the check that would have caught all three defects in this milestone; the ELF check complements it but cannot replace it.
  - verify: the smoke test fails on a bundle missing the Python stdlib and passes on a complete one.
  - size: M

## Phase 23.4: OFX plugin bundles

The staged bundle ships **no OFX plugins at all** — no Read, Write, Merge or Blur.
`Engine/OfxHost.cpp:890` looks for them under `<bin>/../Plugins/OFX/Natron`, and
`cmake --install` stages nothing there. Staging `build/assets/Plugins/*.ofx.bundle`
was attempted and `check-relocatable.sh` correctly rejected the result.

Measured on the built plugins rather than assumed: each `.ofx` already carries
`RUNPATH=$ORIGIN/../../Libraries`, which from `Contents/Linux-x86-64/` correctly
resolves to its own bundle's `Libraries/`. Two things are wrong anyway. Only
`Arena.ofx.bundle` actually *has* a `Libraries/` directory — `CImg`, `IO` and
`Misc` point at one that does not exist. And none of them can reach the Natron
bundle's own `lib/`, where the OCIO, OIIO and OpenEXR they link against live. So
the fix is not "rewrite a build-tree path"; it is to give each plugin a RUNPATH
reaching both its own `Libraries/` and the bundle's `lib/`, without flattening the
per-bundle layout the OFX spec requires.

- [x] M23.P4.T1 — Stage OFX plugin bundles with a RUNPATH that reaches both trees
  - files: `tools/release/stage-bundle.sh`
  - approach: stage `*.ofx.bundle` under `Plugins/OFX/Natron/`, preserving each bundle's `Contents/<arch>/` and `Libraries/` layout exactly — the OFX host resolves plugins by that structure, so it cannot be flattened into `lib/`. Give each staged `.ofx` a RUNPATH listing both its own `Libraries/` and the bundle's `lib/`, computed from where the file lands the way `set_bundle_runpath()` already does rather than hardcoded. Decide deliberately whether a library a plugin needs goes in that plugin's `Libraries/` or in the shared `lib/` — prefer the shared one where the main app already stages the same soname, so the bundle does not ship two copies that could diverge.
  - verify: `check-relocatable.sh` passes with the plugins staged; a plugin's dependency resolves to the bundle's copy.
  - size: L

- [x] M23.P4.T2 — Assert the plugins actually load in the startup gate
  - files: `tools/release/check-startup.sh`
  - approach: `check-relocatable.sh` proves the ELF closure resolves, which is not the same as the OFX host accepting the bundle. Extend the engine-startup probe to assert a non-zero count of loaded OFX plugins and that a few expected ones by name (a reader, a writer, Merge) are present. A bundle whose plugins silently fail to load must fail packaging — that is the state shipped today.
  - verify: the gate fails on a bundle with the plugins removed or with a deliberately broken plugin RUNPATH, and passes with them staged correctly.
  - size: M

## Decisions

- 2026-09-07 — a release bundle must be staged from a non-`Debug` build tree, and
  the gate now says so by name. `CMakeLists.txt` adds `-DDEBUG` for
  `CMAKE_BUILD_TYPE=Debug`, which arms `FE_DIVBYZERO|FE_INVALID|FE_OVERFLOW` for the
  whole process at the top of `App/NatronApp_main.cpp:66`; the host's software GL
  driver raises one while compiling its own shaders during `createContextGLX()`, so
  the GUI stage dies of SIGFPE inside `libgallium`. Nothing in the staged tree is
  involved and neither the excludelist nor the closure can change it. Established by
  swapping binaries between a passing and a failing bundle in both directions, and
  confirmed by an `LD_PRELOAD` shim stubbing `feenableexcept` to a no-op, which makes
  the same bundle pass. `build/release` (RelWithDebInfo) from `main` at `8bb4403f2`
  stages and passes both checks.

- 2026-09-07 — `check-startup.sh`'s header claim that "Xvfb has no GLX" was wrong and
  load-bearing. The test server does serve GLX, through the host's software
  rasteriser. It also *has* to: Qt 6.8 builds its backing store through `QRhiGles2`,
  and under `Xvfb -extension GLX` the GUI hangs for 180s constructing its main window
  (`QRhiGles2: Failed to create context`). So "just disable GLX" is not available as a
  way around the trap above. This does not contradict
  `DECISIONS/2026-09-02-no-glx-under-xvfb.md`, which is about the CI image's Xvfb; the
  host running this gate has a working software GLX.

## Phase 23.5: Do not starve the host's GL driver

Added 2026-09-08, from the milestone gate's first run on a bare Debian 12 desktop.
The interpreter, engine, OFX-registry, fontconfig and OCIO probes all pass there;
the xcb GUI does not:

```
libGL: MESA-LOADER: failed to open /usr/lib/x86_64-linux-gnu/dri/swrast_dri.so:
  <bundle>/lib/libstdc++.so.6: version `GLIBCXX_3.4.30' not found
  (required by /lib/x86_64-linux-gnu/libLLVM-15.so.1)
```

The host's Mesa is present and correct. The bundle ships `libstdc++.so.6` at
GLIBCXX 3.4.29; the host's is 3.4.30 and its `swrast_dri.so` needs that through
`libLLVM-15`. The binaries' RUNPATH is `$ORIGIN/../lib`, so the bundled copy is
already resident by SONAME when Mesa `dlopen`s the driver, and the loader reuses it
rather than the host's newer one. GLX context creation then fails and Qt 6.8's
QRhiGles2 main window construction takes the process down with `GLXBadFBConfig`.

Bundling `libstdc++` is what lets the bundle run on hosts *older* than the build
toolchain, so simply excludelisting it trades this failure for the opposite one.
The choice has to be made at startup, against the host actually present.

- [x] M23.P5.T1 — Prefer whichever `libstdc++`/`libgcc_s` is newer, the host's or the bundle's
  - files: `tools/release/make-appimage.sh` (the generated `AppRun`), `tools/release/stage-bundle.sh`, `tools/release/make-tarball.sh` as needed
  - approach: at launch, compare the host's `libstdc++.so.6` against the bundled one by maximum `GLIBCXX_` version and use the newer; same for `libgcc_s`. This is the established AppImage answer to the problem and it must hold for the **tarball** too, which has no `AppRun` — the executables carry a baked-in `$ORIGIN/../lib`, so a launcher shim or an equivalent mechanism is needed there rather than an AppImage-only fix. Do not solve it by excludelisting `libstdc++`: that breaks every host older than the build toolchain, which is the case the bundling exists for. State the selection rule and where it runs.
  - verify: on this host, the extracted AppImage passes `check-startup.sh`'s GUI stage, and `LIBGL_DEBUG=verbose` shows `swrast_dri.so` loading. The tarball bundle passes the same check. Neither check regresses inside the dev container, where the bundled libstdc++ is the newer one and must still win.
  - size: L

- [ ] M23.P5.T2 — Make the gate run somewhere the bundle is not already at home
  - files: `tools/release/check-startup.sh` or the packaging entry points, `tools/ci/local/README.md`
  - approach: the defect above survived every in-container check and was caught only by running on the host by hand. Whatever is cheapest that makes "does this start where the libraries are absent" a repeatable step rather than a manual one — a documented host-side invocation is acceptable if a second container is not. The point is that the next person does not have to think of it.
  - verify: the step exists, is documented, and fails on a bundle with P5.T1 reverted.
  - size: M

**Verification gate:** a freshly staged bundle passes both the relocatability check and the offscreen smoke test; each check fails on a bundle with its corresponding defect reintroduced; an AppImage built from the fixed tooling starts, opens a window, and loads its OFX plugins on a desktop that has neither the ASWF VFX libraries, nor Qt's xcb dependencies, nor a matching Python installed.
