# M46 - Labelmaker Annotations

Full title: Port Labelmaker as a native node graph annotation feature

A native C++ reimplementation of github.com/charlesangus/Labelmaker (design and config schema carried over, no code). Nodes show what they are doing on the node graph:
- knob readouts that appear once a knob leaves its default;
- colour swatches;
- a channel/mask/premult/mix line;
- "Class | name" when a node has been renamed;
- Read/Write file names.

The readouts are driven by a shipped base config plus a personal JSON config keyed by plugin ID. When a node's label grows, the nodes below it are pushed down automatically, off the undo stack (as Labelmaker does; a preference turns this off). An undoable "De-overlap All" tidies whole graphs. Config lines can also name a Python readout function (`"python": "module.func"`), which runs lazily: in batched, time-budgeted slices on an idle-priority pass, only for dirty nodes in view. Stale labels are acceptable, and GUI stalls are not. Annotations are computed at draw time, never saved, so project files are unchanged.

The layout rides on M56 Phase 56.1 (node text grows to a maximum width, then wraps). One build at the end (user rule): implementers do not compile.

## Scope

**In:**
- A built-in channel-flow readout for Merge-style nodes, as Labelmaker has: native Merge reads `(rgba) plus (rgba) --> (rgba)` from its A, B and Output channel toggles, and ChannelCopy and Shuffle show the channels they route (P1.T3).

**Out:**
- A config editor: users edit the JSON (decided 2026-10-11). No milestone owns it.

**Assumptions:**
- Unconfirmed: the base config and built-in readouts target native plugin IDs first, plus the bundled OFX nodes that have no native replacement yet. OFX twins of native nodes get no entries.
- Unconfirmed: Labelmaker's other readouts carry over too: Write's render order (with colour space and file type), and the Roto and RotoPaint entries in its base config.
- Unconfirmed: the interview stopped at its second question (the milestone went on hold). Callers, failure paths, edge cases and the gate were not walked. Resume with `/cat-grill M46`.

## Phase 46.1: Annotation model (Engine, pure)

- [ ] M46.P1.T1 — Parse and layer annotation configs, with unit tests
  - files: `Engine/NodeAnnotation.h` (new), `Engine/NodeAnnotationConfig.cpp` (new), `Tests/NodeAnnotationConfig_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - `struct AnnotationLineSpec { std::string knob, label, templ, python; std::optional<QJsonValue> defaultValue; bool alwaysShow=false, colorize=true; enum class Refresh { Node, Frame } refresh = Refresh::Node; }`.
    - A line may give `"python": "<module>.<func>"` in place of `name`/`template`, optionally with `label`, `alwaysShow` and `"refresh": "node" | "frame"` (default `"node"`). With `python` and also `name` or `template`, warn and let `python` win. An unknown `refresh` value warns and falls back to `node`. Parsing never touches Python; P1.T5 resolves the function.
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
    - a `python` line parsed, with `"refresh": "frame"`; `python` plus `template` (a warning, `python` kept); an unknown `refresh` (a warning, `node`);
    - broken JSON (rejected, with the offset);
    - a user layer replacing a base class wholesale;
    - a versioned key overriding an unversioned one.
  - size: M

- [ ] M46.P1.T2 — Evaluate a node's configured lines at a time
  - files: `Engine/NodeAnnotation.h`, `Engine/NodeAnnotation.cpp` (new), `Tests/NodeAnnotation_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - `AnnotationEvalResult evaluateAnnotationLines(const NodePtr&, const std::vector<AnnotationLineSpec>&, double time, bool alwaysShowAll, const std::vector<std::optional<QString>>* pyResults)`.
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
    - Python lines: this function stays pure C++ and never takes the GIL. `pyResults` holds the node's cached Python results from P1.T5 ( indexed by the spec's position among the node's `python` lines). A `python` line gives `label text` from that cache, or no line when the entry is missing or empty.
    - `AnnotationEvalResult { std::vector<AnnotationLine> lines; bool needsPython, frameDependent; }`. `needsPython` means the specs contain a `python` line. `frameDependent` means an animated or expression-driven knob was read, or a `python` line has `refresh: frame`.
  - verify: `ctest -R NodeAnnotation` (a BaseTest app fixture) covers:
    - Blur `size` at its default: no line; set to 5: "size 5";
    - `alwaysShow` gives a line at the default;
    - a template "{operation}" on Merge gives the operation label;
    - a Grade `gain` of (2,1,1,1) gives a swatch;
    - an animated knob reports different values at two times;
    - a missing knob is skipped without throwing;
    - a spec with a `python` line and no cached result gives no line and `needsPython == true`; with a cached "x 3" it gives that line;
    - an animated knob, or a `refresh: frame` Python line, sets `frameDependent`; a static Blur does not.
  - size: L

