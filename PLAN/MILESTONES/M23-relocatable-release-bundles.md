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

- [ ] M23.P1.T1 — Walk plugin dependencies, not just executables
  - files: `tools/release/stage-bundle.sh`
  - approach: the ldd-closure walk must seed from every binary the bundle ships that can be `dlopen`'d — the Qt plugin tree under `plugins/` as well as `bin/`. Iterate to a fixed point so a dependency pulled in by a plugin has its own dependencies staged too. Keep honouring `excludelist.txt` for libraries that must come from the host (glibc, GL). Qt 6.5+ requires `xcb-cursor`; do not special-case it, fix the walk so it falls out.
  - verify: the staged tree contains the xcb libraries `libqxcb.so` needs; no staged binary reports a missing direct dependency when resolved against the bundle alone.
  - size: M

- [ ] M23.P1.T2 — Set RUNPATH on every staged library
  - files: `tools/release/stage-bundle.sh`
  - approach: the existing step patches only `bin/*`. Every library staged into `lib/` needs its own RUNPATH (`$ORIGIN`, and `$ORIGIN/../lib` where nesting requires it), and so does every staged plugin, since `DT_RUNPATH` does not chain. Set it as part of staging each file rather than as a separate pass over a hardcoded list, so nothing added later is missed.
  - verify: no staged `.so` is left without a RUNPATH; the resolved dependency of a bundled library is the bundled copy, not a system one.
  - size: S

## Phase 23.2: Verify somewhere the libraries are absent

- [ ] M23.P2.T1 — A relocatability check that cannot pass falsely
  - files: `tools/release/` (new check script), `tools/ci/local/README.md`
  - approach: a check that resolves every staged binary's dependencies **against the bundle alone**, ignoring system paths and `ld.so.cache`, and fails on anything unresolved that is not on the deliberate host-provided excludelist. This is what makes the gate meaningful: it must give the same verdict inside the dev container as on a bare desktop. A container without the VFX libraries installed is the stronger check if it can be arranged cheaply; the dependency-resolution check is the minimum.
  - verify: run against a bundle staged before M23.P1's fixes — it must fail, naming the unresolved libraries; run against one staged after — it must pass.
  - size: M

- [ ] M23.P2.T2 — Wire the check into the release path
  - files: `tools/release/make-appimage.sh` or the packaging entry point, `.github/workflows/` as appropriate
  - approach: run the check as part of producing a release artifact, so a bundle that is not self-contained cannot be published. Match how the existing release workflow is structured rather than adding a parallel one.
  - verify: the packaging path fails loudly on a deliberately broken bundle.
  - size: S

**Verification gate:** a freshly staged bundle passes the relocatability check; the same check fails on a bundle staged with either defect reintroduced; an AppImage built from the fixed tooling starts and opens a window on a desktop that does not have the ASWF VFX libraries or Qt xcb dependencies installed.
