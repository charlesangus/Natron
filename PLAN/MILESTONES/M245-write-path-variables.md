# M245 - Write Path Variables

The native Write does not expand project path variables in its output path. A comp whose Write
is `[Project]/../../../build/bench/m82-profile/out/keying_grade.####.exr` fails with
`Could not open "[Project]/../../../build/bench/m82-profile/out/.keying_grade.0001.tmp-<pid>-<n>.exr"
(No such file or directory)`, from any working directory, GPU on or off. All three bench comps
(`keying_grade`, `cg_multipass`, `defocus_retime`) are affected, so `tools/bench/comps/run_comps.sh`
is broken. Found by the M239 - Weight-Based GPU Placement bench on the GPU host; the workaround
there was copies of the comps with absolute Write paths.

Cause, read from the code at origin/main (`52d58e736` and later): expansion was never a property of
the file knobs. It lived in the OFX proxy layer, `OfxStringInstance::get` →
`projectEnvVar_getProxy` → `Project::canonicalizePath` (`Engine/OfxParamInstance.cpp`), so every
OFX Write got expanded paths. M76 - Native Write (`ca130dcf0`, 2026-10-12) replaced the OFX Write
with `NativeWrite`, which reads its path with `KnobOutputFile::generateFileNameAtTime`
(`Engine/KnobFile.cpp`) — and that function builds the name from the raw `getValue()` with no
expansion. The literal `[Project]` therefore reaches `OiioWriteSupport::temporarySibling` /
`writeFile` (`NativeWrite.cpp`, the `generateFileNameAtTime` call in the render path). The input
side was patched piecemeal: M75 - Native Read canonicalizes the pattern for its frame listing
(`NativeRead::frameListing`), and M100 - Content Node Hashing added `KnobFile::getResolvedPath`,
used by `USDRead` and the OCIO file knobs. `KnobFile::getFileName` still does not expand, and
`NativeRead::pathOf` (sequence frames) and `LensDistortion` use it. `DeepRead`, `DeepWrite` and
`USDWrite` go through `getFileName` / `generateFileNameAtTime` too. Neither bench nor the
existing tests caught it because the bench comps are the only graphs that use `[Project]` in a
Write, and the Serialization tests only compare stored values.

This milestone moves expansion into the two knob-level name generators so every consumer gets it,
adds headless render tests, and gets the bench running on the original comps.

## Scope

**In:**
- `KnobOutputFile::generateFileNameAtTime` and `KnobFile::getFileName` resolve project path variables (and the relative-to-project rule) before generating the per-frame name, through the existing `Project::canonicalizePath`.
- Every consumer inherits it: `NativeWrite`, `DeepWrite`, `USDWrite`, `NativeRead` sequence frames, `DeepRead`, `LensDistortion`.
- A gtest that renders a Write and reads back through a Read with `[Project]` and a user-defined variable in both paths.
- `tools/bench/comps/run_comps.sh` renders all three original comps.

**Out:**
- Changing which variables exist or how the project variable table works — none.
- Removing the now-redundant `projectEnvVar_getProxy` in `OfxParamInstance.cpp` — canonicalizing twice is idempotent; left alone.
- Rewriting the stored knob value to the expanded form — the stored value keeps `[Project]` (it is what is saved and shown).

**Edge cases:**
- A relative Write path: `canonicalizePath` prepends the `[Project]` directory, as the OFX Write did. Output moves for a headless comp whose project directory differs from the working directory; that is the pre-M76 behaviour.
- An unknown variable (`[Nope]/a.exr`) stays literal and fails to open with the unexpanded name in the error, as before.
- A frame pattern (`####`, `%04d`, `%v`) is expanded after variables, so a variable value containing `#` is not a pattern.
- An empty path stays empty.
- Loading a `.ntpz` headless: the `[Project]` row must hold the bundle's directory by the time the render reads the path (see Assumptions; M245.P3.T1 checks).

**Assumptions:**
- `NatronRenderer -w` sets the `[Project]` variable on project load. Unverified; `Tests/Serialization/ProjectOpen_Test.cpp` suggests it does for `.ntp` and the bundle case is covered by `ProjectBundleRoundTrip_Test.cpp`. If a headless `.ntpz` load leaves it empty, M245.P3.T1 fixes that in the project loader and the task grows.
- The `[Project]` of the bench comps resolves to the directory holding the `.ntpz` (`tools/bench/comps/build_comps.py` says so).
- No absolute-path comp copies were committed (`tools/bench/comps/` holds only the three originals), so there is nothing to remove from the tree.

## Phase 245.1: Expand at the knob layer