- [ ] M46.P1.T3 — Add the built-in readouts: channel/mask/premult/mix line, Merge channel flow, class disambiguation, file name
  - files: `Engine/NodeAnnotation.h`, `Engine/NodeAnnotation.cpp`, `Tests/NodeAnnotation_Test.cpp`
  - approach:
    - `QString channelSummaryLine(const NodePtr&)` extends today's layer summary (the logic in `NodeGui::onLayerSelectionChanged`):
      - `(rgb M red)` when a mask input is connected and a mask channel is chosen; `Minv` when `kOfxMaskInvertParamName` is set;
      - ` /* alpha` when (un)premult is on;
      - a separate `mix 0.5` when `kOfxMixParamName` is not 1.
      - Find the premult and mask-channel knob script names on the native nodes (e.g. Grade's, from `Engine/Nodes/Color/Grade.cpp`) and on the openfx-supportext mask/mix helpers, and handle both.
    - `QString mergeFlowLine(const NodePtr&)` is the channel-flow readout for Merge-style nodes. For the native Merge (`Engine/Nodes/Merge/Merge.cpp`), it reads the A, B and Output channel toggles (`kMergeParamAChannels`, `kMergeParamBChannels`, `kMergeParamOutputChannels`) as `(rgba) plus (rgba) --> (rgba)`, using the operation's label. That line replaces today's Merge operator sublabel when annotations are on. ChannelCopy and Shuffle (`Engine/Nodes/Channel/`) get a line naming the channels they route.
    - `QString classDisambiguation(const NodePtr&)` returns `"<plugin label> | <node label>"` when the node label doesn't start with the plugin's label (without its suffix), and empty otherwise.
    - `QString fileLine(const NodePtr&)` returns the basename of the Read/Write file knob (use `isReader()`/`isWriter()` and the file knob). For Writes, it adds the output colour space and file type when they are not the defaults.
  - verify: `ctest -R NodeAnnotation` adds cases for:
    - a masked, inverted Grade with mix 0.5;
    - a Blur renamed "guy" → "Blur | guy", while Blur1 gives nothing;
    - a Read of `/tmp/x/plate.####.exr` → "plate.####.exr";
    - a Merge (plus) with B alpha off → `(rgba) plus (rgb) --> (rgba)`, and a ChannelCopy and a Shuffle routing alpha into red, each with its routing line.
  - size: L

- [ ] M46.P1.T4 — Author the shipped base config for Natron's nodes, with a test that every key and knob exists
  - files: `Gui/Resources/NodeAnnotations/base_config.json` (new), `Gui/GuiResources.qrc`, `Tests/NodeAnnotationBaseConfig_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Translate Labelmaker's `base_config.json` (412 lines) class by class to Natron plugin IDs and knob script names. Cover Blur, Grade, ColorCorrect, Transform, Merge, Shuffle, Reformat, Crop, Erode/Dilate, Saturation, Clamp, TimeOffset, FrameRange, Retime, and the native Read/Write.
    - Classes with no Natron equivalent are dropped. List them in the task report.
    - Ship the file in the qrc under `:/Resources/NodeAnnotations/base_config.json`.
    - The test reads the source file through a compile definition (as `NATRON_TESTS_FIXTURES_DIR` does). For every plugin ID it creates the node and checks that each `name` and each `{knob}` in a template exists.
  - verify: `ctest -R NodeAnnotationBaseConfig` passes, with no unknown plugin or knob.
  - size: M

- [ ] M46.P1.T5 — Run Python readout functions in one batched, time-budgeted pass with callables resolved once
  - files: `Engine/NodeAnnotationPython.h` (new), `Engine/NodeAnnotationPython.cpp` (new), `Tests/NodeAnnotationPython_Test.cpp` (new), `Tests/CMakeLists.txt`
  - context: today every expression and callback goes through `PyRun_String` on a freshly built string (`Knob.cpp:3119`, `interpretPythonScript` at `AppManager.cpp:3760`), so each call parses and compiles. No compiled cache exists (`Expr::code` is commented out, and `compilePyScript` is `#if 0`). This task never uses that path. Node objects already exist as `app1.<node>` Shiboken wrappers, with knobs pre-bound as attributes (`Node::declarePythonFields`), so `thisNode.size.get()` is one attribute lookup plus one wrapped call.
  - approach:
    - `class AnnotationPythonRunner`, GUI thread only:
      - `resolve(const AnnotationConfig&)`, under one `PythonGILLocker`, imports each distinct `module.func` once (`PyImport_ImportModule` + `PyObject_GetAttrString`; modules in the PyPlug search paths are already on `sys.path`). It checks the signature with `getFunctionArguments` (exactly `(thisNode, frame)`) and keeps strong refs. A failure logs once to the Script Editor and marks that spec disabled.
      - `clear()` drops every ref under the GIL; it is called before each `resolve` and at app shutdown, before `Py_Finalize`.
    - `struct PyJob { NodePtr node; std::vector<const AnnotationLineSpec*> lines; }`; `struct PyJobResult { NodePtr node; std::vector<std::optional<QString>> texts; bool error; }`.
    - `size_t run(std::vector<PyJob>& jobs, double frame, std::chrono::microseconds budget, std::vector<PyJobResult>* out)` returns the number of jobs done:
      - it takes **one** `PythonGILLocker` for the whole batch;
      - it resolves each node's `Effect` wrapper from `appID + "." + node->getFullyQualifiedName()` with a new ref-correct helper (`getAttrRecursive` leaks a ref per segment, so don't call it per tick), cached per node and dropped on rename, delete and `clear()`;
      - it calls `PyObject_Vectorcall(func, {nodeObj, PyFloat(frame)})`, with no string building or compiling;
      - it accepts `None` (no line), a `str`, or a list of `str` (several lines). Anything else counts as an error;
      - on an exception, `PyErr_Fetch` plus the formatted traceback go to the Script Editor once per (func, message) until the next `resolve`, and the line reads `"<label> ⚠"`;
      - it times each call. Three consecutive failures, or a single call over 50 ms, disable that function until the next `resolve` (logged);
      - it checks elapsed time after each job and returns early once over `budget`; the caller re-queues the rest;
      - after the batch it drains the `catchOut`/`catchErr` buffers once, as `interpretPythonScript` does, so `print` output reaches the Script Editor;
      - a static re-entrancy guard makes a nested `run` (e.g. from a nested event loop) return 0.
    - Document both limits in the header and in the user-facing config notes:
      - **No timeout:** CPython cannot safely interrupt code on the same thread. A function that hangs hangs the UI, like any Natron Python callback. Only slow-but-finishing functions are caught, by the 50 ms auto-disable.
      - **No cross-node tracking:** a function reading another node's knobs refreshes only on its own node's changes, on frame changes if marked `refresh: frame`, and on "Reload Node Annotations".
    - Readout functions must be read-only. P2.T3 ignores knob changes on a node while that node is being evaluated, so a function that sets knobs can't refresh forever.
    - Cost (estimated, measured at the gate): about 5–10 µs per Python line (GIL 0.2 µs per batch, call about 0.5 µs, 3 knob reads 3–8 µs, result conversion 0.3 µs), so all 200 nodes dirty is about 1–2 ms. The old per-line `PyRun_String` approach would have been about 18 ms.
  - verify: `ctest -R NodeAnnotationPython` (a BaseTest app fixture with Python, as `PyPlugExport_Test` uses) covers:
    - a function from a module injected into `sys.modules` returns "size 5" for a Blur with size 5;
    - a list return gives two lines, and `None` gives none;
    - a raising function gives `⚠` and exactly one log entry, and is disabled after the third failure;
    - a wrong signature is rejected at `resolve`, and so is a missing module;
    - a 1 ms budget over 50 jobs returns fewer than 50, and a second call finishes the rest;
    - a function doing `time.sleep(0.06)` is disabled as slow;
    - 200 trivial jobs complete under a CI-safe 20 ms ceiling, and the real time is logged for the gate report.
  - size: L

