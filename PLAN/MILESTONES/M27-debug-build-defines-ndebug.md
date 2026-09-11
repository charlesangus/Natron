# Milestone 27: Make the debug build a debug build again

`tools/ci/local/build.sh debug` compiles with **both `-DDEBUG` and `-DNDEBUG`**,
so `assert()` expands to nothing: measured from
`build/debug/compile_commands.json` for `Engine/EffectInstanceRenderDeep.cpp`,
which carries `-DDEBUG -DNDEBUG -g` and no `-O` flag. Every assertion in the
engine — including the ones M18's deep render path adds deliberately to catch
node-author bugs loudly — is inert in the only build anyone debugs in. Found
while implementing M18.P2.T2.

The likely source is `Shiboken6::libshiboken`'s
`INTERFACE_COMPILE_DEFINITIONS`: `CMakeLists.txt:97-102` strips `NDEBUG` from it
for debug builds, but that block is gated on `WIN32`, and this is a Linux-only
fork. Confirm before fixing — a `CMAKE_CXX_FLAGS_DEBUG` or toolchain-level
source would look identical from the compile line.

## Phase 27.1: Fix and prove it

- [ ] M27.P1.T1 — Stop `NDEBUG` reaching debug builds
  - files: `CMakeLists.txt`, `Engine/CMakeLists.txt` (only if the leak is not the Shiboken interface), `tools/ci/local/README.md`
  - approach: find where `-DNDEBUG` enters a debug compile — start by narrowing the `WIN32` gate at `CMakeLists.txt:97-102`, and verify with `cmake --graphviz` or by dumping `INTERFACE_COMPILE_DEFINITIONS` on the Shiboken and PySide targets rather than by assuming. Fix it at the source so it holds for every target, not just `NatronEngine`; a blanket `remove_definitions(-DNDEBUG)` is the wrong shape because it leaves a release build's own `NDEBUG` at the mercy of target ordering. Check whether the debug build should also be carrying an explicit `-O0` while in here, and say so rather than widening scope.
  - verify: `grep NDEBUG build/debug/compile_commands.json` comes back empty for `Engine/`, `Gui/` and `Tests/` translation units after a `--reconfigure`; a temporary `assert(false)` in an engine function reached by a ctest case aborts the run (remove it after); the release build still defines `NDEBUG`; whole ctest suite green.
  - size: M

- [ ] M27.P1.T2 — Audit what the inert assertions were hiding
  - files: wherever the audit leads; expected `Engine/EffectInstanceRenderDeep.cpp`, `Engine/DeepFlatten.cpp`, `Engine/Node.cpp`
  - approach: with assertions live, run the whole ctest suite and fix what fires. Assertions that have been no-ops for the life of this fork may encode assumptions the code has since broken, and M18 added several on purpose (the TLS frame-args requirement in `renderDeepRoIFlattened`, the single-`splitRoi` assumption). An assertion that fires is either a real defect or a stale assertion — decide which per case and say which in the commit, never silence one to get green.
  - verify: whole ctest suite green with assertions live, and each assertion changed or removed is justified in the commit message.
  - size: M

**Verification gate:** no debug translation unit defines `NDEBUG`; a deliberate `assert(false)` aborts a ctest case; the release build still defines `NDEBUG`; whole ctest suite green with assertions live.
