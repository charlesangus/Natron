# M46 - Labelmaker Annotations

Full title: Port Labelmaker as a native node graph annotation feature

A native C++ reimplementation of github.com/charlesangus/Labelmaker (design and config schema carried over, no code). Nodes show what they are doing on the node graph:
- knob readouts that appear once a knob leaves its default;
- colour swatches;
- a channel/mask/premult/mix line;
- "Class | name" when a node has been renamed;
- Read/Write file names.

The readouts are driven by a shipped base config plus a personal JSON config keyed by plugin ID. An undoable "De-overlap All" tidies graphs whose nodes grew. Annotations are computed at draw time, never saved, so project files are unchanged.

The layout rides on M56 Phase 56.1 (node text grows to a maximum width, then wraps). One build at the end (user rule): implementers do not compile.

## Phase 46.1: Annotation model (Engine, pure)

- [ ] M46.P1.T1 — Parse and layer annotation configs, with unit tests
  - files: `Engine/NodeAnnotation.h` (new), `Engine/NodeAnnotationConfig.cpp` (new), `Tests/NodeAnnotationConfig_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - `struct AnnotationLineSpec { std::string knob, label, templ; std::optional<QJsonValue> defaultValue; bool alwaysShow=false, colorize=true; }`.
    - `using AnnotationConfig = std::map<std::string /*pluginID*/, std::vector<AnnotationLineSpec>>`.
    - `bool parseAnnotationConfig(const QByteArray&, AnnotationConfig*, QStringList* warnings)` uses `QJsonDocument`:
      - accept Labelmaker's keys, with `template` replacing `tcl_string`;
      - warn on, and skip, unknown keys or a line with neither `name` nor `template`;
      - reject malformed JSON as a whole, with the parse error's offset.
    - `AnnotationConfig layerConfigs(const std::vector<AnnotationConfig>&)`: later layers replace whole plugin entries, as Labelmaker does.
    - A key may also carry a major version, e.g. `"net.sf.openfx.GradePlugin"` or `"...@2"`. The versioned key wins.
  - verify: `ctest -R NodeAnnotationConfig` covers:
    - a valid file;
    - an unknown key (a warning, the line kept);
    - a line with neither field (skipped);
    - broken JSON (rejected, with the offset);
    - a user layer replacing a base class wholesale;
    - a versioned key overriding an unversioned one.
  - size: M

- [ ] M46.P1.T2 — Evaluate a node's configured lines at a time
  - files: `Engine/NodeAnnotation.h`, `Engine/NodeAnnotation.cpp` (new), `Tests/NodeAnnotation_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - `std::vector<AnnotationLine> evaluateAnnotationLines(const NodePtr&, const std::vector<AnnotationLineSpec>&, double time, bool alwaysShowAll)`.
    - `AnnotationLine { QString text; std::optional<std::array<double,4>> swatch; }`.
    - Look knobs up with `Node::getKnobByName`. Skip a missing knob and log it once per (plugin, knob).
    - When a line shows:
      - with `alwaysShowAll`, or `alwaysShow` set, it always shows;
      - otherwise, with a config `default`, it shows when any dimension differs from that default (a scalar or a per-dimension list);
      - otherwise it shows when `KnobI::hasModifications()` is true.
    - Value formatting:
      - doubles use `%g` trimmed to 3 decimals, and multi-dimension values are space-separated;
      - choices use the option label; bools read on/off; strings and files are used as-is.
    - Text: `label value`, where `label` defaults to the knob's script name.
    - Templates replace `{knob}` with the formatted value, and `{knob.N}` with dimension N.
    - A colour knob (`KnobColor`) with `colorize` set fills `swatch` with its values at `time`.
    - Read values at `time` so animated knobs show the current frame's value.
  - verify: `ctest -R NodeAnnotation` (a BaseTest app fixture) covers:
    - Blur `size` at its default: no line; set to 5: "size 5";
    - `alwaysShow` gives a line at the default;
    - a template "{operation}" on Merge gives the operation label;
    - a Grade `gain` of (2,1,1,1) gives a swatch;
    - an animated knob reports different values at two times;
    - a missing knob is skipped without throwing.
  - size: L

