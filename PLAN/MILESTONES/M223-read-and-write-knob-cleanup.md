# M223 - Read And Write Knob Cleanup

UAT follow-ups from M167 - Per-Frame Node Metadata (user, 2026-10-26). The Read shows one Reload: its labelled button. The per-file-knob reload icon is hidden on nodes that own a Reload button. The Write always writes its input's format; the Format Type and format knobs and the project/fixed code paths are deleted, not hidden.

Acceptance sketch:
- A Read panel has one Reload button, and it reloads.
- A Write has no format-type or format knobs; the written file's size is the input's format/RoD; code paths for the other modes are deleted.
- Tests cover the Write's output size following the input; the Read's knob list has one Reload.

## Scope

**In:**
- Delete `formatType`, the `ExtentKnobs` format knobs, `FormatTypeEnum` and `kNatronWriteFormatType*`. Keep `clipToRoD` (data window = input RoD, display window = input format).
- `KnobFile::setShowsReloadButton(bool)`, set to false on NativeRead's File and Proxy File, and on the OCIO file nodes (`kOcioFileParamReload`) and USDRead (`kUSDReadParamReload`), which show the same double Reload.

**Out:**
- Reformatting on write: Reformat node upstream.

**Edge cases:**
- The input format is empty: fall back to the input RoD, not the project format.
- An old project with `formatType`: no compatibility; the knob is simply gone.

## Phase 223.1: Knob cleanup

- [ ] M223.P1.T1 — Write always writes the input format
  - files: `Engine/Nodes/IO/NativeWrite.h`, `Engine/Nodes/IO/NativeWrite.cpp`
  - approach: delete the knobs and members (cpp:861-881, h:64,84-87,330-333). `selectedFormat` (cpp:1806) returns the input's `getOutputFormat()` and PAR, else the RoD. Remove the format branches in the refresh code (cpp:1638-1667) and in `knobChanged` (cpp:1750-1771). Reword the clipToRoD hint and the header comment (h:105).
  - verify: at the phase build, `ctest -R NativeWrite` passes (T2).
  - size: M
- [ ] M223.P1.T2 — Update and extend the Write tests
  - files: `Tests/Native/NativeWriteEncode_Test.cpp`, `Tests/Native/NativeWriteLarge_Test.cpp`, `Tests/Native/NativeWriteReadBack_Test.cpp`, `Tests/Native/NativeWriteMetadata_Test.cpp`
  - approach: replace fixed/input-format setups with an upstream Reformat. New case: a 640x360 input in a 1920x1080 project writes a 640x360 display window, with the data window = the RoD when clipToRoD is on. The knob list has no formatType.
  - verify: at the phase build, `ctest -R NativeWrite` passes.
  - size: M
- [ ] M223.P1.T3 — One Reload on file-reading nodes
  - files: `Engine/KnobFile.h`, `Engine/KnobFile.cpp`, `Gui/KnobGuiFile.cpp`, `Engine/Nodes/IO/NativeRead.cpp`, the OCIO file nodes, `Engine/Nodes/Scene/USDRead.cpp`
  - approach: the new flag defaults to true; `KnobGuiFile` (cpp:121) builds `_reloadButton` only when it is set. NativeRead clears it on File (cpp:159) and Proxy File (cpp:181); the OCIO file nodes and USDRead clear it on the knobs whose node has its own Reload.
  - verify: gtest in `Tests/Native/NativeReadReload_Test.cpp`: both file knobs report no reload button, and the Read has exactly one button knob named `reload`, which still reloads.
  - size: M
- [ ] M223.P1.T4 — GUI checks and docs
  - files: `Tests/gui/m76_write.py`, `Tests/gui/m223_read_write_knobs.py` (new), `Engine/Nodes/IO/docs/NativeWrite.md`, `Engine/Nodes/IO/docs/NativeRead.md`
  - approach: drop "Format Type" from m76's panel order (line 400). New Xvfb script: the Read panel has one reload widget and clicking it shows a file rewritten on disk; the Write panel has no format widgets. Screenshots.
  - verify: both scripts pass under the remote GUI runner.
  - size: M

**Verification gate:** full build + ctest (`NativeWrite*`, `NativeReadReload`); `tools/ci/remote/build.sh debug --tests` green; the GUI bench (diff touches `Engine/Knob*`) compared against the Xvfb baseline and recorded in `## Decisions`; Xvfb screenshots shared with the user before sign-off.

## Decisions

- 2026-10-28: implemented (build pending). Write format = NativeWrite::inputFormat(): input format, else its RoD, else empty (no input). Read-back tests restamp a written file in a foreign format (the Write can no longer write one). Only Read, the OCIO file nodes and USDRead own both a file knob and a Reload button.
- 2026-10-28: elaborated (consultant). Defaults taken from its recommendations, open to the user's override: keep the labelled Reload and hide the file knob's icon (the button reloads File and Proxy together); apply the same flag to the OCIO file nodes and USDRead in T3; keep `clipToRoD`, which is not a format control.