- [ ] M245.P1.T1 — Resolve project variables in `generateFileNameAtTime` and `getFileName`
  - files: `Engine/KnobFile.cpp`, `Engine/KnobFile.h` (doc comments only)
  - approach: in `KnobOutputFile::generateFileNameAtTime` and the input branch of `KnobFile::getFileName`, run the raw `getValue()` through the same step `KnobFile::getResolvedPath` uses (`getApp()->getProject()->canonicalizePath`, guarded for a holder with no app) before `SequenceParsing::generateFileNameFromPattern`. Factor the guarded call into one private helper both `getResolvedPath` and the two generators use, rather than a third copy. The non-input-image branch of `getFileName` returns the resolved value too. Document on both functions that the result is expanded.
  - verify: builds; `ctest -R "NativeWrite|NativeRead|DeepReadWrite|KnobFile"` still green (existing tests set absolute paths, so they are unchanged by this task).
  - size: M

- [ ] M245.P1.T2 — Audit the remaining raw `getValue()` consumers of file knobs
  - files: `Engine/Nodes/IO/NativeWrite.cpp`, `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/Deep/DeepRead.cpp`, `Engine/Nodes/Deep/DeepWrite.cpp`, `Engine/Nodes/Scene/USDWrite.cpp`
  - approach: list every `filename->getValue()` in these files that feeds a filesystem call, a stat, a hash or a read-back (not an extension or empty check, which are fine on the raw value) and route it through `getResolvedPath()`. Known so far: `NativeWrite` read-back and overwrite check go through `generateFileNameAtTime` (covered by T1), the extension checks use the raw value (leave). Record the audited list in the commit message; add nothing if the audit finds nothing.
  - verify: builds; `ctest -R "NativeWrite|DeepReadWrite|USD"` green.
  - size: M

## Phase 245.2: Tests

- [ ] M245.P2.T1 — gtest: Write and Read expand `[Project]` and user variables in a headless render
  - files: `Tests/Native/NativeWritePathVariables_Test.cpp`, `Tests/CMakeLists.txt` (or the Tests source list the neighbouring `NativeWriteEntryPoints_Test.cpp` is registered in)
  - approach: model on `Tests/Native/NativeWriteEntryPoints_Test.cpp` and its `BaseTest.h` setup. Create a `QTemporaryDir`; set the project's `[Project]` row to it (as `Tests/Serialization/SyntheticProjectGraphs.h` does) and add a user row `[Out]` pointing at a subdirectory. Cases: Write `[Project]/w.####.exr` renders frame 1 and `w.0001.exr` exists under the temp dir; Write `[Out]/w.####.exr` likewise; a relative path `rel/w.####.exr` lands under `[Project]`; then a Read with `[Project]/w.####.exr` and one with `[Out]/w.####.exr` read the frame back with the same pixels (sequence frame path, not only the listing); an unknown `[Nope]/w.exr` render fails and no file is created. Assert the stored knob value still reads `[Project]/...`. Also a `DeepWrite`/`USDWrite` case if the fixtures of `DeepReadWrite_Test.cpp` make it a few lines, otherwise leave them to the T1 shared path.
  - verify: the test fails on a build without M245.P1.T1 (run once against origin/main sources in the batch build to confirm it is not vacuous) and passes with it.
  - size: M

## Phase 245.3: The bench

- [ ] M245.P3.T1 — Prove `run_comps.sh` on the original comps and fix what the render still needs
  - files: `tools/bench/comps/run_comps.sh`, `tools/bench/README.md`
  - approach: with the fix built, run `tools/bench/comps/run_comps.sh 1 2` and confirm all three comps render under `build/bench/m82-profile/out/` and produce `*.steady.jsonl`. Check by hand that the headless `.ntpz` load gives `[Project]` the comp's directory (the Assumption above); if it does not, make the loader set it and add a case to M245.P2.T1 before continuing. Keep the script's contract unchanged (originals only, no absolute-path copies); if the README mentions the absolute-path workaround, remove that note.
  - verify: `tools/bench/comps/run_comps.sh 1 2` exits 0 and prints three `frames 1-2 wall` lines; the temp `.tmp-*` siblings are gone and the `out/` dir is cleaned by the script.
  - size: M

**Verification gate:** the release gate is green (`tools/ci/remote/build.sh release --tests`, including `NativeWritePathVariables_Test`), and `tools/bench/comps/run_comps.sh` renders `keying_grade`, `cg_multipass` and `defocus_retime` from the original `.ntpz` files.

## Decisions

- 2026-10-28: implemented (build pending). Variables expand before the frame pattern, so a `#` inside a variable's value becomes a frame pattern (as Read's frame listing and hashing already do); the edge case saying otherwise is superseded. An unknown variable in a relative path is reported as <project dir>/[Nope]/…. No DeepWrite/USDWrite cases in the gtest (they share the KnobFile path). Bench comps re-run pending a build.
