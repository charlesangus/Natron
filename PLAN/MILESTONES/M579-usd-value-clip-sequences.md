# M579 - USD Value Clip Sequences

A sequence option on USDRead and USDReference that reads a numbered file sequence (`shot.0101.usd`, `sim.####.abc`) as one animated scene by authoring USD value-clip metadata (`UsdClipsAPI`: `clipAssetPaths`, `clipPrimPath`, `clipActive`, `clipTimes`, `clipManifestAssetPath`) on a prim, instead of opening every file into the stage. Time samples then load a frame at a time, so a long cache plays without composing all of it. It follows the Solaris Value Clip LOP and the file-sequence half of the Geometry Clip Sequence LOP. USDRead (`Engine/Nodes/Scene/USDRead.{h,cpp}`) sublayers one file and maps time by seconds (`SceneOps::frameToTimeCode()`); USDReference (`Engine/Nodes/Scene/USDReference.{h,cpp}`) references or pays in a prim of a file onto a prim path. Both gain a Sequence toggle and a group of sequence knobs; the file knob then holds a frame-number pattern. The bundled USD has the Alembic file-format plug-in (`/usr/local/plugin/usd/usdAbc`, `tools/ci/local/Dockerfile`), and both nodes already read `.abc`, so Alembic sequences are In if Phase 1 shows value clips work over `.abc` layers.

## Scope