## Phase 46.2: Preferences, config store and refresh

- [ ] M46.P2.T1 — Add the annotation preferences
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`
  - approach:
    - On the Nodegraph page (`_nodegraphTab`, `Settings.cpp:1213`), add:
      - "Show node annotations" (default on);
      - "Always show all annotation lines" (off);
      - "Colour swatches in annotations" (on);
      - "Use built-in annotation config" (on);
      - "Personal annotation config" (a file knob; default `~/.Natron/node_annotations.json`, i.e. `node_annotations.json` in the user PyPlug directory that `AppManager::getAllNonOFXPluginsPaths` builds as `mainPath` (`AppManager.cpp:1767`), next to the user's `init.py`. Don't use `AppConfigLocation`);
      - "Push nodes down when an annotation grows" (default on; see P4.T3).
    - Add accessors. In `onKnobValueChanged`, any of these calls a new `AppManager`-level "annotation settings changed" notification, following the pattern other nodegraph settings use to refresh the graph.
  - verify: at the gate, the six knobs appear on the Nodegraph tab, the personal-config default resolves to `~/.Natron/node_annotations.json`, and toggling "Show node annotations" off restores today's exact label on every node (P5.T1).
  - size: M

- [ ] M46.P2.T2 — Add a config store that loads base + personal, reloads on change, and notifies NodeGuis
  - files: `Gui/NodeAnnotationStore.h` (new), `Gui/NodeAnnotationStore.cpp` (new), `Gui/GuiApplicationManager.h`, `Gui/GuiApplicationManager.cpp`, `Gui/NodeGraph35.cpp`
  - approach:
    - A `QObject` owned by `GuiApplicationManager`. It loads, through P1.T1, and layers in this order:
      - the qrc base (if enabled);
      - any `node_annotations.json` found in the other PyPlug search directories (`AppManager::getAllNonOFXPluginsPaths`: `NATRON_PLUGIN_PATH` entries and the preference's extra paths, in search order), so a studio can ship a shared config next to its PyPlugs;
      - the personal file from P2.T1 (if present), last.
    - It holds the layered config and owns the `AnnotationPythonRunner` (P1.T5). On every successful (re)load it calls `runner.clear()` then `runner.resolve(config)` before emitting `configChanged()`.
    - The `QFileSystemWatcher` watches the personal file and each found search-path file.
    - It emits `configChanged()` after:
      - a settings change (P2.T1);
      - a `QFileSystemWatcher` change on a watched file, debounced to 200 ms. "Reload Node Annotations" also re-imports Python modules: `importlib.reload` on each module named by a `python` line, before `resolve`;
      - "Reload Node Annotations" in the graph's right-click Edit menu (`NodeGraph35.cpp`).
    - Parse warnings go to the Script Editor log once per load. A broken personal file keeps the last good config and logs the error.
    - On `configChanged`, every `NodeGraph` refreshes all its NodeGuis, recursing into groups (walk the project's nodes, as M55.P1.T3 did).
  - verify: P5.T1 edits the personal file while the app is running. The Blur gains its new line within a second with no restart, and a malformed edit leaves the old lines in place and logs an error. A `node_annotations.json` in a `NATRON_PLUGIN_PATH` directory is layered under the personal file. A `python` line whose function file is edited picks up the new code after "Reload Node Annotations".
  - size: M

- [ ] M46.P2.T3 — Refresh annotations lazily on an idle-priority pass, for dirty visible nodes only
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph.cpp`
  - approach:
    - In `Node::onEffectKnobValueChanged` (`Node.cpp:6123`), emit a new `annotationInputsChanged()` for every knob change. Keep the existing per-knob branches.
    - Emit it also from input connect/disconnect, so the mask readout follows the mask pipe. Renames already go through `onInternalNameChanged`, which also marks the node dirty and drops its cached Python node object (P1.T5).
    - Coalesce per `NodeGraph`, not per `NodeGui`, so one GIL hold covers the whole batch:
      - `NodeGui` connects the signal queued to `scheduleAnnotationRefresh()`, which adds the node to `NodeGraph::_annotationDirty` (a set of weak refs) and arms the graph's idle pass (below). Marking dirty does no evaluation.
      - **Idle pass, not a tick.** Nothing runs on a fixed period or per event. The pass is a single-shot timer, restarted by every input event in the graph (mouse, wheel, key) and armed at 150 ms after the last one. It runs only when there is no drag, pan, zoom, rubber band or playback in progress, and posts itself at `Qt::LowEventPriority`, so it yields to all user input. When the pass would run while input is pending, it re-arms instead.
      - **Visible first, off-screen never eagerly.** The pass takes only dirty nodes whose rect intersects the viewport, plus a small margin. Off-screen dirty nodes stay dirty, and are picked up when they scroll into view: the pan and zoom end handlers re-arm the pass. Labels may show stale values until then.
      - Per slice, the graph runs P1.T2 for the dirty visible nodes, using that node's cached Python results (`NodeGui::_annotationPyCache`).
      - Nodes reporting `needsPython` become `PyJob`s, and the graph calls `store->runner().run(jobs, frame, 4ms, &out)` once.
      - It writes the results into each node's cache, re-runs P1.T2 for those nodes (pure C++, cheap), and calls `setNameItemHtml` once per node.
      - The whole slice, C++ evaluation plus Python, is capped at 4 ms. Nodes left over the budget stay dirty, and the pass re-arms through the same idle path.
    - Frame changes: connect the app timeline's `frameChanged` once per `NodeGraph`. Mark dirty only the NodeGuis whose last evaluation reported `frameDependent`.
    - While a pass runs, `scheduleAnnotationRefresh` from the node currently being evaluated is ignored (a guard set around each job), so a readout function that sets knobs can't loop.
    - During playback and interaction no annotation evaluates at all, template or Python. Frame-dependent nodes are marked dirty, and one idle pass runs after playback or the interaction stops. While a project is loading, or `getApp()->isCreatingPythonGroup()` is true, the Python pass is skipped and re-armed.
    - GIL contention: CPython 3.13 has no try-acquire, and `USE_NATRON_GIL` is not defined, so `PythonGILLocker` is a plain `PyGILState_Ensure`. A render thread evaluating an expression can delay the pass by up to the switch interval (5 ms). The 4 ms budget and the playback throttle bound the cost on our side. No other mitigation is planned; document this.
    - Skip Dot, Backdrop, Viewer and multi-instance children.
  - verify: P5.T1 checks that:
    - editing Blur size updates the label once;
    - scrubbing frames on a node with an animated size updates it;
    - connecting a mask pipe adds `M`;
    - a Python line updates once per knob edit;
    - a Python readout function that sets a knob settles after one pass (pass count logged);
    - a 200-node graph, every node with a `refresh: frame` Python line, keeps each slice under 4 ms (per-slice time logged), runs no slice while dragging or playing, and evaluates off-screen nodes only once they scroll into view (counter logged).
  - size: L

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
      - the sublabel, or for Merge-style nodes `mergeFlowLine` from P1.T3 in place of the operator;
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
  - approach: Port `labelmaker_deoverlap.py` (178 lines) as `std::vector<std::pair<id, QPointF>> planDeoverlap(const std::vector<DeoverlapRect>&, DeoverlapMode mode, const std::set<id>* seeds = nullptr)`:
    - `DeoverlapRect { id; QRectF bare; QRectF full; }`. `bare` is the node box without annotation lines, and `full` is the box with them. `enum class DeoverlapMode { All, AnnotationOnly }`;
    - sweep top to bottom;
    - a node overlapping a node above it is pushed down by the overlap plus `NodeGui::DEFAULT_OFFSET_BETWEEN_NODES`, and pushes cascade;
    - side-by-side nodes at the same height are untouched;
    - backdrops and viewers are skipped;
    - `AnnotationOnly` (used by the auto-push, P4.T3) resolves only overlaps caused by annotation growth: node A's `full` rect intersects a node B below it, while A's `bare` rect does not intersect B. Overlaps the user made between bare boxes are left alone. With `seeds`, the sweep starts only from those nodes (the ones that just grew), and cascades still follow;
    - `All` (used by De-overlap All, P4.T2) resolves every overlap using `full` rects;
    - nothing is ever moved up.
  - verify: `ctest -R NodeGraphDeoverlap` covers: a two-node overlap, a three-node cascade, side-by-side left alone, a backdrop ignored, a bare-box overlap left alone in `AnnotationOnly` mode but resolved in `All`, a seeded `AnnotationOnly` run touching only the seed's cascade, idempotence (a second run on the output plans no moves), and no upward moves.
  - size: M

