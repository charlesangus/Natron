# M16 - Project Format Redesign

Full title: Project file format redesign (.ntp successor)

Replace the boost::serialization XML `.ntp` with a `.ntpz` bundle: a stored-uncompressed zip
holding:
- `project.yaml`: the graph, with sparse knobs, explicit sibling-relative connections and
  compact curves;
- `gui.yaml`: workspace, viewers and node positions;
- `manifest.yaml`;
- 64-byte-aligned binary blob members for heavy per-node data: roto strokes, animated bezier
  shapes, tracker curves, and any curve over a key-count threshold.

**Clean break** (user, 2026-10-10): there is no import of old XML `.ntp`, `.nps` or `.nl` files.
Opening one gives a clear "old XML format, not supported" message. The boost XML reader and
writer and the `Serialization/BoostXml/` adapters are deleted. boost.serialization stays only
for the disk-cache TOC. Fixtures and bench comps are regenerated in the new format by their
scripts. Design: `DECISIONS/2026-09-05-project-format-bundle-design.md`, amended below for
connections. Depends on **M93 - Serialization Boundary**.

Scouting facts this plan relies on (2026-10-10):
- Save: `Project::saveProjectInternal` (`Engine/Project.cpp:714`) writes its temp file to the
  *system temp dir*, and a cross-device rename falls back to `QFile::copy`. Saves **are not
  atomic today**.
- Load: `loadProjectInternal` (`:435`) → `NodeCollectionSerialization::restoreFromSerialization`
  (`Engine/NodeGroupSerialization.cpp:84`). Wiring is already a second pass keyed by input
  label (`:374-393`), but a failed connect is only a `qDebug`.
- A headless (NatronRenderer) save writes no GUI state.
- Other XML users after M93: `.nps` presets (`Gui/NodeSettingsPanel.cpp:211-300`), the clipboard
  (`Gui/NodeGraph40.cpp`), `.nl` layouts (`Gui/Gui10.cpp:316`, `Gui/Gui15.cpp:73,106`), and knob
  undo (`Gui/KnobUndoCommand.cpp:94,106`).
- Roto and tracker use the `RotoContext`/`TrackerContext` model. A bezier control point carries
  12 curves; a stroke has 3 parallel sample curves; a track is a list of knob curves with one
  key per frame.
- The layer registry serializes at project level. Typed edges and per-frame metadata are
  derived at runtime, not stored. Native nodes use the generic knob path, and plugin
  major/minor round-trips.
- Knob saving is already sparse (`hasModificationsForSerialization`), with a channel-set
  always-save rule.
- Version-gated legacy migration code that the clean break makes dead:
  - `Node.cpp:1636` (channel set below v17);
  - `Node.cpp:1448-1575` (legacy colour rows, `resetLegacyColorLayerKnobs`) and
    `KnobChannelSet::legacyColorDefaultRows` (`Engine/KnobChannelSet.cpp:113`);
  - `Node.cpp:1759` (knob layers below v14);
  - `ProjectPrivate.cpp:273`;
  - `Gui/ProjectGui.cpp:575,588`;
  - the RC2/RC3 scan in `Project.cpp:456-475`;
  - `_oldInputs`.
- `.ntp` string sites:
  - `Global/Macros.h:95` (`NATRON_PROJECT_FILE_EXT`);
  - `Engine/Project.cpp:960-970` (autosave search), `:1254` and `:1573` (Python help text);
  - `Engine/AppInstance.cpp:619` and `:1751` (`RENDER_SAVE.ntp`);
  - `Engine/CLArgs.cpp:375-380` and `:1019`;
  - `Engine/PrecompNode.cpp:174`;
  - `Gui/GuiAppInstance.cpp:398`, `Gui/GuiApplicationManager10.cpp:261`;
  - `Gui/Resources/Mime/x-natron.xml:7`, `App/NatronInfo.plist.in:30`;
  - `tools/genStaticDocs.sh:66-68`, `tools/ci/smoke_test.py` (5), `tools/bench/graph_bench.py`,
    `tools/bench/comps/run_comps.sh`;
  - about 14 test files that save to `*.ntp` names.
- Fixture generators: `tools/ci/regen_read_time_offset.py` (`Tests/fixtures/read-time-offset.ntp`)
  and `tools/bench/comps/build_comps.sh` with `build_comps.py`, which saves through
  `app.saveProject`. `Tests/fixtures/channel-set-legacy-defaults.ntp` and
  `m65-legacy-color.ntp` are hand-written *legacy-version* fixtures. They exist only to test
  the migration code above.
- Dependencies: no yaml-cpp, zip or FlatBuffers in the tree. libzip is absent from
  `aswf/ci-vfxall:2027-clang21.1`. Whether yaml-cpp is in the image is unverified, because
  there is no local image on this host.
- `SequenceFileDialog` matches extensions on the last dot (`Gui/SequenceFileDialog.cpp:733,1719,1868`).

## Phase 16.1: Dependencies and container primitives

- [x] M16.P1.T1 — Probe the CI image for yaml-cpp and zlib
  - files: none (findings go in `## Decisions`)
  - approach: in `tools/ci/local/devshell.sh`, run `find / \( -path '*yaml-cpp/yaml.h' -o -name 'yaml-cpp*Config.cmake' -o -name 'libyaml-cpp*' -o -name zlib.h -o -name 'libz.so*' \) 2>/dev/null`. Record each path, its version, and whether it is a public install with a CMake config.
  - verify: a Decisions entry lists what was found, or "none".
  - size: S

