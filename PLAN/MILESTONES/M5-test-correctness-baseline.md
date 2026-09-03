# Milestone 5: Test & correctness baseline

ongoing after M4 · low risk. The foundation is "clean" once it's green *and*
hard to silently break again. That intent is unchanged. The breakdown below
replaces the original three tasks, which were written before M2, M3, M10 and
M13 shipped and no longer describe this tree.

## What is actually there (measured 2026-09-02, not estimated)

`Tests/CMakeLists.txt:57` holds **one** `add_test(NAME Tests COMMAND Tests)`,
running 30 gtest cases across 15 suites in 10 files. Keeping that green is
already mechanically enforced — `build-and-test` is a required check on `main`
— so "keep the suite passing" is not a task and is not restated below.

What those 30 cases cover is narrower than the file list suggests:

| file | cases | what it actually tests |
|---|---|---|
| `KnobFile_Test.cpp` | 7 | `libs/SequenceParsing`. Not knobs. |
| `Tracker_Test.cpp` | 6 | openMVG's Prosac estimator. Not `TrackerContext`, not `TrackMarker`. |
| `Image_Test.cpp` | 3 | `Bitmap` rect bookkeeping and `ImageKey` hashing. Never constructs an `Image`. |
| `FileSystemModel_Test.cpp` | 3 | path splitting and cleaning, a third of it Windows drive letters. |
| `BaseTest.cpp` | 3 | one 1-frame render, knob set/interpolate, connect/disconnect. |
| `Curve_Test.cpp` | 2 | keyframe and curve basics. |
| `ProjectOCIO_Test.cpp` | 2 | project load puts unresolvable colorspaces into an error state. |
| `OSGLContext_Test.cpp` | 2 | **nothing — see `M5.P1.T3`.** |
| `Hash64_Test.cpp` | 1 | the hash primitive. Not the cache-key invariant resting on it. |
| `Lut_Test.cpp` | 1 | integer width conversions. No colour transform. |

`tools/ci/smoke_test.py`, run as its own CI step through `NatronRenderer`,
already covers the end-to-end headless path and is **not** re-specified by any
task below: the OFX bundle set plus a `dlopen`/`OfxGetPlugin` probe, PySide6 and
shiboken6 liveness, `app.render([(writer, 1, 1)])` over PNG→PNG, that the active
OCIO config is Natron's own default (`studio-config-v4.0.0_aces-v2.0_ocio-v2.5`),
and that scene-linear 0.18 survives an EXR→8-bit-sRGB-PNG render as code value
118 ± 2.