- [ ] M46.P4.T2 — Add "De-overlap All Nodes" as an undoable graph command
  - files: `Gui/NodeGraph35.cpp`, `Gui/NodeGraph45.cpp`, `Gui/NodeGraph.h`, `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`
  - approach:
    - Add a graph Edit-menu action with a registered (unbound) shortcut ID. It runs P4.T1 over the graph's visible nodes and pushes one `MoveMultipleNodesCommand` with the text "De-overlap nodes".
    - This is the only de-overlap that goes on the undo stack. Automatic pushes are P4.T3.
  - verify: P5.T1 makes three overlapping nodes, runs the action (they no longer overlap), presses Ctrl+Z (they're back exactly), then Ctrl+Shift+Z.
  - size: M

- [ ] M46.P4.T3 — Push nodes down automatically when an annotation grows, off the undo stack, and re-settle after undo/redo
  - files: `Gui/NodeGraph.h`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph.cpp`, `Gui/NodeGui.h`, `Gui/NodeGui.cpp`
  - approach:
    - The user chose Labelmaker's behaviour knowingly: hidden moves that push no command. Gate it on the P2.T1 preference "Push nodes down when an annotation grows" (default on).
    - `NodeGui` keeps its last `full` height. When `setNameItemHtml` makes the node taller (shrinking never pulls nodes back up), it adds the node to `NodeGraph::_pushSeeds`. The graph handles the seeds once per P2.T3 pass, after all labels in that pass are updated:
      - it runs `planDeoverlap(rects, AnnotationOnly, &seeds)` over the visible nodes of that graph level;
      - it applies the moves with plain `NodeGui::setPosition` and pushes **no** undo command;
      - it calls `triggerAutoSave()` as other moves do.
    - When it does not run: during project load, config reload, toggling "Show node annotations", and the playback throttle window. These can grow hundreds of labels at once, and opening a saved project must not move its nodes. The user runs De-overlap All (P4.T2) for those cases. Only growth after a user edit (a knob, input or label change, or a frame change while not playing) pushes.
    - Undo interaction, decided:
      - `MoveMultipleNodesCommand` stores **deltas** (`NodeGraphUndoRedo.cpp:75`), so undoing an earlier move shifts nodes relative to where the hidden pushes left them. The pushes survive and nothing jumps back on top.
      - Commands that restore **absolute** positions (`RearrangeNodesCommand`, `NodeGraphUndoRedo.cpp:1057`, plus the splice, extract and group commands) can put a node back inside a grown label.
      - So after every undo/redo on the graph's undo stack (`QUndoStack::indexChanged`, coalesced into the next pass), the graph runs a settle pass: `planDeoverlap(rects, AnnotationOnly)`, unseeded, off-stack.
      - Because `AnnotationOnly` only fixes annotation-caused overlaps and is idempotent, re-settling after undo/redo is stable and never fights an undo. Overlaps the user made on purpose between bare boxes are never touched. Redo of an absolute command followed by a settle reaches the same layout every time.
      - Document the limit: a hidden push is not itself undoable. Ctrl+Z undoes the user's action, and the push reappears only if the label grows again.
    - Skip Backdrops and Viewers. Never move a node the user is currently dragging; defer until the mouse is released.
  - verify: P5.T1 checks that:
    - setting Blur size on a node with another node just below pushes that node down, and the undo stack gains only the knob edit;
    - with the preference off, there is no push;
    - opening a saved project with tightly packed nodes moves nothing;
    - after a push, "Rearrange nodes" then Ctrl+Z leaves no annotation overlap (the settle ran), and Ctrl+Shift+Z gives the same layout as the first redo;
    - an earlier user move undone after a push lands without overlap;
    - two nodes the user overlapped on purpose stay overlapped through all of the above.
  - size: L

## Phase 46.5: Tests and UAT

- [ ] M46.P5.T1 — Write a GUI test script for annotations
  - files: `Tests/gui/m46_annotations.py` (new)
  - approach: Model it on `Tests/gui/m55_node_graph.py`. Cover each verify bullet in P2.T1–P4.T3, including the Python cases:
    - a `python` line from the personal config (its module written to a temp dir on `sys.path`);
    - an error marker after editing the function to raise, which clears after a fixed function and "Reload Node Annotations";
    - the slow-function auto-disable.
    - Read the label text from the NodeGui's text item (the `QGraphicsTextItem` under the node in the scene) rather than from pixels where possible. Point the personal config at a temp file through the preference. Take a shot of every case, plus a busy-graph overview before and after "De-overlap All", and before and after an auto-push.
  - verify: `NATRON_GUI_TEST_OUT=build/m46-shots Tests/gui/run-gui-test.sh Tests/gui/m46_annotations.py build/fast/App/Natron` reports 0 failures at the gate build.
  - size: L

- [ ] M46.P5.T3 — Hold M118's budgets with annotations on
  - files: `tools/bench/gui/gen_graph.py`, `tools/bench/gui/gui_bench.py`, `tools/bench/gui/BASELINE.md`
  - approach: add an `annotations=True` option to the M117 generator, which writes a personal config covering every generated plugin, with one Python readout per tenth node. Run the Xvfb GUI bench at N=3000 `mixed` with annotations on and off. Compare both runs against M118 - Large Graph GUI Responsiveness's Xvfb baseline, and ask the user for a real-host run with annotations on.
  - verify: with annotations on, `compare.py` against the M118 Xvfb baseline exits 0. No stall over 250 ms is attributed to an annotation pass. The user's host run meets M118's budgets.
  - size: M
- [ ] M46.P5.T2 — Run the one build, the tests, and package the AppImage for UAT
  - files: none
  - approach: re-run cmake (new globbed files and the qrc), build, run `ctest`, then `m46_annotations.py`, `m24_border.py`, `m55_node_graph.py` and M56's wrap script. Package `build/appimages/M46-<sha>.AppImage`.
  - verify: everything is green, and the AppImage is posted for UAT.
  - size: M

**Verification gate:** CI `format`, `lint-ci` and `build-and-test` are green, including the `NodeAnnotationConfig`, `NodeAnnotation`, `NodeAnnotationBaseConfig`, `NodeAnnotationHtml`, `NodeAnnotationPython` and `NodeGraphDeoverlap` tests. The gate report records the measured `NodeAnnotationPython` 200-job time and the P2.T3 per-slice time, and P5.T3's GUI bench comparison against M118's Xvfb baseline plus the user's host run within M118's budgets. `Tests/gui/m46_annotations.py`, `m24_border.py` and `m55_node_graph.py` report 0 failures on the gate build. The user signs off the shots and the AppImage UAT, which covers: readouts appearing and disappearing with defaults, swatches, the channel/mask/mix line, "Class | name", Read/Write file names, a live personal-config reload, a Python readout line with its error marker, the preference off giving the old labels, the auto-push (and its preference off), undo/redo after a push leaving no annotation overlap, and an undoable De-overlap All.

Acceptance sketch:
- Nodes can display rich, Labelmaker-style annotation text/data on the node
  graph.

Scheduling note (2026-10-08, user): runs together with M55. Before elaborating, evaluate whether Labelmaker-style node-graph info belongs inside node-graph polish; elaborate them as one milestone or two accordingly.

## Decisions

- 2026-10-11 (user):
  - **Licence:** the user wrote Labelmaker and relicenses its config schema and base config under GPL-2.0-or-later, so translating `base_config.json` (P1.T4) is fine.
  - **Python readouts:** batched, not per line. A config line may give `"python": "module.func"`, resolved once per config load and called with `(thisNode, frame)` in one GIL hold per idle slice, with a 4 ms budget and per-node cached results (P1.T5, P2.T3). No init.py registration API.
  - **Both Python limits accepted, and documented:** no timeout (a hanging function hangs the UI; only calls over 50 ms are auto-disabled), and no cross-node dependency tracking (refresh on own-node changes, `refresh: frame`, or "Reload Node Annotations").
  - **No config editor** in M46; users edit the JSON.
  - **Auto-push on, off the undo stack**, as Labelmaker does, with a preference to turn it off (P4.T3). Undo interaction: `MoveMultipleNodesCommand` stores deltas, so pushes survive its undo. Commands that restore absolute positions are followed by an idempotent, off-stack `AnnotationOnly` settle after every undo/redo. Hidden pushes are themselves not undoable. Manual "De-overlap All" stays undoable (P4.T2).
  - **The summary line replaces** the layer summary when annotations are on (P3.T2).
  - **Personal config lives next to the PyPlugs:** `~/.Natron/node_annotations.json` by default, plus a `node_annotations.json` in any PyPlug search directory, layered (P2.T1, P2.T2). Not `AppConfigLocation`.
  - **Deps:** `M56` (Phase 56.1 width cap and wrap).
- 2026-10-13 — Annotations evaluate lazily, never per tick (user, during the M118 - Large Graph GUI Responsiveness grilling): "should be evaluated lazily, low prio idle process, stale labels are acceptable but gui performance issues are not." P2.T3 now runs an input-debounced, low-priority idle pass over dirty visible nodes only, with no evaluation during playback or interaction. P5.T3 holds M118's bench budgets with annotations on.
- 2026-10-13 — Deps are now M56 and M118 - Large Graph GUI Responsiveness (user): the push-down layout builds on M118's local stacking and indexed scene, and M46 must pass the GUI bench.
- 2026-10-27 — Merge, ChannelCopy and Shuffle get a built-in channel-flow readout (user, `/cat-grill`): a port means every Labelmaker feature, and templates can't read the flow reliably. P1.T3 adds `mergeFlowLine`, and P3.T2 uses it in place of the operator sublabel.
- 2026-10-27 — On hold, not eligible for pickup (user, during the grilling): the board row is `blocked` with a blank Owner, so no PM can claim it. The reason is in the board's `# Open questions`.