**Solaris Value Clip LOP controls, each In or Out:**
- Primitive Path: In, as `Clip Prim Path` (the prim that carries the clips). USDRead default: the template file's default prim, else its first root prim. USDReference: its Prim Path.
- Primitive Kind, Parent Primitive Type: Out. Both only matter when the node creates a prim that does not exist; the clip prim comes from the template file or from USDReference's own prim creation.
- Clip Set: In (`Clip Set`, default `default`).
- Clip Primitive Path: In, as `Clip Source Prim` (the prim inside each clip file mapped onto the clip prim; default: the same relative path as the template's).
- Manifest File: In, as a `Manifest` menu: Generate From First File (default), Generate From All Files, From File. A generated manifest is written as an in-memory anonymous layer sublayered by the node's layer, never to disk.
- Start Frame, First Clip Start Frame: In as `Start Frame`/`End Frame` (the first and last file numbers of the sequence used, auto-detected from disk and overridable). Placement in project time is USDRead's existing Frame Offset; USDReference gains a `Frame Offset` in the group.
- Clip Time Scale: Out. Time maps by seconds (the stage's time codes per second over the project frame rate), so a rate change needs no scale knob.
- Loop Until Frame: In (`Loop Until Frame`, off by default). Looping is authored as explicit repeated clips, as USD has no loop.
- Loop First and Last Frames Match: In (`Loop Ends Match`), authoring the discontinuity in `clipTimes` the reference describes.
- Number of Clips: Out. One sequence per node; chain nodes for several, which the stage composes the same.
- Clip File or Sequence: In (the existing file knob, a frame-number pattern in Sequence mode).
- File Duration: In, as `Frames Per File` (default 1): chunked caches with several frames per file.
- File Sequence Count, File Index Start, File Index Step: In as the discovered frame list with `Frame Step` (`Start Frame`/`End Frame` bound it; a step above 1 uses every n-th file).

**Geometry Clip Sequence and Geometry Sequence LOP controls:**
- Sampling Behavior, Start/End/Inc, Subframe Sampling and Shutter group: Out. These bake time samples from a live input; this milestone only reads files.
- Primitive Path, Clip Set, Clip Primitive Path, Start Frame, End Frame, Loop Frames, Reference Topology File: In, as above (the template file is the Reference Topology File: `Template File`, default the first file).
- Handle Missing Files (Error, Warn, Ignore, Allow Missing Files on the Stage): In, as `Missing Frames` (Error, Hold Previous). Error is the default.
- Reload Files: In, the existing Reload button now also re-scans the directory.
- All Clip Files Have Matching Scene Graph Structure: In, as the `Manifest` choice: First File asserts it; All Files does not.
- Track Primitive Existence to Set Visibility, Flatten Clip Files, Remove Unused Data From Flattened Clip Files, Save Clip File Path, Manifest File Save Path, Topology File Save Path, Get Geometry From Second Input, Load Clip File Path: Out. They write clip files to disk or take a generated second input; Natron writes through USDWrite.
- Geometry Sequence's non-USD geometry import controls (Sublayer Style, Adjust Transforms for Input Hierarchy, Reference Primitive, Make Instanceable, Import Group, Import Path Prefix, Packed USD Primitives, Other Primitives, Packed Primitives, NURBS Curves and Surfaces, Kind Authoring, Path Attributes and the rest): Out. They convert Houdini geometry formats, not USD or Alembic.

**In:**
- A `Sequence` toggle and group on USDRead and USDReference: Clip Prim Path, Clip Source Prim, Clip Set, Template File, Start Frame, End Frame, Frame Step, Frames Per File, Missing Frames, Manifest, Time In File, Loop Until Frame, Loop Ends Match, (USDReference) Frame Offset.
- `SceneClips`, a USD-only helper that discovers a sequence's files, reads each file's time range, and authors the clip metadata and manifest on a prim.
- Time In File: Automatic (the file's own start time code, so a one-frame file whose sample sits at its frame number or anywhere else works) or Frame Number (the file's number is the clip time).
- Usual node duties: the sequence's files joined to the node's file stamps so a changed file re-keys it, Reload, docs, tests.

**Out:**
- Writing clip sets with USDWrite, stitching clips to disk or generating topology files on disk: none (the reference nodes' `Flatten`/`Save` options are Out above).
- Converting `.bgeo` and other non-USD geometry sequences: none.
- Reading a sequence of different camera or light files as clips: they are scenes, not value clips; use USDMerge.
- Per-clip time offsets inside one sequence: none.

**Edge cases:**
- A file's time samples: a clip file with only `default` values and no time samples contributes nothing (USD clips read time samples only); the node warns "<file> has no time samples" for the first such file and still authors the sequence.
- Gap in the numbering: `Missing Frames` decides (Error names the first missing number; Hold Previous extends the previous clip over the gap).
- Sequence of one file: authored as a clip set of one clip; a warning "one file matched".
- No file matches the pattern: node error naming the pattern.
- Pattern is not a frame-number pattern while Sequence is on: node error.
- Files with different time codes per second: the template file's governs the stage; clip files that differ get a warning naming the first.
- Opening each file to read its time range runs off the UI thread; a sequence of thousands of files shows the node as evaluating and cancels fast when the user edits. Manifest From All Files opens every file and says so in its tooltip; the default opens only the first and the last.
- The viewer never loads payloads: clip metadata is authored on the prim; USDReference in Payload mode keeps the payload unloaded and the clips cost nothing until the payload loads.
- Alembic: if value clips do not work over `.abc` layers, Phase 1 records it and the Sequence toggle refuses `.abc` with a clear error.

**Assumptions:**
- Unconfirmed: `UsdClipsAPI` clips and `GenerateClipManifestFromLayers()` work with layers from the `usdAbc` format; M579.P1.T1 settles it before any node work.
- Unconfirmed: a stage composing a thousand clip files holds only the layers it needs: clip layers open on first use in USD, which M579.P1.T1 measures.
- Unconfirmed: sequences are named by Natron's frame-number conventions (`####`, `%04d`); the existing `SequenceParsing` handles them, as `NativeRead` does.

## Phase 579.1: Probe and the clip helper

- [ ] M579.P1.T1 — Probe clips over USD and Alembic files
  - files: `Tests/Scene/SceneClipsProbe_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: generate N one-frame `.usda` files in a temp directory (a sphere radius sampled at its frame number) and N one-frame `.abc` files if the build can write Alembic (otherwise a small committed `.abc` fixture of a few KB, mindful of the LFS quota), author clips by hand with `UsdClipsAPI` on a prim of an in-memory stage, and read the radius at several frames. Also record how many clip layers are open after reading one frame of 200 files, and whether a clip file with only default values yields a value.
  - verify: gtest — the radius at frame k equals file k's value for both formats (an Alembic failure is recorded as a finding in `## Decisions`, and the test then asserts the failure so the doc stays true); only the layers needed are open.
  - size: M

- [ ] M579.P1.T2 — Sequence discovery
  - files: `Engine/Nodes/Scene/SceneClips.{h,cpp}`, `Tests/Scene/SceneClips_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: `SceneClips::discover(pattern, start, end, step, missingFrames, &files, &error)` lists the existing frames from the pattern through the shared sequence parsing and returns the file list with frame numbers, the first and last, and the gaps handled per `Missing Frames`; absolute paths only. No USD opened here.
  - verify: gtest on a temp directory — contiguous frames, a gap with each Missing Frames mode, `%04d` and `####` patterns, a range clipped by Start/End, step 2, no match, a single file.
  - size: M

- [ ] M579.P1.T3 — Authoring the clip metadata and manifest
  - files: `Engine/Nodes/Scene/SceneClips.{h,cpp}`, `Tests/Scene/SceneClips_Test.cpp`
  - approach: `SceneClips::author(prim/over in a layer, clip set, file list, framesPerFile, timeInFile, loop options, template, manifest mode, stageTimeOfFrame, &error)`: reads each file's start time code (Automatic) or uses its number (Frame Number), writes `clipAssetPaths`, `clipPrimPath`, `clipActive` and `clipTimes` (pairs of stage time and clip time) as `UsdClipsAPI` metadata, sets `clipManifestAssetPath` to an in-memory manifest built from the first file, all files (opened one by one off the main thread, polling cancel), or a given file, and applies Loop Until Frame and Loop Ends Match by repeating the pairs as the reference describes. Stage time of the file numbered n is the project frame n's timecode (`SceneOps::frameToTimeCode()`), so USDRead's Frame Offset keeps working.
  - verify: gtest — clip times for a plain sequence, frames-per-file 5, a file whose sample is not at its number, looping 3 times with and without the ends-match discontinuity (the `(100,100)`, `(100,1)`, `(199,100)` pair shape of the reference), the manifest holds the attributes of the first file, cancel stops it.
  - size: L

## Phase 579.2: The nodes

- [ ] M579.P2.T1 — USDRead sequence mode
  - files: `Engine/Nodes/Scene/USDRead.{h,cpp}`
  - approach: `Sequence` bool and the group of Scope knobs (labels equal the readable script names: `sequence`, `clipPrimPath`, `clipSourcePrim`, `clipSet`, `templateFile`, `startFrame`, `endFrame`, `frameStep`, `framesPerFile`, `missingFrames`, `manifest`, `timeInFile`, `loopUntilFrame`, `loopEndsMatch`). In Sequence mode the layer sublayers the template file (default the first frame file) and `SceneClips::author` writes the clips on an over of the Clip Prim Path in the node's layer. The node's content hash and file-stamp set include every sequence file (stat only, off the UI thread), so a changed or added file re-keys it; Reload re-scans the directory. Up axis, units and Initial Draw work from the template file as before.
  - verify: gtest — a 10-file sequence read at frames 1 to 10 gives each file's value; Frame Offset shifts the mapping; adding a file and pressing Reload extends the sequence; Sequence off is byte for byte the old behaviour.
  - size: L

- [ ] M579.P2.T2 — USDReference sequence mode
  - files: `Engine/Nodes/Scene/USDReference.{h,cpp}`
  - approach: the same group on USDReference plus `Frame Offset` (`frameOffset`); the reference or payload arc targets the template file, and the clips are authored on the Prim Path prim; both Reference and Payload modes work. File-stamp keying as in the USDRead task, through the shared `SceneClips` helper.
  - verify: gtest — a sequence referenced under `/World/Cache` plays across 10 frames in Reference and in Payload mode (payload loaded); Payload mode with the payload unloaded loads nothing and opens no clip file.
  - size: M

- [ ] M579.P2.T3 — Docs and tooltips
  - files: `Engine/Nodes/Scene/docs/USDRead.md`, `Engine/Nodes/Scene/docs/USDReference.md` (new, in the style of `USDRead.md` if the node has none), the knob tooltips
  - approach: a Sequence section for each: the pattern forms, what is authored, every knob, time mapping, missing frames, manifest modes and their cost, the Alembic result of Phase 1, looping; the file tooltips mention `.abc` as before. No reference-app names.
  - verify: docs name every sequence knob; the tooltips match.
  - size: M

- [ ] M579.P2.T4 — Params-to-knob audit
  - files: this milestone file (a checked table)
  - approach: walk the Value Clip and Geometry Clip Sequence control lists in Scope against the built knobs on both nodes; each In control has a knob and a doc line.
  - verify: the table in the PR body maps every control.
  - size: S

## Phase 579.3: GUI check

- [ ] M579.P3.T1 — Playback and Reload under Xvfb
  - files: `Tests/gui/usd_value_clips.py`
  - approach: write a 20-file one-frame sequence (and an `.abc` one if Phase 1 allows), USDRead with Sequence on in the viewer, step the frame and assert the shape changes with the frame; add a file, press Reload and assert the end frame grows; edit `Frames Per File`/`Missing Frames` knobs and assert the viewer refreshes (a knob-driven refresh, so ctest alone is not enough). Screenshots go to the user.
  - verify: the script passes under Xvfb at 1920x1080; the user OKs the screenshots.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SceneClipsProbe_Test`, `SceneClips_Test` and the new USDRead and USDReference sequence tests pass, and the existing USDRead and USDReference tests are unchanged; `Tests/gui/usd_value_clips.py` passes under Xvfb at 1920x1080; the user OKs the screenshots.

## Decisions

- 2026-11-24 — **Milestone now, recommended shape (user):** a sequence option on USDRead and USDReference that authors value-clip metadata for a numbered USD file sequence, and for Alembic if the bundled USD has the Alembic plug-in. It does: `usdAbc` is installed; whether clips work over it is Phase 1's finding.
- 2026-11-24 — The `Missing Frames` menu is Error and Hold Previous only, and Error is the default (planner).