- [ ] M46.P1.T3 — Add the built-in readouts: channel/mask/premult/mix line, class disambiguation, file name
  - files: `Engine/NodeAnnotation.h`, `Engine/NodeAnnotation.cpp`, `Tests/NodeAnnotation_Test.cpp`
  - approach:
    - `QString channelSummaryLine(const NodePtr&)` extends today's layer summary (the logic in `NodeGui::onLayerSelectionChanged`):
      - `(rgb M red)` when a mask input is connected and a mask channel is chosen; `Minv` when `kOfxMaskInvertParamName` is set;
      - ` /* alpha` when (un)premult is on;
      - a separate `mix 0.5` when `kOfxMixParamName` is not 1.
      - Find the premult and mask-channel knob script names on the native nodes (e.g. Grade's, from `Engine/Nodes/Color/Grade.cpp`) and on the openfx-supportext mask/mix helpers, and handle both.
    - `QString classDisambiguation(const NodePtr&)` returns `"<plugin label> | <node label>"` when the node label doesn't start with the plugin's label (without its suffix), and empty otherwise.
    - `QString fileLine(const NodePtr&)` returns the basename of the Read/Write file knob (use `isReader()`/`isWriter()` and the file knob). For Writes, it adds the output colour space and file type when they are not the defaults.
  - verify: `ctest -R NodeAnnotation` adds cases for:
    - a masked, inverted Grade with mix 0.5;
    - a Blur renamed "guy" → "Blur | guy", while Blur1 gives nothing;
    - a Read of `/tmp/x/plate.####.exr` → "plate.####.exr".
  - size: M

- [ ] M46.P1.T4 — Author the shipped base config for Natron's nodes, with a test that every key and knob exists
  - files: `Gui/Resources/NodeAnnotations/base_config.json` (new), `Gui/GuiResources.qrc`, `Tests/NodeAnnotationBaseConfig_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Translate Labelmaker's `base_config.json` (412 lines) class by class to Natron plugin IDs and knob script names. Cover Blur, Grade, ColorCorrect, Transform, Merge, Shuffle, Reformat, Crop, Erode/Dilate, Saturation, Clamp, TimeOffset, FrameRange, Retime, and the native Read/Write.
    - Classes with no Natron equivalent are dropped. List them in the task report.
    - Ship the file in the qrc under `:/Resources/NodeAnnotations/base_config.json`.
    - The test reads the source file through a compile definition (as `NATRON_TESTS_FIXTURES_DIR` does). For every plugin ID it creates the node and checks that each `name` and each `{knob}` in a template exists.
  - verify: `ctest -R NodeAnnotationBaseConfig` passes, with no unknown plugin or knob.
  - size: M

## Phase 46.2: Preferences, config store and refresh

- [ ] M46.P2.T1 — Add the annotation preferences
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`
  - approach:
    - On the Nodegraph page (`_nodegraphTab`, `Settings.cpp:1213`), add:
      - "Show node annotations" (default on);
      - "Always show all annotation lines" (off);
      - "Colour swatches in annotations" (on);
      - "Use built-in annotation config" (on);
      - "Personal annotation config" (a file knob; default `<QStandardPaths::AppConfigLocation>/node_annotations.json`).
    - Add accessors. In `onKnobValueChanged`, any of these calls a new `AppManager`-level "annotation settings changed" notification, following the pattern other nodegraph settings use to refresh the graph.
  - verify: at the gate, the five knobs appear on the Nodegraph tab, and toggling "Show node annotations" off restores today's exact label on every node (P5.T1).
  - size: M

- [ ] M46.P2.T2 — Add a config store that loads base + personal, reloads on change, and notifies NodeGuis
  - files: `Gui/NodeAnnotationStore.h` (new), `Gui/NodeAnnotationStore.cpp` (new), `Gui/GuiApplicationManager.h`, `Gui/GuiApplicationManager.cpp`, `Gui/NodeGraph35.cpp`
  - approach:
    - A `QObject` owned by `GuiApplicationManager`. It loads the qrc base (if enabled) and the personal file (if present) through P1.T1, and holds the layered config.
    - It emits `configChanged()` after:
      - a settings change (P2.T1);
      - a `QFileSystemWatcher` change on the personal file, debounced to 200 ms;
      - "Reload Node Annotations" in the graph's right-click Edit menu (`NodeGraph35.cpp`).
    - Parse warnings go to the Script Editor log once per load. A broken personal file keeps the last good config and logs the error.
    - On `configChanged`, every `NodeGraph` refreshes all its NodeGuis, recursing into groups (walk the project's nodes, as M55.P1.T3 did).
  - verify: P5.T1 edits the personal file while the app is running. The Blur gains its new line within a second with no restart, and a malformed edit leaves the old lines in place and logs an error.
  - size: M

- [ ] M46.P2.T3 — Refresh a node's annotation on knob, label, input and frame changes, coalesced
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Gui/NodeGui.h`, `Gui/NodeGui.cpp`
  - approach:
    - In `Node::onEffectKnobValueChanged` (`Node.cpp:6123`), emit a new `annotationInputsChanged()` for every knob change. Keep the existing per-knob branches.
    - Emit it also from input connect/disconnect, so the mask readout follows the mask pipe.
    - `NodeGui` connects it queued to `scheduleAnnotationRefresh()`: a single-shot 0 ms timer that coalesces a burst into one `setNameItemHtml`. Renames already go through `onInternalNameChanged`.
    - Frame changes: connect the app timeline's `frameChanged` once per `NodeGraph`. Refresh only the NodeGuis whose last evaluation read an animated knob; `NodeGui` keeps that flag from P1.T2's evaluation.
    - Skip Dot, Backdrop, Viewer and multi-instance children.
  - verify: P5.T1 checks that:
    - editing Blur size updates the label once;
    - scrubbing frames on a node with an animated size updates it;
    - connecting a mask pipe adds `M`;
    - a 200-node graph at the gate build stays interactive while scrubbing (a timed scrub, logged).
  - size: M

## Phase 46.3: Drawing on the node

- [ ] M46.P3.T1 — Add a pure helper that turns annotation lines into label HTML, with a swatch tone map, plus unit tests
  - files: `Gui/NodeAnnotationHtml.h` (new), `Gui/NodeAnnotationHtml.cpp` (new), `Tests/NodeAnnotationHtml_Test.cpp` (new), `Tests/CMakeLists.txt` (`GuiTests_SOURCES`)
  - approach:
    - `QString annotationLinesToHtml(const std::vector<AnnotationLine>&, bool swatches)`.
    - Each line is a paragraph, HTML-escaped.
    - A swatch line is `<span style="background-color:#RRGGBB;color:#..">&nbsp;text&nbsp;</span>`:
      - the background is the linear RGB tone-mapped to display: a Reinhard-style shoulder then sRGB encode, approximating Labelmaker's AlexaToRec choice so bright gains stay legible;
      - the text colour comes from `NodeGui::contrastingLabelTextColor`.
    - No Natron node types, so it tests offscreen.
  - verify: `ctest -R NodeAnnotationHtml` covers:
    - escaping of `<&>`;
    - a gain of (8,8,8) maps to a non-white, non-clipped swatch;
    - (0,0,0) gives white text, and (1,1,1) gives black;
    - with swatches off, there is no span.
  - size: M

- [ ] M46.P3.T2 — Compose annotations into the node label
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach:
    - In `setNameItemHtml` (`NodeGui.cpp:3348`), when annotations are enabled, build the text in this order:
      - the name, or `classDisambiguation` when non-empty;
      - the sublabel or Merge operator (unchanged);
      - the channel summary line from P1.T3, which **replaces** `_channelsExtraLabel` so the layer summary is never shown twice;
      - the file line;
      - the configured lines (P1.T2 → P3.T1 HTML);
      - the user label (unchanged, with its font and colour tags still honoured).
    - Annotation lines use a font one step smaller than the name.
    - Width: M56.P1's width cap and wrap applies to the whole block. Don't add a second width rule.
    - `adjustSizeToContent` and `refreshPreviewAndLabelPosition` already size from `_nameItem->boundingRect()`. Check they stay correct with the preview on.
    - With annotations off, the output must be byte-identical to today's, so the `finalText == oldText` early return still works.
  - verify: P5.T1 shots of a Grade with a swatch, a masked Merge, a renamed Transform, and a Read. `m24_border.py` and M56.P1's wrap check still pass. The node outline contains the label's bounding rect in every shot.
  - size: L

## Phase 46.4: De-overlap

- [ ] M46.P4.T1 — Add a pure de-overlap planner with unit tests
  - files: `Gui/NodeGraphDeoverlap.h` (new), `Gui/NodeGraphDeoverlap.cpp` (new), `Tests/NodeGraphDeoverlap_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Port `labelmaker_deoverlap.py` (178 lines) as `std::vector<std::pair<id, QPointF>> planDeoverlap(const std::vector<DeoverlapRect>&, std::optional<id> grownOnly)`:
    - sweep top to bottom;
    - a node overlapping a node above it is pushed down by the overlap plus `NodeGui::DEFAULT_OFFSET_BETWEEN_NODES`, and pushes cascade;
    - side-by-side nodes at the same height are untouched;
    - backdrops and viewers are skipped;
    - with `grownOnly` set, only overlaps caused by that node's growth are resolved;
    - nothing is ever moved up.
  - verify: `ctest -R NodeGraphDeoverlap` covers: a two-node overlap, a three-node cascade, side-by-side left alone, a backdrop ignored, an existing unrelated overlap left alone in `grownOnly` mode, and no upward moves.
  - size: M

- [ ] M46.P4.T2 — Add "De-overlap All Nodes" as an undoable graph command
  - files: `Gui/NodeGraph35.cpp`, `Gui/NodeGraph45.cpp`, `Gui/NodeGraph.h`, `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`
  - approach:
    - Add a graph Edit-menu action with a registered (unbound) shortcut ID. It runs P4.T1 over the graph's visible nodes and pushes one `MoveMultipleNodesCommand` with the text "De-overlap nodes".
    - Auto de-overlap on label growth goes in only if Q4 says so. If it does, it's a separate follow-up task that pushes its own move command, so undo stays consistent. Never hidden moves.
  - verify: P5.T1 makes three overlapping nodes, runs the action (they no longer overlap), presses Ctrl+Z (they're back exactly), then Ctrl+Shift+Z.
  - size: M

## Phase 46.5: Tests and UAT

- [ ] M46.P5.T1 — Write a GUI test script for annotations
  - files: `Tests/gui/m46_annotations.py` (new)
  - approach: Model it on `Tests/gui/m55_node_graph.py`. Cover each verify bullet in P2.T1–P4.T2. Read the label text from the NodeGui's text item (the `QGraphicsTextItem` under the node in the scene) rather than from pixels where possible. Point the personal config at a temp file through the preference. Take a shot of every case, plus a busy-graph overview before and after "De-overlap All".
  - verify: `NATRON_GUI_TEST_OUT=build/m46-shots Tests/gui/run-gui-test.sh Tests/gui/m46_annotations.py build/fast/App/Natron` reports 0 failures at the gate build.
  - size: M

- [ ] M46.P5.T2 — Run the one build, the tests, and package the AppImage for UAT
  - files: none
  - approach: re-run cmake (new globbed files and the qrc), build, run `ctest`, then `m46_annotations.py`, `m24_border.py`, `m55_node_graph.py` and M56's wrap script. Package `build/appimages/M46-<sha>.AppImage`.
  - verify: everything is green, and the AppImage is posted for UAT.
  - size: M

**Verification gate:** CI `format`, `lint-ci` and `build-and-test` are green, including the `NodeAnnotationConfig`, `NodeAnnotation`, `NodeAnnotationBaseConfig`, `NodeAnnotationHtml` and `NodeGraphDeoverlap` tests. `Tests/gui/m46_annotations.py`, `m24_border.py` and `m55_node_graph.py` report 0 failures on the gate build. The user signs off the shots and the AppImage UAT, which covers: readouts appearing and disappearing with defaults, swatches, the channel/mask/mix line, "Class | name", Read/Write file names, a live personal-config reload, the preference off giving the old labels, and an undoable De-overlap All.

Acceptance sketch:
- Nodes can display rich, Labelmaker-style annotation text/data on the node
  graph.

Scheduling note (2026-10-08, user): runs together with M55. Before elaborating, evaluate whether Labelmaker-style node-graph info belongs inside node-graph polish; elaborate them as one milestone or two accordingly.