- [x] M16.P1.T2 — Add yaml-cpp 0.8 to the build
  - files: `CMakeLists.txt`, new `libs/yaml-cpp/` (only if P1.T1 found no usable install), `libs/CMakeLists.txt`, `Serialization/CMakeLists.txt` (add `GLOB_RECURSE` for the new `Bundle/` and `Yaml/` subdirs; it globs only top-level `*.cpp` plus `BoostXml/` today), `tools/ci/check_serialization_layering.sh` (if it needs to allow the new subdirs), `tools/license/components/LICENSE-yaml-cpp.txt` (and its README line)
  - approach: try `find_package(yaml-cpp 0.8 CONFIG)` first. Otherwise vendor the pinned 0.8.0 release as a static target with tests, tools and contrib OFF. Link it PRIVATE to `NatronSerialization` only, so no Engine or Gui header includes yaml-cpp. Exclude `libs/yaml-cpp` from the format and lint gates as other `libs/` entries are.
  - verify: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug` configures and links, and `grep -rn yaml-cpp Engine Gui` is empty.
  - size: M

- [x] M16.P1.T3 — Stored-only zip writer and reader with 64-byte member alignment
  - files: new `Serialization/Bundle/ZipStore.h/.cpp`, new `Tests/Serialization/ZipStore_Test.cpp`
  - approach: write it in-house; no library can align member data, and libzip is not in the image.
    - Writer: the first member is `mimetype` (`application/vnd.natron.project+zip`, stored, no extra field). Every member is method 0, with the local-header extra field padded (id `0x1986`, as usdz does) so its data starts on a 64-byte boundary. CRC32 comes from zlib if P1.T1 found it, otherwise a table. A fixed 1980-01-01 DOS timestamp makes the output deterministic. Zip64 and members over 4 GiB are refused with a clear error.
    - Reader: parse the central directory with every offset bounds-checked. Reject compressed, encrypted or zip64 members, duplicate names, and absolute or `..` names. Map the file read-only (`QFile::map`) and return a `std::span<const std::byte>` per member. CRC checking is on demand (`verify()`). Stored but unaligned members, such as a bundle hand-repacked with Python `zipfile` `ZIP_STORED`, are accepted.
  - verify: compiled at P1.T5.
    - gtest round-trips 0, 1 and 50 members of sizes 0, 1, 63, 64, 65 and 1 MiB, and every data offset is a multiple of 64.
    - `python3 -c 'zipfile.ZipFile(p).testzip()'` (run via `QProcess` when python3 exists, otherwise a logged skip line) and `unzip -t` accept the output.
    - Two saves of the same input are byte-identical.
    - Truncated, bit-flipped and traversal-named archives throw and do not crash.
  - size: M

- [x] M16.P1.T4 — Blob table codec (flat typed columns)
  - files: new `Serialization/Bundle/BlobTable.h/.cpp`, new `Tests/Serialization/BlobTable_Test.cpp`
  - approach: a 64-byte header (magic `NTBL`, u16 version, u16 column count, u64 rows, u64 column-directory offset), followed by columns that are each 64-byte aligned, named, typed (`f64|f32|i32|u8`), contiguous and little-endian (SoA). The YAML side keeps a descriptor `{blob: blobs/<path>.bin, rows: N, columns: [time:f64, x:f64, ...]}`. The reader matches columns **by name**: an expected column that is missing gets its default, and an unknown column is ignored. That is the schema-evolution story. `BlobTableView` gives typed spans with no copy when aligned, and copies otherwise. A static_assert enforces little-endian hosts.
  - verify: compiled at P1.T5. gtest round-trips every dtype, a 0-row table and a 1e6-row table; checks the missing and unknown column rules; and checks that a corrupt count or offset is rejected without reading out of bounds (ASan in the debug build if available).
  - size: M

- [x] M16.P1.T5 — Phase 16.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `ctest -R 'ZipStore|BlobTable'`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 16.2: YAML encoding of the data model

- [x] M16.P2.T1 — Scalar and curve YAML encoding
  - files: new `Serialization/Yaml/YamlScalars.h/.cpp`, new `Serialization/Yaml/CurveYaml.cpp`, new `Tests/Serialization/CurveYaml_Test.cpp`
  - approach: port RB-3's compact curve grammar (`git show origin/master:Serialization/CurveSerialization.cpp`). It is a flow sequence: an interpolation token only when it changes, then `time value`, then `rightDeriv` for free or broken keys, then `leftDeriv` for broken keys. Rewrite the decoder as an explicit state machine. Doubles are emitted as `std::to_chars` shortest-round-trip strings and parsed with `std::from_chars`; `.inf`/`.nan` use the YAML forms.
  - verify: compiled at P2.T7. Every interpolation type, mixed sequences, broken and free tangents, ±0, denormals, 1e300 and 17-digit values all round-trip bit-exactly. A 3-key linear curve's text is pinned as a literal. Malformed input throws a descriptive error.
  - size: M

- [x] M16.P2.T2 — Knob YAML encoding (sparse)
  - files: new `Serialization/Yaml/KnobYaml.cpp`, new `Tests/Serialization/KnobYaml_Test.cpp`
  - approach: knobs form a map keyed by script name, in data-model order, holding only knobs that `toSerialization` emitted. Value forms:
    - a scalar for a 1-dim knob, a flow sequence for N dims;
    - `{curve: [...]}` for animation;
    - `{expr: "...", hasRet: bool}` for an expression;
    - `{link: "<fully.qualified.Node>.knob[.dim]"}` for a link;
    - the choice option ID, plus `label` only when it differs;
    - layer-select, channel-set and shuffle-map values in their existing string encodings;
    - a `user:` block for user knobs;
    - a list of curves for parametric knobs;
    - `{curve: [t, "s", ...]}` for string animation.

    Unknown keys on decode are ignored and collected as warnings.
  - verify: compiled at P2.T7. A table over every knob type in M93's conversion list, in static, animated, expression, linked and user variants, checks encode→decode equality field by field. A default-valued knob is absent from the output.
  - size: L

- [x] M16.P2.T3 — Node and project YAML encoding
  - files: new `Serialization/Yaml/NodeYaml.cpp`, new `Serialization/Yaml/ProjectYaml.cpp`, new `Tests/Serialization/ProjectYaml_Test.cpp`
  - approach:
    - A node is `{plugin: id, version: [major, minor], label?, python: {module, version}?, knobs, inputs: {<input label>: <sibling script name>}, pages?, userPages?, children: {...}}`.
    - Group membership comes from nesting: a group's `children:` map holds its nodes. Connections are on the consumer, keyed by input label, and name a **sibling** (user decision). Expressions and links stay fully qualified.
    - The node maps are keyed by script name and emitted sorted, so order in the file is cosmetic and duplicates are detectable.
    - `project.yaml` holds `format: natron-project`, `formatVersion: 1`, the app version, sparse project knobs, additional formats, the layer registry sorted by layer ID, and the timeline.
    - Save date and author go in `manifest.yaml`, so re-saving an unchanged project leaves `project.yaml` byte-identical.
    - Dropped fields: `KnobsAge`, `CacheID`, multi-instance, `oldInputs`, and the master node (M92 defines its own linked-node fields).
    - A newer `formatVersion` fails with "saved by a newer Natron".
  - verify: compiled at P2.T7. A data-model project built in code, with a nested group, connections and a layer registry: encode→decode is equal, re-encoding is byte-identical, a hand-shuffled file decodes equal, and a duplicate script name or `formatVersion: 99` gives the expected error.
  - size: M

- [x] M16.P2.T4 — GUI YAML encoding
  - files: new `Serialization/Yaml/ProjectGuiYaml.cpp`, new `Tests/Serialization/ProjectGuiYaml_Test.cpp`
  - approach: `gui.yaml` holds node UI keyed by fully-qualified name (position, size, colour, overlay colour), backdrops, viewer data, histograms and the pane layout. The layout encoder is a separate entry point because `.nl` files reuse it (P4.T3).
  - verify: compiled at P2.T7. A fully populated `ProjectGuiSerialization` round-trips field by field.
  - size: M

- [x] M16.P2.T5 — Roto encoding with blob members
  - files: new `Serialization/Yaml/RotoYaml.cpp`, new `Tests/Serialization/RotoYaml_Test.cpp`
  - approach: the item tree and item knobs stay in YAML.
    - **Strokes:** one table with columns `x, y, pressure, time` (the three parallel curves merged). This assumes they share key times; if not, fall back to inline curves.
    - **Animated beziers:** one table per bezier with rows (key × cp) and columns `time, cp, x, y, lx, ly, rx, ry`, a `feather` set of the same columns, and a u8 `interp`. This relies on the bezier's shape keyframes being shared by all CPs. If any CP breaks that, fall back losslessly to per-curve encoding.
    - Static beziers are an inline flow list.
    - A table under the threshold (64 rows, a constant) is written inline as a flow sequence with the same columns.
    - Paths are `blobs/<fq node>/<item>.<kind>.bin`. The encoder writes through an abstract `BlobSink` (bundle or inline base64), shared with presets and the clipboard.
  - verify: compiled at P2.T7. A 2000-sample stroke, a 20-CP feathered bezier with a broken tangent over 100 keys, a static bezier, and a bezier with an odd CP key set all round-trip bit-exactly. Blobs appear only above the threshold, and no per-sample text is in the YAML.
  - size: L

- [x] M16.P2.T6 — Tracker encoding with blob members
  - files: new `Serialization/Yaml/TrackerYaml.cpp`, new `Tests/Serialization/TrackerYaml_Test.cpp`
  - approach: track flags, names and user keys go inline. Knob curves at or above the threshold go into **one table per track**: rows are the union of key times; columns are `time`, `<knob>.<dim>` values, a u8 `<knob>.<dim>.present` mask, and `interp` columns only when not all-linear. Curves with free or broken derivatives fall back to inline curves.
  - verify: compiled at P2.T7. 4 tracks × 1000 keys with sparse curves and one broken tangent round-trip bit-exactly as four blob members.
  - size: M

- [x] M16.P2.T7 — Phase 16.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `ctest -R Yaml`, then the full ctest.
  - verify: green. The build is required.
  - size: S

## Phase 16.3: `.ntpz` save and load, clean break

- [x] M16.P3.T1 — Project bundle writer and reader
  - files: new `Serialization/Bundle/ProjectBundle.h/.cpp`, new `Tests/Serialization/ProjectBundle_Test.cpp`
  - approach: members are written in a fixed order: `mimetype`, `manifest.yaml` (bundle version, app version and commit, save date, author, member list with sizes and CRCs), `project.yaml`, `gui.yaml` (optional), then the `blobs/**` members sorted. The reader validates `mimetype`. A referenced blob that is missing gives a warning and empty item data. `detectProjectFileKind(path)` returns bundle, legacy XML (`<?xml`/`<!DOCTYPE boost_serialization` within the first 64 bytes), or unknown.
  - verify: compiled at P3.T8. Write and read roto and tracker blobs; a deleted blob member gives a warning; two saves of the same data differ only in `manifest.yaml`; `detectProjectFileKind` classifies samples of each kind.
  - size: M

- [x] M16.P3.T2 — Save `.ntpz` atomically
  - files: `Engine/Project.cpp` (`saveProjectInternal`), `Engine/ProjectPrivate.cpp`, `Engine/AppInstance.h/.cpp`, `Gui/GuiAppInstance.cpp`
  - approach:
    - Build `ProjectSerialization` and `ProjectGuiSerialization`, then write them with `ProjectBundleWriter` into a `QSaveFile` **in the target directory**. That gives fsync and rename and removes the temp-dir write and copy fallback.
    - Keep backup rotation and autosave naming. A headless save writes back the `gui.yaml` it loaded as an opaque member, so positions survive NatronRenderer re-saves.
    - Keep the XML writer callable only as the internal `Project::saveLegacyXmlForBenchmark()` (no UI, no Python) for P6. P7.T1 deletes it.
  - verify: compiled at P3.T8. There, the project round-trip gtests pass through the bundle path, and a writer made to throw after `project.yaml` leaves the previous file intact.
  - size: M

- [x] M16.P3.T3 — Load `.ntpz` only; refuse old XML clearly
  - files: `Engine/Project.cpp` (`loadProjectInternal`), `Gui/NodeSettingsPanel.cpp` (the refusal message only), `Serialization/BoostXml/*` (delete every load half; the save halves stay for P6)
  - approach: by `detectProjectFileKind`:
    - a bundle goes through `ProjectBundleReader` → `restoreFromSerialization`;
    - legacy XML throws "This project uses the old XML .ntp format, which this version of Natron cannot open (no compatibility, by design)";
    - anything else gives "Unrecognized or damaged project file".

    The error reaches the GUI dialog and the CLI exit code through today's paths. Delete the RC2/RC3 scan (`:456-475`). PrecompNode and autosave recovery go through the same function.
  - verify: compiled at P3.T8; the tests are in P5.T2.
  - size: M

- [x] M16.P3.T4 — Dangling references and order independence at restore
  - files: `Engine/NodeGroupSerialization.cpp`, `Engine/Project.h/.cpp` (a load-warnings list)
  - approach: in the wiring pass (`:374-393`), an unknown input label or a missing sibling target becomes a load warning ("Blur1.Source → Missing1: node not found; input left disconnected"). Warnings go to `appPTR->writeToErrorLog_mt_safe` and are collected on Project. The GUI shows one summary dialog after load; the CLI prints to stderr. Missing link or expression targets get the same treatment. Keep the order as: create all nodes, then wire all inputs, then restore links.
  - verify: compiled at P3.T8; the tests are in P5.T2.
  - size: M

- [x] M16.P3.T5 — Delete legacy-format tests and fixtures
  - files: `Tests/fixtures/golden-ntp/` and `Tests/Serialization/GoldenXml_Test.cpp` (M93's), plus the other XML-only tests if they depend on the XML format: `ProjectXmlEquivalence_Test`, `KnobXmlEquivalence_Test`, `RotoXmlEquivalence_Test`, `ProjectXmlHelpers.h`, the `MissingPlugin*` XML tests, `Tests/fixtures/native-golden`. (The two legacy `.ntp` fixtures and `LegacyColorProjectTest` were already removed by M69.)
  - approach: delete only the assertions about *migrating* old versions. Where a legacy test also pins current behaviour, rebuild that graph in code and keep the assertion. For example, `ResavingWritesRgbaNeverTheStorageID` becomes a check that a fresh Blur's saved `project.yaml` never contains the storage ID. List every deleted test name in the commit message.
  - verify: compiled at P3.T8. `grep -rn "legacy" Tests/*.cpp` shows nothing that loads an XML fixture.
  - size: M

- [x] M16.P3.T6 — Delete version-gated legacy migration code
  - files: `Engine/Project.cpp` (the RC2/RC3 scan, `setReadingRC2OrRC3Archive`), `Engine/NodeGroupSerialization.cpp` (`oldInputs`, ~`:363`), `Serialization/*Serialization.h` (remove the `version` fields and `oldInputs`). Most Node.cpp/ProjectPrivate/ProjectGui legacy gates were already removed; re-scout for any remaining version gates rather than trusting line numbers
  - approach: the clean break makes these unreachable, because a `.ntpz` is always current. Remove the code and its constants (the `*_SERIALIZATION_*` version macros go with `BoostXml` in P7.T1).
  - verify: compiled at P3.T8, where the full ctest is green.
  - size: M

- [x] M16.P3.T7 — `.ntpz` across Engine, CLI and desktop integration
  - files:
    - `Global/Macros.h` (`NATRON_PROJECT_FILE_EXT "ntpz"`)
    - `Engine/Project.cpp` (autosave search `:960-970`, help text `:1254,1573`)
    - `Engine/AppInstance.cpp` (`:619` message, `:1751` `RENDER_SAVE.ntpz`)
    - `Engine/CLArgs.cpp` (`:375-380` usage, `:1019`, which should accept `.ntpz` and give the old-format message for `.ntp`)
    - `Engine/PrecompNode.cpp` (`:174` help and its file-knob filter)
    - `Gui/GuiAppInstance.cpp:398`, `Gui/GuiApplicationManager10.cpp:261`
    - `Gui/Resources/Mime/x-natron.xml` (glob `*.ntpz`, plus magic on `mimetype` at offset 30)
    - `Gui/Resources/Applications/fr.natron.Natron.desktop`
    - `App/NatronInfo.plist.in` (only if still installed; otherwise leave it)
  - approach:
    - Open and save dialogs, drag-drop (`Gui/Gui50.cpp`) and recent files (`Gui/Gui20.cpp`, `Gui/GuiPrivate.cpp`) follow the macro. Check that each does.
    - A recent-files entry ending in `.ntp` stays listed, and opening it shows the old-format message.
    - Autosaves become `<name>.ntpz.autosave[.hash]`.
    - `.ntp` stays registered in the MIME file with no Natron handler, so the desktop does not offer to open old files.
  - verify: compiled at P3.T8. `xmllint --noout` on the MIME file passes. `grep -rn '"ntp"\|\.ntp\b' Engine Gui Global App` shows only the old-format refusal paths.
  - size: M

- [x] M16.P3.T8 — Rename test and tool paths, build, regenerate fixtures and comps, full ctest
  - files: every test file found by `grep -rl '\.ntp"' Tests` (about 30, including `Tests/Native/`), `tools/ci/smoke_test.py`, `tools/ci/regen_read_time_offset.py`, `tools/bench/comps/build_comps.{sh,py}`, `tools/bench/comps/run_comps.sh`, `tools/bench/graph_bench.py`, `tools/genStaticDocs.sh`, `Tests/fixtures/read-time-offset.ntpz` (regenerated), `tools/bench/comps/*.ntpz` (regenerated), and the old `*.ntp` files (deleted)
  - approach:
    1. Replace `.ntp"` with `.ntpz"` in the listed test strings and tool paths. In `genStaticDocs.sh`, check that an empty dummy file is still accepted by `--export-docs`; if not, make it write a real empty project first.
    2. Build: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug` (and `release`, because `build_comps.sh` uses `build/release`).
    3. Regenerate with the existing scripts: `READ_TIME_OFFSET_FIXTURE_PATH=$PWD/Tests/fixtures/read-time-offset.ntpz ... NatronRenderer tools/ci/regen_read_time_offset.py`, then `tools/bench/comps/build_comps.sh`. Then `git rm` the old `.ntp` files.
    4. Run `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green, and the regenerated `.ntpz` files are bundles (`unzip -l` lists `mimetype` first). Tests that still assert on saved XML text fail here *by design*. List them; P5.T1 fixes them. The build is required.
  - size: M

## Phase 16.4: Presets, clipboard, layouts and undo

- [x] M16.P4.T1 — `.nps` presets as a plain YAML document
  - files: `Gui/NodeSettingsPanel.cpp` (`:211-300`), new `Serialization/Yaml/PresetYaml.cpp`, new `Tests/Serialization/PresetYaml_Test.cpp`
  - approach: keep the `.nps` extension (see Decisions). The file is one YAML document: `format: natron-preset`, `formatVersion: 1`, then a list of nodes in the `project.yaml` node schema, with blobs inline as YAML `!!binary` tables through `BlobSink`. An old XML `.nps` (detected by `<?xml` at the start) gets the old-format message.
  - verify: compiled at P5.T5. A gtest round-trips a preset holding a RotoPaint with a heavy stroke. UAT covers export and import in the GUI.
  - size: M

- [x] M16.P4.T2 — Copy and paste as YAML text
  - files: `Gui/NodeGraph40.cpp` (`:130-200`), `Gui/NodeClipBoard.h`, new `Serialization/Yaml/ClipboardYaml.cpp`
  - approach: copy puts a `format: natron-clipboard` document (nodes with inline blobs, plus their positions) on the system clipboard as `text/plain` and `application/vnd.natron.nodes+yaml`. Paste accepts that document; other text is ignored, as today.
  - verify: compiled at P5.T5. UAT: copy a Blur→Grade pair, edit the Blur size in a text editor, paste it back, and the edit and the connection survive.
  - size: M

- [x] M16.P4.T3 — `.nl` layouts as YAML, and knob undo without an archive
  - files: `Gui/Gui10.cpp`, `Gui/Gui15.cpp`
  - approach: `.nl` save and load use P2.T4's layout encoder, and an XML `.nl` gets the old-format message. (`KnobUndoCommand` already keeps an in-memory snapshot; nothing to do there.)
  - verify: compiled at P5.T5. UAT saves and loads a layout, then undoes and redoes a knob edit.
  - size: M

## Phase 16.5: Tests

- [x] M16.P5.T1 — Migrate tests that inspect saved XML
  - files: the tests P3.T8 listed (for example `Tests/DefaultChannelSet_Test.cpp:547`, `Tests/ProjectSerialization_Test.cpp` `SavedProjectContainsNoUserComponents`), new `Tests/Serialization/BundleTestUtils.h`
  - approach: add `readBundleMember(path, "project.yaml")`. Rewrite XML-text assertions against the YAML text or the decoded data, keeping what each assertion means.
  - verify: compiled at P5.T5. Each migrated test passes. Spot-check one by reverting its guarded behaviour: it must fail.
  - size: M

- [x] M16.P5.T2 — Dangling-reference, order, robustness and old-format refusal tests
  - files: new `Tests/Serialization/BundleLoad_Test.cpp`, new `Tests/Serialization/ProjectEquivalence.h/.cpp`, new `Tests/fixtures/bundle/*.yaml` (the source YAML, packed with `ZipStore` at run time, so no binary fixtures are committed), new `Tests/fixtures/old-format.ntp` (a 3-line XML stub)
  - approach: `ProjectEquivalence` compares live projects: node tree, plugin ID and version, inputs, every persistent knob value (bit-exact curves), expressions and links, the layer registry, formats and the timeline. Cases:
    1. three node orders load to equivalent projects;
    2. a missing input target is disconnected with exactly one named warning;
    3. an unknown input label warns;
    4. a missing expression or link target warns and the project still loads;
    5. unknown keys warn and are ignored;
    6. `formatVersion: 99` is refused with the "newer Natron" message;
    7. a truncated or corrupt central directory gives "damaged project file";
    8. a missing blob gives a warning and an empty stroke;
    9. the old-format stub, an XML `.nps` and an XML `.nl` each give the old-format message without crashing;
    10. a garbage file gives "unrecognized".
  - verify: compiled at P5.T5; every case passes.
  - size: M

- [x] M16.P5.T3 — End-to-end roto and tracker round-trip
  - files: new `Tests/Serialization/RotoTrackerBundle_Test.cpp`
  - approach: build a live RotoPaint (strokes with pressure, feathered animated beziers with a broken tangent, nested layers) and a Tracker (500-frame tracks, one disabled, user keys) as `Tests/RotoLayer_Test.cpp` and `Tests/TrackerLayer_Test.cpp` do. Save, reset and load, then check `ProjectEquivalence`, the item and track structure, and a pixel-exact roto render at two frames.
  - verify: compiled at P5.T5; it passes, and the bundle contains stroke, bezier and track blob members.
  - size: M

- [x] M16.P5.T4 — Python API round-trip
  - files: `tools/ci/smoke_test.py` (one new check), `Engine/PyAppInstance.h` (doc comments only)
  - approach: under NatronRenderer, `app.saveProject("x.ntpz")` then `app.loadProject` on a small graph; `app.loadProject` on `Tests/fixtures/old-format.ntp` raises with the old-format message. Update the doc comments of `saveProject`, `saveProjectAs` and `loadProject` (`:310-313`) to name `.ntpz`.
  - verify: run at P5.T5 via `tools/ci/local/test.sh smoke debug`.
  - size: M

- [x] M16.P5.T5 — Phase 16.5 build and full ctest
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

## Phase 16.6: One-off benchmark (both writers still exist)

- [ ] M16.P6.T1 — Heavy roto/tracker benchmark harness
  - files: new `tools/bench/project_io_bench.py`, new `tools/bench/make_heavy_project.py` (both deleted in P7.T1; they stay in git history)
  - approach: `make_heavy_project.py` (under NatronRenderer) builds a seeded project through `PyRoto`/`PyTracker`: 200 strokes × 500 samples, 30 beziers × 24 CPs × 200 keys with feather, 16 tracks × 2000 frames, plus the `keying_grade` graph. `project_io_bench.py` times `saveLegacyXmlForBenchmark` against the `.ntpz` save, and autosave, as a median of 7 after warm-up. It records file sizes and the `.ntpz` load time; XML load is no longer possible. Results go to `build/bench/results-project-io.jsonl` through `lib.sh`'s helpers.
  - verify: one run against a `fast` build prints the table and exits 0.
  - size: M

- [ ] M16.P6.T2 — Run the comparison and record the numbers
  - files: `tools/bench/BASELINE.md` (one section; the numbers outlive the scripts)
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh release`, then three runs of P6.T1. Record the size ratio, save and autosave medians, and the `.ntpz` load time.
  - verify: `.ntpz` is at least 10x smaller, and its save and autosave medians are faster. If either misses, stop and report before P7. The release build is required.
  - size: S

## Phase 16.7: Cleanup, docs and UAT

- [ ] M16.P7.T1 — Delete the boost XML format entirely
  - files: `Serialization/BoostXml/` (removed), `Engine/Project.cpp` (`saveLegacyXmlForBenchmark`), `tools/bench/project_io_bench.py` and `tools/bench/make_heavy_project.py` (removed), `Serialization/CMakeLists.txt`, `CMakeLists.txt` (`find_package(Boost COMPONENTS headers serialization)` at `:56` stays only if the cache TOC still needs it; link it only to Engine)
  - approach: after this task, `grep -rnE "xml_[io]archive|boost/serialization" Engine Gui Serialization` lists only the cache TOC (`Engine/AppManagerPrivate.cpp`, the cache key headers, and their `RectI`/`RectD`/`Format`/`ImageLayerDesc` helpers). Remove the `*_SERIALIZATION_*` version macros.
  - verify: the grep result matches; `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug` then `tools/ci/local/test.sh ctest debug` are green. The build is required.
  - size: M

- [ ] M16.P7.T2 — Format reference and decision record
  - files: new `docs/project-format.md`, new `.plan/PLAN/DECISIONS/2026-10-10-project-format-resolved-calls.md` (published to `docs/decisions/` per `publish_decisions`), an amendment note appended to `.plan/PLAN/DECISIONS/2026-09-05-project-format-bundle-design.md` (sibling-relative connections, no import bridge), `Serialization/README.md` ("adding a field")
  - approach: the format reference covers: member order and alignment; `manifest.yaml`, `project.yaml` and `gui.yaml` schemas with an example; the curve grammar; the blob table format; `.nps`, `.nl` and clipboard documents; versioning (`formatVersion` bumps; unknown keys warn); the clean-break policy; and hand-repair (`unzip -p f.ntpz project.yaml`; repacking with Python `zipfile` `ZIP_STORED`, `mimetype` first, loads but loses alignment until the next Natron save).
  - verify: the example YAML is copied from P2.T3's pinned test output; links resolve.
  - size: M

- [ ] M16.P7.T3 — AppImage and user UAT checkpoint
  - files: `build/appimages/M16-<sha>.AppImage`, `build/appimages/M16-uat.md`
  - approach: build with the release `tools/ci/local/package.sh` and launch-check with `build/appimages/run-launch-check.sh`. The script:
    1. Open each regenerated `tools/bench/comps/*.ntpz`. Open an old `.ntp`, from a recent-files entry and via drag-drop, and get the clear old-format message with no crash.
    2. Save. `unzip -l` lists the members, and `unzip -p x.ntpz project.yaml` is readable.
    3. Move a node: only `gui.yaml` changes. Change a Blur size: one line of `project.yaml` changes.
    4. Point an input at a missing node by hand (repack with `zipfile`), open, and get one warning with the input disconnected.
    5. Paint 20 strokes and track 2 points over 200 frames. Autosave does not stall and is named `.ntpz.autosave`.
    6. Export and import a `.nps` preset; save and load an `.nl` layout; copy nodes, edit them in a text editor, and paste them back.
    7. `kill -9` during a save of the heavy project: the previous file is intact.
    8. The file manager associates `.ntpz` with Natron.
  - verify: the user signs off `M16-uat.md`. **Stop for the user.**
  - size: M

**Verification gate:** full `tools/ci/local/test.sh ctest debug` and `smoke debug` green, and
CI `format`, `lint-ci` and `build-and-test` green. The regenerated fixtures and comps are
`.ntpz` bundles. The dangling, order, robustness and old-format refusal tests pass (P5.T2).
The roto and tracker bundle round-trip is bit-exact (P5.T3). The one-off benchmark shows at
least 10x smaller files and faster save and autosave (P6.T2). No boost XML archive or
boost.serialization use remains outside the disk-cache TOC (P7.T1). The user signs off the
UAT (P7.T3).

## Decisions

- 2026-10-10 — **User decision: clean break, no backwards compatibility.** No import of old XML `.ntp`, `.nps` or `.nl` files. Opening one gives a clear old-format message and never crashes. The boost XML reader and writer and `Serialization/BoostXml/` are deleted, and so is the version-gated legacy migration code (legacy colour rows, the channel set below v17, knob layers below v14, timeline bounds, RC2/RC3). The fixtures and bench comps are regenerated through their scripts (`tools/ci/regen_read_time_offset.py`, `tools/bench/comps/build_comps.sh`). The two hand-written legacy-version fixtures (`channel-set-legacy-defaults.ntp`, `m65-legacy-color.ntp`) are **deleted, not regenerated**: they exist only to test the migration being removed, and any current-behaviour assertion in their tests is rebuilt in code. The size/speed benchmark is a one-off taken while both writers exist; its numbers stay in `BASELINE.md` and the scripts are deleted. Presets are new-format YAML only. Extends `docs/decisions/2026-09-18-clean-break-no-project-compat.md`.
- 2026-10-10 — **User decision: the bundle extension is `.ntpz`**, used in dialogs, MIME and desktop associations, autosave (`.ntpz.autosave`), recent files, the NatronRenderer CLI and the Python API docs. `.ntp` is recognised only to give the old-format message.
- 2026-10-10 — **User decision (confirmed 2026-10-10): connections are sibling-relative.** Group membership is stored by nesting (a group's `children:` map). A consumer's `inputs:` names a sibling's script name, keyed by input label. Expressions, links and `gui.yaml` keys stay fully qualified. **This amends `DECISIONS/2026-09-05-project-format-bundle-design.md`**, which said fully-qualified targets. Natron connections never cross group boundaries, and fully-qualified targets would rewrite every inner connection when a group is renamed.
- 2026-10-10 — **User decision: M93 precedes M16** (see M93's Decisions).
- 2026-10-10 — Presets keep the **`.nps`** extension, as a plain YAML document. Rationale: a preset is one small text file, not a container, so a `z` suffix would mislead. `.nps.yaml` would break the last-dot extension matching in `SequenceFileDialog` (`:733,1719,1868`) and the `NodeSettingsPanel.cpp:275` append logic, and it would be picked up by generic `*.yaml` filters. An old XML `.nps` is detected by content and gets the old-format message, the same way `.ntp` does.
- 2026-10-10 — Blob encoding: flat typed SoA column tables with a 64-byte header, with column names and dtypes repeated in the YAML descriptor and matched by name. They map with no copy, need no dependency or codegen (`flatc` is not in the image), and named columns give the schema evolution FlatBuffers was wanted for.
- 2026-10-10 — RB-3 is a reference, ported selectively: the curve grammar and the GUI and workspace shapes. `SerializationCompat` is not ported (no import bridge) and `KnobTableItemSerialization` is not ported (its model is absent here).
- 2026-10-10 — YAML library: yaml-cpp 0.8, from the system if the image has it, otherwise vendored under `libs/`, and private to `NatronSerialization`. Doubles go through `std::to_chars` for an exact round-trip.
- 2026-10-10 — Zip: an in-house stored-only `ZipStore`. No library aligns member data, and libzip is absent from the image.
- 2026-10-10 — Member split: the graph is in `project.yaml`; positions, viewers and layout are in `gui.yaml`; save date and author are in `manifest.yaml`. Cosmetic edits and re-saves then leave the graph diff untouched. Headless saves pass `gui.yaml` through.
- 2026-10-10 — boost.serialization is kept only for the disk-cache TOC (`Engine/AppManagerPrivate.cpp`, cache keys). Replacing it is a possible follow-up milestone, not M16 scope.
- 2026-10-10 — Out of scope: incremental or partial save, lazy load, and the node-creation and `Node::loadKnob` O(K²) open cost (follow-on work, per the design decision).
- 2026-10-22 — **Claimed despite strict file overlap** (user): run alongside M80, M16 and M158 in parallel lanes, with M64 - Tiled Rendering (parked) not treated as a blocker; overlaps with active rows (M86, M107, M154) and sibling lanes are one or two shared files, handled by rebasing after each merge.
- 2026-10-22 — Freshness check (§5a) at claim: stale file lists and line numbers corrected in place; no premise change.
- 2026-10-22 — Build mode on this host is remote: every `tools/ci/local/build.sh`/`test.sh` checkpoint in this file runs as `tools/ci/remote/build.sh debug --tests` on pushed HEAD instead (release where the task says release). Local-only steps (P1.T1 probe) use the natron-dev container.
- 2026-10-22 — P1.T1 probe (running natron-dev, `aswf/ci-vfxall:2027-clang21.1` base; the ghcr dev image adds nothing for these): yaml-cpp **0.9.0** at `/usr/local` with a CMake config (`yaml-cpp::yaml-cpp`, shared only, already bundled in AppImages via OCIO); zlib 1.3.2 at `/usr/local` with `ZLIB::ZLIB`. So `find_package(yaml-cpp 0.8 CONFIG)` takes 0.9.0 and nothing is vendored.
- 2026-10-22 — `ZipReader` maps with POSIX `mmap`, not `QFile::map`: `Serialization/` must not include Qt (README + layering script). A file truncated under a live mapping would SIGBUS, so saves must write-then-rename (already P3.T2's QSaveFile plan).
- 2026-10-22 — P1.T2's `grep -rn yaml-cpp Engine Gui` check is not literally empty: it matches the About-box licence qrc line and two old comments linking a yaml-cpp issue. No Engine/Gui file includes yaml-cpp, which is the check's intent.
- 2026-10-22 — Phase 16.2 load-side contract that Phase 16.3 must honour: (1) non-user knobs decode untyped, so call `completeKnobSerialization(knob, liveTypeName, liveDimension, &liveChoiceEntries)` before `fromSerialization`; (2) defaults are saved only for user knobs, so `KnobHelper::fromSerialization` must skip `cloneDefaultValues` when none was saved; (3) only free/broken keys keep derivatives, so loaded curves recompute them (`Curve::setKeyframes(keys, true)` or equivalent).
- 2026-10-22 — Choice knobs have no stored label in the data model: written as the option ID, plus `{value: idx, option: id}` when the index doesn't match that ID. `gui.yaml` drops `legacyBackdrops`/`legacyPanes`/`version` (clean break). Only `Serialization/Yaml/*.cpp` and `YamlPrivate.h` include yaml-cpp. Subnormal doubles parse via `strtod_l` (libstdc++ `from_chars` rejects them). The example `project.yaml` for the docs is pinned as `kExampleProjectYaml` in `Tests/Serialization/ProjectYaml_Test.cpp`.
- 2026-10-22 — Between Phase 16.3 and 16.4, copy/paste, `.nl` import and `.nps` import are inert (the XML readers are gone; `decodeClipboardNodes` returns false and cut still deletes). Phase 16.4 restores them; the milestone never ships in between.
- 2026-10-22 — `softwareVersion`, `lastAuthor` and `lastSaveDate` project knobs are kept out of `project.yaml` so re-saves stay byte-identical; author and date round-trip through `manifest.yaml`, `softwareVersion` is set from the running build on load.
- 2026-10-22 — A missing link target is a load warning, not a failed load (`KnobI::restoreLinks` reports `missingTargets`). The GUI summary dialog lists the first 20 warnings.
- 2026-10-22 — The clipboard document uses the project node schema, which omits the clone master (`masterNodeName`), so a pasted clone is no longer linked to its original. Duplicate and clone don't go through the clipboard. M92 - Linked Nodes defines linked-node fields; accepted under the clean break.
- 2026-10-22 — A default-layout preference pointing at an XML, damaged or empty `.nl` falls back to the built-in layout with an error dialog (previously: error and no layout at all).
- 2026-10-22 — Build of `ffbddea94` (P3+P4): 30 tests red. Root cause: `detectProjectFileKind` sniffed 64 bytes, but the bundle mime check needs 72, so every `.ntpz` was refused as unrecognized. Fixed in `07dea085a`, along with a test that expected 5 manifest members where there are 4. The bench comps still need regenerating, and the decode path is unexercised until a build passes detection.
- 2026-10-22 — P5.T2–T4 code is in `098c600e8`, unchecked until a green build. `.nps`/`.nl` loads are GUI-only and not linked into the Engine test binary, so P5.T2 case 9 tests the shared `Serialization::isXmlDocument` check and the YAML decoders throwing `YamlError`. `app.loadProject` on an old `.ntp` returns `None` rather than raising, and the message goes to stderr; the smoke check captures it there. P5.T3 compares the first and second load with `ProjectEquivalence`, not the built graph with the first load, because loading a RotoPaint switches its paint tool and changes its compositing-operator knob.
- 2026-10-22 — The build of `07dea085a` passed 336 of 338 tests; the decode path is sound. The remaining failures were a string-animation encode mismatch (fixed in `19302cc5e`) and the missing comps. The fixture and comps were regenerated as `.ntpz` with that build's AppImage, and the old `.ntp` files were removed (`e569e4b26`). `build_comps.sh` hard-codes `build/release`, so `build_comps.py` was run directly. Its repack now mirrors `ZipStore`, with aligned members and a manifest whose size and CRC32 match. P5.T5 runs as a remote debug build of `e569e4b26`.
- 2026-10-22 — The P6 benchmark runs from a fast AppImage under NatronRenderer, because this host builds remotely and has no local build to run a C++ bench from. That needs two temporary Python methods, `App.saveLegacyXmlForBenchmark` and `App.autoSaveForBenchmark`. The second exists because `Project::autoSave` returns early in background mode. This deviates from P3.T2's "no UI, no Python", and P7.T1 deletes both methods along with the XML writer. The Python roto API can't create paint strokes, so the heavy project's 200 "strokes" are unfinished 500-point beziers without pressure.

## Handoff (2026-10-22, PM paused by user)

- Lane: `.worktrees/m16`, branch `milestone/m16-project-format-redesign`, HEAD `ffbddea94`, pushed. Checked: P1.*, P2.*, P3.T1–T7, P4.T1–T3, P5.T1 (P3/P4 code checked off before its build — re-verify).
- Builds: P2 green (338/338). A remote **fast** build of `c59e7d1ab` (P3) was in flight when paused (GitHub run `fast-c59e7d1a-7006b8a8`; local watcher stopped, detached `build.sh` may still write `/tmp/natron-builds/m16-p3.{log,done}` and `build/remote/fast/` in the lane). Expected failures until fixtures are regenerated: `CacheEquivalenceBenchCompTest.EditsAndDisables` and smoke `check_reader_cli_time_offset_regression`.
- Next build should be of HEAD (P3+P4); then P3.T8 steps 2–4: regenerate `Tests/fixtures/read-time-offset.ntpz` (`tools/ci/regen_read_time_offset.py`) and `tools/bench/comps/*.ntpz` (`build_comps.sh`) with NatronRenderer from the fast AppImage, `git rm` the old `.ntp` files.
- **Uncommitted in the worktree:** a stopped P5.T2–T4 implementer's partial work (`Tests/Serialization/ProjectEquivalence.{h,cpp}`, `Tests/fixtures/bundle/`, `Tests/fixtures/old-format.ntp`, `Tests/CMakeLists.txt` edit). Re-dispatch P5.T2–T4 over it.
- Remaining after that: P5.T5 build, Phase 16.6 benchmark (release build), Phase 16.7 cleanup/docs/UAT, gate, PR.