The gaps worth closing first, for a compositor whose value is correct pixels:
`Engine/Lut.cpp` (the transforms the viewer and node previews use),
`Image::convertToFormat` (the engine's own depth/premult converter), the
`Hash64` cache-key invariant, `Engine/CLArgs.cpp` (1273 lines, the whole
`NatronRenderer` entry surface, zero coverage), project save/load round-tripping,
and any render longer than one frame.

**Scope.** This milestone delivers the safety net — coverage and regression
infrastructure — plus two deliberate exceptions. Phase 5.2 changes behaviour only
where a test could otherwise not observe a failure at all. Phase 5.5 fixes one
defect, folded in on the user's instruction after the safety-net work surfaced it
(see `## Decisions`); it is the single bug fix in this milestone and does not
reopen the cancelled list below.

> **Cancelled, 2026-09-02 — the inherited P0 task.** The original `M5.P1.T3`
> ("Work the existing P0 list once CI can catch regressions") is dropped, not
> deferred and not rewritten. Its upstream issue numbers (#192, #248, #795,
> #845, #864, #1008, #1029, #1057) were scraped from
> `Documentation/source/maintainers/todo.rst`, which predates this fork's first
> commit by six weeks; see
> `DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`. It was also not
> a task by this project's own sizing rule — "fix seven bugs, size L, varies per
> bug" is a phase at best, and its verification gate could never close. Nothing
> below replaces it: M5 delivers the safety net, and anything the safety net then
> reveals goes to the user as a finding, not into this milestone.

## Phase 5.1: Make the suite report what it actually ran

- [x] M5.P1.T1 — Report one ctest case per gtest case
  - files: `Tests/CMakeLists.txt`
  - approach: `Tests/CMakeLists.txt:57` is a single `add_test(NAME Tests COMMAND
    Tests)`, so `ctest` reports `1/1 passed` whether the binary ran 30 cases or
    zero, and dropping a file from `Tests_SOURCES` is invisible in the result.
    Replace it with `include(GoogleTest)` and `gtest_discover_tests(Tests
    DISCOVERY_MODE PRE_TEST)`. `PRE_TEST`, not the default `POST_BUILD`:
    discovery runs the binary, and `Tests/wmain.cpp` constructs an `AppManager`
    and calls `manager.load()` before `RUN_ALL_TESTS()` even under
    `--gtest_list_tests`, so it needs `libpython3.13`, `OFX_PLUGIN_PATH` and the
    built bundles — all present at `ctest` time under `tools/ci/local/test.sh`,
    none guaranteed at build time.
  - verify: `tools/ci/local/test.sh ctest debug` lists 30 cases by name instead
    of reporting `1/1`; deleting a file from `Tests_SOURCES` lowers the count.
  - size: S

- [ ] M5.P1.T2 — Bound every test's runtime
  - files: `Tests/CMakeLists.txt`, `tools/ci/local/test.sh`
  - approach: nothing bounds a test's runtime today, and the hang this guards
    against is reachable in current code, not hypothetical:
    `Engine/OutputSchedulerThread.cpp:1443` waits on `bufEmptyCondition` with no
    predicate, no loop and no timeout, so a wakeup lost between the buffer check
    and the wait parks the scheduler indefinitely with the process alive. Set
    `TIMEOUT` on the discovered tests — the whole current suite runs in 884 ms in
    a local debug build and the slowest case (`BaseTest.GenerateDot`) in 176 ms,
    so 300 s per case is several orders of headroom and still converts a hang
    into a red test within minutes. Bound the smoke step too: `test.sh`'s `smoke`
    branch `exec`s `NatronRenderer` bare, so a hang there wedges the job until
    GitHub's six-hour default kills it, with no diagnostic and no ctest timeout
    in play. Both harnesses, or the safety net has a hole in exactly the place
    `M5.P4.T3` is about to start rendering.
  - verify: `ctest -V` prints a per-test timeout; a temporary `sleep` longer than
    the timeout in one case fails that case by name and leaves the rest green.
  - size: S

- [ ] M5.P1.T3 — Stop reporting the two OpenGL tests as passing when they do not run
  - files: `Tests/OSGLContext_Test.cpp`,
    `PLAN/DECISIONS/2026-09-02-no-glx-under-xvfb.md`, `PLAN/DECISIONS/INDEX.md`
  - approach: `OSGLContext.Basic` and `GPUContextPool.Basic` both open with
    `if (!appPTR->isOpenGLLoaded()) { std::cerr << "Skipping test..."; return; }`
    — a bare `return`, which gtest scores as a pass. In
    `aswf/ci-vfxall:2027-clang21.1` under `xvfb-run` that branch is always taken:
    the ctest log records `Error while loading OpenGL: GLX: No GLXFBConfigs
    returned` and `OpenGL rendering is disabled`, so the suite reports
    `[ PASSED ] 30 tests` while executing 28. Measured directly against the
    image, not inferred: `Xvfb` there yields no GLX visual with
    `+extension GLX`, with `+iglx`, or with `LIBGL_ALWAYS_SOFTWARE=1
    GALLIUM_DRIVER=llvmpipe`, though `swrast_dri.so` and `libGLX_mesa.so.0` are
    both installed — this is not a flag away. Rename both to `DISABLED_Basic` so
    gtest prints `YOU HAVE 2 DISABLED TESTS` and the count tells the truth, and
    comment each with the measured cause and with how to run them for real:
    `--gtest_also_run_disabled_tests` inside `devshell.sh` on a host with
    `/dev/dri`, which `devshell.sh` already passes through. The vendored gtest
    predates `GTEST_SKIP` (the file's own TODO says so), so `DISABLED_` is the
    mechanism available. Record the measurement as a decision rather than only a
    comment: restoring GL coverage means an EGL-surfaceless path in
    `Engine/OSGLContext_x11.cpp` or a different X server, which is a project,
    not a task.
  - verify: `test.sh ctest debug` reports 28 run and 2 disabled where it reported
    30 passed; on a host that passes `/dev/dri` through `devshell.sh`, the two
    still pass under `--gtest_also_run_disabled_tests` (if no such host is
    available, say so rather than assuming it).
  - size: S

**Verification gate:** `ctest` names every case individually, bounds each one's
runtime, and reports 28 run plus 2 explicitly disabled — a count that matches
what the binary executes. Removing a test file changes the reported number.

## Phase 5.2: Make a failed render detectable

Two of the three gaps `DECISIONS/2026-09-01-render-failure-signalling-gaps.md`
measured in this tree and assigned to M5. They come before the coverage phases
because an assertion that cannot observe a failure is not a test: today a wrong
render and a right one are indistinguishable to anything automated. The
decision's third gap — that nothing refuses to render a graph containing a node
already in an error state — changes render *semantics* rather than what a test
can see, and is deliberately left out of this milestone.

- [ ] M5.P2.T1 — Carry the render result out to NatronRenderer's exit status
  - files: `Engine/AppInstance.cpp`, `Engine/AppInstance.h`,
    `Renderer/NatronRenderer_main.cpp`
  - approach: the failure is already computed and then dropped.
    `OutputSchedulerThread.cpp:1194` emits `s_renderFinished(wasAborted ? 1 : 0)`,
    and every render exception (`EffectInstanceRenderRoI.cpp:1648`/`1653`,
    surfaced at `OutputSchedulerThread.cpp:2369`) routes through
    `notifyRenderFailure` (`:1867`) → `abortRenderingNoRestart()`, so
    `wasAborted` is true. `AppInstance::onQueuedRenderFinished(int /*retCode*/)`
    (`Engine/AppInstance.cpp:1927`) discards it — the parameter name is
    commented out. Latch a sticky failure flag on the `AppInstance` there, expose
    it, and have `main()` in `Renderer/NatronRenderer_main.cpp` — which today
    `return 0`s whenever `manager.load()` succeeded — return non-zero when it is
    set.
  - verify: `NatronRenderer` on a project whose Write node points into a
    non-existent directory prints `Error while rendering` as it does now **and**
    exits non-zero; the smoke test's two successful renders still exit 0, so
    `test.sh smoke debug` is unchanged.
  - size: M

- [ ] M5.P2.T2 — Keep persistent error messages readable in background mode
  - files: `Engine/Node.cpp`
  - approach: `Node::setPersistentMessage` (`Engine/Node.cpp:3795`) branches on
    `!appPTR->isBackground()` at `:3804`; the background `else` at `:3839-3841`
    only does `std::cout << "Persistent message: " << content`, storing nothing,
    so `hasPersistentMessage()` is permanently false in `NatronRenderer` **and in
    the `Tests` binary**, which `Tests/wmain.cpp` constructs with
    `CLArgs(args, true)`. That is why `Tests/ProjectOCIO_Test.cpp` asserts on
    captured stdout rather than on node state, and why no future test can assert
    an error without string-matching a log line. Store the message in the
    background branch too, keeping the print exactly as it is — CI and users read
    it.
  - verify: a new case loads `Tests/fixtures/ocio-old-config.ntp` unmodified and
    asserts `getProject()->getNodeByName("Read1")->hasPersistentMessage()` is
    true, where it is false today; `ProjectOCIOTest`'s two existing stdout
    assertions still pass untouched.
  - size: S

**Verification gate:** a failed headless render is visible to its caller —
`NatronRenderer` exits non-zero on it — and a node's error state is readable from
C++ in background mode instead of only as a printed line.

## Phase 5.3: Cover the pixel path

- [ ] M5.P3.T1 — Test the sRGB, Rec709 and BT1886 transforms
  - files: `Tests/Lut_Test.cpp`
  - approach: `Lut_Test.cpp`'s single case tests only integer width conversions;
    the transfer functions are untested, though `Engine/Lut.cpp` drives
    `ViewerInstance.cpp:138-144` (what the viewer shows),
    `ImageConvert.cpp:113-119`, and `Node.cpp:3214-3216` (node previews). For
    each of `LutManager::sRGBLut()`, `Rec709Lut()` and `BT1886Lut()`
    (`Engine/Lut.h:90-92`): round-trip `toColorSpaceFloatFromLinearFloat` against
    `fromColorSpaceFloatToLinearFloat` across the domain; check the table-driven
    fast paths (`toColorSpaceUint8FromLinearFloatFast`,
    `fromColorSpaceUint8ToLinearFloatFast`) agree with the analytic functions to
    within one code value; and anchor sRGB on scene-linear 0.18 → 118 —
    deliberately the same number `smoke_test.py`'s `SRGB_GREY_CODE` asserts end
    to end, so the unit test and the integration test pin one fact from opposite
    ends.
  - verify: the new cases pass; changing `to_func_srgb`'s exponent
    (`Engine/Lut.h:491`) fails the anchor case with expected and actual code
    values in the message.
  - size: M

- [ ] M5.P3.T2 — Test Image::convertToFormat across depths and premultiplication
  - files: `Tests/Image_Test.cpp`
  - approach: `Image_Test.cpp` never constructs an `Image`.
    `Image::convertToFormat` (`Engine/Image.h:797`, implemented through the
    templated ladder at `Engine/ImageConvert.cpp:491-601`) is the engine's own
    depth/colorspace/premult converter and has no coverage at all. Use the
    local-allocation constructor documented at `Engine/Image.h:186` ("This
    constructor can be used to allocate a local Image") so no cache is involved.
    Fill a float RGBA image with known values; convert to `eImageBitDepthByte`
    and back with matching colorspaces and assert the round trip is within one
    code value; convert with `requiresUnpremult` true against an alpha of 0.5 and
    assert the colour channels double. Keep to the depth pairs that ship —
    float↔byte and float↔short — rather than walking the whole template matrix.
  - verify: the new cases pass; inverting the branch at `ImageConvert.cpp:483`
    fails the unpremult case and leaves the round-trip case green.
  - size: M

- [ ] M5.P3.T3 — Assert the render hash changes when the render would change
  - files: `Tests/BaseTest.cpp`
  - approach: cache correctness in this engine rests entirely on the hash —
    `docs/maintainer-notes.md` states the invariant ("if an input changes, the
    hash changes and the old entry is simply not found — this is why cache
    invalidation stays correct with no explicit dirty-tracking") and nothing
    tests it. `Hash64_Test.cpp` tests the primitive, not the invariant, and a
    hash that fails to change is a stale-pixel bug with no other detector. Using
    the `BaseTest` fixture and the `_generatorPluginID` node it already creates:
    read `Node::getHashValue()` (`Engine/Node.h:224`), assert it is stable across
    a no-op, assert it changes after `setValue` on a knob, and assert connecting
    an input changes the downstream node's hash.
  - verify: the new case passes and adds no fixture — it reuses nodes `BaseTest`
    already builds; removing a knob from the node's hash contribution fails it.
  - size: S

**Verification gate:** the colour transforms, the depth/premult converter and the
cache-key invariant each have a case that goes red when that behaviour breaks,
and `test.sh ctest debug` is green.

## Phase 5.4: Cover the headless entry points

- [ ] M5.P4.T1 — Characterise command-line parsing
  - files: `Tests/CLArgs_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `Engine/CLArgs.cpp` is 1273 lines with no coverage and is the
    entire entry surface of `NatronRenderer`. It needs no app state —
    `CLArgs(const QStringList&, bool forceBackground)` (`Engine/CLArgs.h:83`) is
    public and `Tests/wmain.cpp` already builds one. Cover what the render path
    depends on: `-i <name> <file>` and `-w <name> [<file>]` landing in
    `getReaderArgs()`/`getWriterArgs()`; frame ranges (`10-20`, `1-10,20-30`,
    `1-10:2`, a bare `5`) landing in `getFrameRanges()` with the right step; and
    malformed input setting `getError()` rather than being ignored. Write it as
    characterisation — assert what the code does today and treat a surprise as a
    finding to report, not a test to bend. One is already visible by inspection:
    the optional-filename heuristic for `-w` at `Engine/CLArgs.cpp:1017` excludes
    frame ranges with the anchored pattern `[0-9\-,]*`, which contains no `:`, so
    `-w Write 1-10:2` looks set to swallow the frame range as the writer's
    filename.
  - verify: the new file builds and its cases pass against current behaviour;
    every surprise is written into this milestone's `## Decisions` rather than
    silently asserted away.
  - size: M

- [ ] M5.P4.T2 — Round-trip a project through save and load
  - files: `Tests/ProjectSerialization_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `docs/maintainer-notes.md` calls the `…Serialization` structs "the
    single easiest place to cause data-loss regressions", and nothing round-trips
    them — `ProjectOCIO_Test.cpp` only loads a committed fixture. Build a small
    graph in code (the `BaseTest` generator → `WriteOIIO` chain), set a
    distinctive value on one knob of each kind that matters (int, double, string,
    choice, file), `Project::saveProject()` (`Engine/Project.h:95`) into a
    `QTemporaryDir`, `Project::reset()`, `Project::loadProject()`
    (`Engine/Project.h:87`) it back, and assert node names, connections and every
    knob value survived.
  - verify: the new case passes; the `.ntp` it writes is boost XML like
    `Tests/fixtures/ocio-old-config.ntp`, so a failure diffs by eye. Reordering a
    serialized field without bumping its `BOOST_CLASS_VERSION` fails it.
  - size: M

- [ ] M5.P4.T3 — Render a frame range and assert every frame is its own frame
  - files: `Tests/RenderRange_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: the only render coverage is `BaseTest.GenerateDot`, which renders
    one frame and asserts only that a file exists; nothing renders a range. Build
    `Constant` → `WriteOIIO` — `net.sf.openfx.ConstantPlugin`
    (`PLUGINID_OFX_CONSTANT`, `Engine/EffectInstance.h:65`) is confirmed present
    in the `Misc.ofx` bundle `fetch-assets.sh` builds — keyframe the Constant's
    colour so frame *n* carries an exactly known, distinct value, render 1–5 to
    `render.####.exr`, then read each back and assert its pixel equals the value
    keyframed for that frame. That one assertion catches a scheduler that stops
    dispatching (a file missing) and a writer that numbers or overwrites frames
    wrongly (frames equal), and it is exact rather than approximate because the
    input is synthesised at stated values. It does not cover reader-side frame
    mapping — there is no reader in this graph; see `## Decisions`.
  - verify: five files exist, each carrying its own keyframed value, and the case
    completes well inside the timeout `M5.P1.T2` set. Forcing the writer to
    render frame 1 five times fails it, naming the frames that matched when they
    should not have.
  - size: M

**Verification gate:** the command-line surface, project round-tripping and a
multi-frame render each have coverage; `test.sh ctest debug` and `test.sh smoke
debug` are both green.

## Phase 5.5: Fix the `-i` stale-`timeOffset` defect

Folded into M5 on the user's instruction, 2026-09-02. This is upstream #864 —
one of the eight numbers Phase 5.1's cancellation note drops — and folding it in
does **not** reopen that list. The cancellation was about provenance: those
numbers entered the plan through inherited documentation instead of through the
user. This one is back because the user put it back, having been shown a
reproduction against this tree. Nothing else from that list returns with it.

Test first, then fix, so the test is proven to catch the defect rather than
written against an already-fixed tree.

- [x] M5.P5.T1 — Add a regression test that fails on the stale offset today
  - files: `Tests/fixtures/read-time-offset.ntp` (new), `tools/ci/smoke_test.py`
  - approach: this cannot be a gtest case. The defect lives on the
    `AppInstance::loadInternal` CLI path, and `Tests/wmain.cpp` shares one
    `AppManager` across all cases while `loadInternal` is one-shot per instance —
    so the test drives the built `NatronRenderer` as a subprocess, which
    `smoke_test.py` already does. Build a fixture project with a Read node whose
    **`timeOffset` is non-zero** and a Write node; `.ntp` is boost XML text, and
    `Tests/ProjectOCIO_Test.cpp:53-105` is the existing load-and-substitute
    pattern to copy for path fixups. Render a 3-frame range through
    `-i <read> <file>`, then assert the three outputs are **not** byte-identical
    to each other.
    **The non-zero `timeOffset` is what makes this test real:** a first attempt
    with the default offset of 0 passed green against the broken code. Say so in
    a comment, or the next person will simplify the fixture and silently void the
    test.
  - verify: the new check **fails** against the current tree, naming the frames
    that matched when they should differ. Every existing `smoke_test.py` check
    still passes. Do not fix the defect in this task.
  - size: M

- [x] M5.P5.T2 — Keep `timeOffset` consistent when the filename changes
  - landed: `charlesangus/openfx-io` PR #2, merged as `020d898f9`; pin bumped
    from `40764b207` in `tools/ci/local/fetch-assets.sh`.
  - files: to be determined by the task — either `Engine/AppInstance.cpp`, or
    `charlesangus/openfx-io` plus the `OPENFX_IO_REF` pin in
    `tools/ci/local/fetch-assets.sh`
  - approach: `-i` applies the filename with a bare `KnobFile::setValue`
    (`Engine/AppInstance.cpp:645-654`). The reader's own filename handler then
    refreshes `originalFrameRange`, `firstFrame`, `lastFrame` and `startingTime`
    but never `timeOffset`, and `timeOffset` is the only term in the decode
    mapping (`*sequenceTime = t - timeOffset`, `GenericReader.cpp:693`). The node
    is left internally inconsistent, every render time falls outside the sequence
    domain, and the before/after hold behaviour collapses the range onto one
    frame. `createReader()` escapes it only because a fresh node defaults
    `timeOffset` to 0.
    **Decide where the fix belongs and justify it in the commit message.** The
    plugin already maintains four of the five derived params on a filename
    change, which argues the omission is its bug and the fix belongs in the
    `openfx-io` fork under `DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md`
    — upstreamable, and it fixes every host, not just this one. Against that, a
    host-side fix needs no pin bump and no second repository. Weigh both against
    the evidence rather than taking either as given; the host-side variant in
    particular was only ever a code-reading hypothesis and has not been shown to
    work.
  - verify: `M5.P5.T1`'s check passes, and each rendered frame carries its own
    content. `test.sh ctest debug` and `test.sh smoke debug` are both green, and
    a project whose reader has a zero `timeOffset` renders exactly as it does
    today.
  - size: M

**Verification gate:** a project whose Read node carries a non-zero `timeOffset`
renders distinct frames through `-i`, and the check that proves it was watched to
fail before the fix landed.

## Decisions

- **#864 is folded in, and only #864.** The user reinstated it on 2026-09-02
  after being shown a reproduction against this tree. This does not weaken
  `DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`: that rule is
  about how work *enters* the plan, and #864 entered this time through the user,
  which is exactly the channel the rule prescribes. Bad provenance never made the
  defect unreal — the earlier triage established that most of the eight were
  Windows-only or Qt5-bundle-specific, but never established anything either way
  about #864 specifically, and reporting the cancellation as though it had was an
  overreach on the PM's part. The other seven stay cancelled and are not to be
  revisited without the same explicit instruction.

- **The `-i` fix went plugin-side, and the host-side hypothesis was disproved,
  not merely rejected.** `GenericReaderPlugin` keeps one time mapping in two
  params under the invariant `timeOffset == startingTime - firstFrame`; three of
  the four `changedParam` branches maintain it, and the `kParamOriginalFrameRange`
  branch that fires on a file change resets `firstFrame`/`lastFrame`/`startingTime`
  while leaving `timeOffset` stale. `getTimeDomain()` reads `startingTime` and
  `getSequenceTime()` reads `timeOffset`, so the node advertises a range it cannot
  decode. A probe setting the filename through the **Python API** — no `-i`
  anywhere — reproduced the identical inconsistency, because
  `OfxEffectInstance.cpp:2480` maps Natron's internal edit reason to
  `kOfxChangeUserEdited`, exactly as a GUI file-dialog pick does. So `-i` is one
  of three doors into one bug. A host-side fix in `AppInstance.cpp` *would* have
  turned the test green (`timeOffset->setValue(0)` re-enters the plugin's own
  handler) while leaving the GUI and script paths broken and hard-coding a plugin
  invariant into the host — cheap and wrong. Chosen semantics: `timeOffset = 0`,
  because the plugin has just reset `startingTime` to the new sequence's first
  frame and the two must agree. This is not a semantics change; it makes a hidden
  param follow the visible one it is derived from.

- **The PR will not carry `Closes #864`.** That number belongs to
  `NatronGitHub/Natron`, not to this fork; a closing keyword would silently do
  nothing, or worse, resolve against an unrelated issue in this repo.

- **`M5.P1.T1` needed a second file the brief did not name.** With `PRE_TEST`
  discovery wired up, `ctest` reported **46** cases rather than 30.
  `Tests/wmain.cpp` reaches `AppManager::load()` even under
  `--gtest_list_tests`, and in a debug build `Global/PythonUtils.cpp`'s
  `initializePython3` dumps `PATH`, `sys.path` and friends to **stdout** — the
  same channel gtest writes its machine-readable listing to. CMake's
  `GoogleTestAddTests.cmake` captures only stdout, so the two-space-indented
  diagnostic lines following a suite header parsed as sixteen extra test names
  under `ModelSearch.`. Redirecting that block to stderr (`fprintf`,
  `PySys_*Stderr`) fixes it with no loss of output. Committed separately from
  the CMake change, since it is a distinct defect that merely happened to
  surface here.

- **No golden image.** A committed reference frame would not earn its
  maintenance cost here. The pixels come out of `openfx-io`, `openfx-misc` and
  `openfx-arena` bundles pinned by SHA in `fetch-assets.sh` and built against the
  container's OIIO, OCIO and ImageMagick, so a legitimate pin bump changes the
  bytes and the golden must be regenerated for a reason that is not a Natron
  regression — and a binary blob in a diff is unreviewable, so regenerating it is
  indistinguishable from papering over a real break. This fork already has the
  better pattern and it is in `smoke_test.py`: synthesise the input at a stated
  value and assert a named number in code with a stated tolerance
  (`SCENE_LINEAR_GREY = 0.18`, `SRGB_GREY_CODE = 118`, `SRGB_GREY_TOLERANCE = 2`,
  each carrying a comment naming the failure it exists to catch). `M5.P3.T1` and
  `M5.P4.T3` follow it. If a full-frame comparison is ever genuinely needed, the
  robust form is `oiiotool --diff` with an explicit failure threshold against an
  image the test *generates*, never one committed beside it.

- **The suite runs 28 tests, not 30.** Measured from
  `build/debug/Testing/Temporary/LastTest.log`, not inferred — see `M5.P1.T3`.

- **Reader-side frame mapping stays uncovered, knowingly.** `M5.P4.T3` renders
  from a generator, so nothing in this milestone exercises how a Read node maps
  an output frame to a file on disk. Covering it needs a reader whose
  `timeOffset` is non-zero, which is a different fixture and a different test
  shape (`AppInstance::loadInternal` is one-shot per instance, so it cannot live
  in the shared-`AppManager` gtest binary). Recorded as a gap rather than quietly
  implied to be covered.

- **No bug list.** The upstream issue numbers the previous breakdown carried
  entered M5 through inherited upstream documentation, not through the plan; see
  the cancellation note above and
  `DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`. M5 delivers the
  safety net. Anything the safety net then reveals goes to the user, not into
  this milestone.

- **The third render-signalling gap stays out.** Making a persistent error state
  refuse to start a render is a change to render semantics — it can reject work
  that renders correctly today, and it needs `M5.P2.T2` before the flag is even
  readable in background mode. `M5.P2.T1` and `M5.P2.T2` are in because they make
  a failure *observable*, which is a precondition for every test in Phases 5.3
  and 5.4. Fixing what the observation then shows is not this milestone.

**Verification gate:** `ctest` reports a per-case, time-bounded result whose count
matches what the binary actually runs; a failed headless render exits non-zero;
and the colour transforms, the depth/premult converter, the cache-key invariant,
the command-line surface, project round-tripping and a multi-frame render each
have a case that fails when that behaviour breaks. The `-i` stale-`timeOffset`
defect is fixed, with the check that proves it having been watched to fail first.
`format`, `lint-ci` and `build-and-test` are all green.
