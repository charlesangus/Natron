# M45 - Native Tab Menu

Full title: Lift tabtabtab-nuke as the native tab menu

Replace Natron's node-creation dialog (`Gui/NodeCreationDialog.*`, a frameless `QDialog` around a `CompleterLineEdit` that matches `*a*b*c*` wildcards over plugin names) with tabtabtab itself. The code is vendored from github.com/charlesangus/tabtabtab-nuke (Unlicense) at commit `48de32d`. Its core module is already host-agnostic and PySide6-first. Natron gets a new adapter in place of the Nuke one, plus a thin C++ bridge that the node graph's Tab handler calls. The C++ reimplementation planned earlier (phases 45.1 to 45.4) is cancelled. The menu brings:
- anchored fuzzy search, with leading-space prefixes for the other search modes and `[` to search the category;
- ranking by consecutive match first, then usage weight, then name;
- per-category colour blocks (from M24), plugin icons and shortcuts in each row;
- main-menu commands in the same list;
- Tab-Tab to repeat the last node;
- a correct position the first time it opens (this takes over M69's "Tab menu position" item);
- insertion after the selected node (today's behaviour, kept);
- tabtabtab's preferences (the search-mode remap and the scroll toggle) as a "Tab menu" section on the Nodegraph page of Natron's Preferences (M45.P6.T5). The adapter reads them on every open, so tabtabtab's own prefs JSON and dialog are not used or vendored.

Layout:
- The vendored files live in `Gui/Resources/TabTabTab/`. They ship to `<bin>/../Resources/tabtabtab/`, through a post-build copy for dev builds and `stage-bundle.sh` for the AppImage.
- Vendored files stay byte-identical to upstream apart from the one patch in M45.P5.T2, which is listed in `UPSTREAM`.

One build at the end (user rule): implementers do not compile. The pure-Python tests (M45.P5.T2, M45.P5.T4) need no build, and implementers may run them with the host `python3`.

## Phase 45.1: Matching, ranking and items (cancelled)

- [ ] ~~M45.P1.T1 — Add a pure tab-menu matcher and ranker, with unit tests~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user)
- [ ] ~~M45.P1.T2 — Build the tab-menu item list from the plugin registry, with category colour, icon, shortcut and weight~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; its item-list and category-colour logic moves to M45.P6.T1/T2)
- [ ] ~~M45.P1.T3 — Add the Tab menu preferences (search-mode remap, scroll toggle) to the Nodegraph page, with accessors~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; its Settings knobs are reinstated, without the C++ accessors, as M45.P6.T5)

## Phase 45.2: The palette (cancelled)

- [ ] ~~M45.P2.T1 — Write the TabMenu popup: line edit, coloured result list, keys, and correct first-show position~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user)
- [ ] ~~M45.P2.T2 — Wire the TabMenu into the node graph and honour the rebindable shortcut~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; superseded by M45.P6.T3)
- [ ] ~~M45.P2.T3 — Remove NodeCreationDialog, keeping CompleterLineEdit for its other users~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; carried over as M45.P6.T4)

## Phase 45.3: Smart insertion and menu commands (cancelled)

- [ ] ~~M45.P3.T1 — With nothing selected, splice the new node into the pipe under the cursor~~ (cancelled 2026-10-11 by the user: insert-after-selected only)
- [ ] ~~M45.P3.T2 — Add main-menu commands to the palette~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; tabtabtab's `find_qt_menu_items` does it, wired in M45.P5.T3)

## Phase 45.4: Tests and UAT (cancelled)

- [ ] ~~M45.P4.T1 — Write a GUI test script for the tab menu~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; superseded by M45.P8.T1)
- [ ] ~~M45.P4.T2 — Run the one build, the tests, and package the AppImage for UAT~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; superseded by M45.P8.T3)

## Phase 45.5: Vendor tabtabtab and write the Natron adapter

- [x] M45.P5.T1 — Vendor tabtabtab-nuke's core, core tests and licence at a pinned commit
  - files: `Gui/Resources/TabTabTab/` (new): `tabtabtab_nuke_core.py`, `LICENSE`, `UPSTREAM`, `tests/__init__.py`, `tests/test_node_model_diff.py`, `tests/test_monitor_selection.py`, `tests/test_main_window_discovery.py`, `tests/test_cursor_restore_on_close.py`
  - approach:
    - `git clone https://github.com/charlesangus/tabtabtab-nuke` and `git checkout 48de32d`. If the user names a newer tag, use that instead and record it.
    - Copy the files above byte for byte. Keep the upstream file names: the modules import each other by name, and renaming them would turn the vendored copy into a fork.
    - Do not vendor:
      - `tabtabtab_nuke.py` and `menu.py` (the Nuke adapter, replaced by M45.P5.T3);
      - `tabtabtab_prefs.py` and `tabtabtab_prefs_dialog.py`. The preferences live in Natron's Settings (M45.P6.T5), and the core never imports these two files, so leaving them out means no patch to the dialog's Nuke import;
      - `tests/test_menu_fingerprint.py` (it tests the Nuke adapter);
      - `readme.md`, `docs/`, `Makefile`, `.github/`.
    - `UPSTREAM` is plain text: the source URL, the commit hash, the date, the list of files taken, the list of files left out, and a "Local patches" heading that M45.P5.T2 fills in.
    - These are vendored third-party files, so the project comment policy does not apply to them. Do not reformat or re-comment them.
  - verify: run `cd Gui/Resources/TabTabTab && python3 -m pytest -q tests` (host python, no build). All the vendored tests pass; upstream reports 38 passing, minus the fingerprint tests that were left out. Run `diff -r` against the clone, restricted to the vendored files; it shows no differences.
  - size: S

- [x] M45.P5.T2 — Patch the vendored core so the host's main window is pluggable (one small patch that can go upstream)
  - files: `Gui/Resources/TabTabTab/tabtabtab_nuke_core.py`, `Gui/Resources/TabTabTab/UPSTREAM`, `Gui/Resources/TabTabTab/tests/test_host_hooks.py` (new)
  - approach:
    - The patch, a main-window finder:
      - Add a module global `_main_window_finder = None`, a public `set_main_window_finder(fn)`, and `_find_main_window()`, which returns `(_main_window_finder or _find_nuke_main_window)()`. The late lookup is deliberate: the upstream tests rely on module-global name resolution.
      - Replace the three `_find_nuke_main_window()` calls, in `_try_reparent_preloaded`, `_create_tabtabtab_widget` and `launch`, with `_find_main_window()`.
      - Leave `_find_nuke_main_window` itself unchanged.
    - The earlier second patch (an optional Enable toggle in the prefs dialog) was dropped on 2026-10-12, because the dialog is no longer vendored.
    - Record the patch under "Local patches" in `UPSTREAM` with a one-line description (user, 2026-10-12: patch the vendored copy and record it there).
    - Keep upstream's style in the patched lines: 4-space indent and docstrings where upstream has them.
  - verify:
    - `python3 -m pytest -q tests` in `Gui/Resources/TabTabTab` still passes, including `test_main_window_discovery.py`, unmodified.
    - A new test file in the same folder, `tests/test_host_hooks.py`, uses upstream's stub pattern from `test_main_window_discovery.py`. It checks that:
      - with no finder set, `_find_main_window()` returns the Nuke finder's result;
      - after `set_main_window_finder(lambda: sentinel)`, `_create_tabtabtab_widget` passes `sentinel` as the parent.
    - `test_host_hooks.py` is the one Natron-added file in `tests/`; `UPSTREAM` lists it.
  - size: M

- [x] M45.P5.T3 — Write the Natron adapter `tabtabtab_natron.py`
  - files: `Gui/Resources/TabTabTab/tabtabtab_natron.py` (new)
  - approach:
    - This replaces `tabtabtab_nuke.py`, and is modelled on its `NukePlugin`. Imports: `tabtabtab_nuke_core` (`TabTabTabPlugin`, `launch`, `find_qt_menu_items`, `set_main_window_finder`, `DEFAULT_SPACE_MODE_ORDER`), `NatronGui`, and `PySide6` (`QtCore`, `QtGui`, `QtWidgets`). It does not import `tabtabtab_prefs` or the prefs dialog, which are not vendored.
    - Weights storage:
      - The weights path defaults to `os.path.expanduser("~/.Natron/tabtabtab_weights.json")`.
      - `set_storage_dir(path)` points it at `os.path.join(path, "tabtabtab_weights.json")` instead. The GUI test calls it with a scratch folder.
    - Preferences, read from Natron's Settings (M45.P6.T5) on every `launch`:
      - Get `NatronGui.natron.getSettings()` (`PyCoreApplication::getSettings`, `PyGlobalFunctions.h:219`). Its `getParam(name)` returns a typed wrapper (`AppSettings::getParam`, `PyAppInstance.cpp:247`).
      - `tabMenuSearchModes` is a `ChoiceParam`. `p.getOption(p.getValue())` returns the option ID (`ChoiceParam::getOption`, `PyParameter.cpp:1811`), and the option IDs are tabtabtab's own mode names joined by commas. So `space_mode_order = p.getOption(p.getValue()).split(",")`, with no mapping table on the Python side.
      - `tabMenuScroll` is a `BooleanParam`; `scroll_enabled = p.getValue()`.
      - If a param is missing (`None`) or anything raises, fall back to `DEFAULT_SPACE_MODE_ORDER` and `True`. The core also validates the order itself.
    - Main window:
      - `_find_natron_main_window()` takes `QApplication.activeWindow()` and walks up `parentWidget()` to the first widget whose `metaObject().className() == "Natron::Gui"`. This also covers a node graph in a floating pane.
      - If there is none, it returns the first top-level `QMainWindow` with that class name, or `None`.
      - Register it with `set_main_window_finder` at import.
    - `class NatronPlugin(TabTabTabPlugin)`:
      - `set_app(app)` stores the `GuiApp` that the bridge passes in.
      - `get_items()` returns node items and then command items:
        - Node items come from `json.loads(app.getTabMenuNodeItems())` (M45.P6.T2). Each entry has `id`, `major`, `label`, `grouping` (a list), `icon` (a path or ""), `color` ("#rrggbb" or ""), and `shortcut` (text or "").
        - A node item's `menupath` is `"/".join(grouping) + "/" + label`. Replace any "/" inside `label` or inside a grouping element with "∕" (U+2215), so the core's `rpartition("/")` splits in the right place.
        - A node item's `menuobj` is a small `_NodeEntry` holding id, major, icon and colour; `shortcut` is set only when it is non-empty.
        - Command items come from the core's `find_qt_menu_items(window.menuBar())` on the window from `_find_natron_main_window()`, or an empty list if there is no window. Add `shortcut = action.shortcut().toString(QtGui.QKeySequence.NativeText)` when it is non-empty.
        - If two items end up with the same `menupath`, append " (2)", " (3)" and so on to the later ones, because the core's row diff is keyed on `menupath`.
        - Do not cache anything. The node list is rebuilt from C++ on every call, so a category-colour edit (M24) or a shortcut change shows up on the next open.
      - `get_weights_file()` returns the weights path. Weights start fresh; there is no seeding from the old `QSettings` counts (user, 2026-10-12).
      - `invoke(thing)` must not act synchronously: the core calls `invoke` before it closes the popup, and both creating a Read (a modal file dialog) and menu actions (Preferences) open windows. Keep a reference to the callable and `QtCore.QTimer.singleShot(0, ...)` it:
        - for a `_NodeEntry`, call `app.createNodeFromTabMenu(id, major)`;
        - for a `QAction`, `action.trigger()` if the action still exists. Guard with `shiboken6.isValid` and catch `RuntimeError`.
      - `get_icon(menuobj)`:
        - for a `_NodeEntry`, a `QIcon(path)` cached per path, or `None` if the path is empty or missing;
        - for a `QAction`, the base-class default (`menuobj.icon()`).
      - `get_color(menuobj)`: for a `_NodeEntry` with a colour, `(QColor(c), QColor(c))`, which is the Nuke adapter's convention; otherwise `(None, None)`.
    - Module entry points:
      - `launch(app)`: calls `_plugin.set_app(app)`, reads the two preferences as above, then calls the core's `launch(_plugin, space_mode_order=..., scroll_enabled=...)`. The core's warm-instance path re-applies both on every call, so a change in Preferences applies on the next open.
      - There is no `show_prefs_dialog` and no Edit-menu entry. The preferences are on Natron's Nodegraph page.
    - Do not port `schedule_preload`: the widget is built on the first Tab, and the item build is cheap.
    - The comment policy applies to this file, which is new Natron code.
  - verify: M45.P5.T4's unit tests at any time (no build); M45.P8.T1 at the gate.
  - size: L

- [x] M45.P5.T4 — Unit-test the adapter with stubbed PySide6 and NatronGui, and register the Python tests with ctest
  - files: `Tests/python/test_tabtabtab_natron.py` (new), `Tests/python/run_tabtabtab_tests.py` (new), `Tests/CMakeLists.txt`
  - approach:
    - `test_tabtabtab_natron.py` is a stdlib `unittest` module. It loads the adapter by path, using the stub-module pattern of upstream `tests/test_main_window_discovery.py`: `PySide6`/`QtCore`/`QtGui`/`QtWidgets`/`shiboken6` stubs in `sys.modules`, restored afterwards.
    - `QTimer.singleShot` is stubbed so that it records calls. The `app` is a fake with canned `getTabMenuNodeItems` JSON and a recording `createNodeFromTabMenu`. A `NatronGui` stub provides `natron.getSettings().getParam(name)`, returning fakes with `getValue`/`getOption`, or `None`.
    - Cases:
      - node `menupath` and display formatting ("Blur [Filter]");
      - a "/" in a label is replaced;
      - a duplicate menupath gets " (2)";
      - command items are appended, with shortcut text;
      - `get_color` parses "#rrggbb" and returns `(None, None)` for commands and for an empty colour;
      - `invoke` on a node schedules exactly one call and does not call `createNodeFromTabMenu` until the recorded callback runs;
      - `invoke` on an action schedules `trigger`;
      - `launch` passes `space_mode_order == ["consecutive", "anchored_fuzzy", "non_anchored_fuzzy"]` and `scroll_enabled=False` to the stubbed core `launch` when the fake settings hold that option ID and False;
      - `launch` falls back to `DEFAULT_SPACE_MODE_ORDER` and `True` when `getParam` returns `None`;
      - `get_weights_file` returns the default path when the file is absent, and creates nothing;
      - `set_storage_dir` repoints the weights path;
      - `_find_natron_main_window` prefers the `Natron::Gui` ancestor of the active window, and falls back to the top-level scan.
    - `run_tabtabtab_tests.py` takes no arguments:
      - it puts `Gui/Resources/TabTabTab` and `Tests/python` on `sys.path`;
      - it imports each `test_*.py` in `Gui/Resources/TabTabTab/tests` and calls every module-level `test_*` callable (upstream's tests are plain pytest-style functions with no fixtures), then runs `unittest` discovery on `Tests/python`;
      - it prints one line per failure and exits non-zero if anything fails.
      - This means ctest does not need pytest in the CI container.
    - `Tests/CMakeLists.txt`: inside the existing `if(Python3_Interpreter_FOUND)` block, beside `RenderProfileReportSelfTest`, add `add_test(NAME TabTabTabPython COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/python/run_tabtabtab_tests.py)`.
  - verify: `python3 Tests/python/run_tabtabtab_tests.py` exits 0 on the host (no build). At the gate, `ctest -R TabTabTabPython` passes.
  - size: M

## Phase 45.6: The C++ bridge

- [x] M45.P6.T1 — Resolve a plugin's node category without an instance
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Tests/NodeCategory_Test.cpp`
  - approach:
    - Add `static NodeCategoryEnum Node::categoryForPlugin(const Plugin& plugin)`, declared beside `domainCategoryFromGrouping`/`categoryFromGroupingAndLabel` (`Node.h:797-808`). It mirrors `Node::getNodeCategory()` (`Node.cpp:4402-4432`) without an effect instance:
      - `domainCategoryFromGrouping` on the plugin's grouping (converted to `std::list<std::string>`);
      - then `plugin.isReader()` → Read and `plugin.isWriter()` → Write;
      - then `categoryFromGroupingAndLabel(first grouping, plugin label)`.
    - Generators can't be detected without an instance, so they fall through to the grouping heuristic. This is the user's decision ("generators coloured by grouping"). Say so in one comment line on the declaration.
  - verify: `ctest -R NodeCategory`. New cases show that `categoryForPlugin(*node->getPlugin())` equals `node->getNodeCategory()` for a Read, a Write, a Deep node, a Grade, a Blur and a Merge created in the test app.
  - size: M

- [x] M45.P6.T2 — Write `TabMenuBridge`: the node item list, node creation, launch and startup bootstrap, exposed on `GuiApp`
  - files: `Gui/TabMenuBridge.h` (new), `Gui/TabMenuBridge.cpp` (new), `Gui/PyGuiApp.h`, `Gui/PyGuiApp.cpp`
  - approach:
    - `TabMenuBridge` is a plain namespace of free functions with file-static state `QPointer<NodeGraph> g_target` and `QPointF g_scenePos`. It is GUI-thread only, and only one popup exists at a time.
    - `QString nodeItemsJson()` returns a `QJsonDocument` array. Move the list-building rules from the `NodeCreationDialog` constructor (`Gui/NodeCreationDialog.cpp:370-440`) into it:
      - walk `appPTR->getPluginsList()` and skip `!getIsUserCreatable()`;
      - for multi-version plugins, the newest version's label is `getLabelWithoutSuffix()` and older ones use `getLabelVersionMajorEncoded()`;
      - PyPlugs and toolsets stay in the list.
      - Each object has:
        - `id` and `major`;
        - `label`;
        - `grouping`, the full `getGrouping()` list;
        - `icon`: `getIconFilePath()` if `QFile::exists`, otherwise "";
        - `color`: `Settings::getNodeCategoryColor(Node::categoryForPlugin(*p), &r, &g, &b)` (`Settings.h:222`) as `QColor::fromRgbF(...).name()`;
        - `shortcut`: the first entry of `getKeybind(group, pluginID)` (the macro at `GuiApplicationManager.h:63`) as `toString(QKeySequence::NativeText)`, or "". `group` is `kShortcutGroupNodes` followed by `"/" + g` for each grouping element; this is how `GuiApplicationManager::onPluginLoaded` (`GuiApplicationManager.cpp:990-1047`) registers it. Note that `Plugin::getPluginShortcutGroup()` is the viewer-interface group, not this one.
    - There is no legacy-weights function: weights start fresh (user, 2026-10-12). The old `<id><major>_tab_weight` `QSettings` keys are neither read nor written any more.
    - `bool createNode(const QString& pluginID, int major)`:
      - returns false if `g_target` is null;
      - otherwise does the body of today's `NodeGraph::onCreateNodeDialogFinished` (`NodeGraph25.cpp:202-225`): `CreateNodeArgs(id, g_target->getGroup())`, `kCreateNodeArgsPropNodeInitialPosition` = `g_scenePos`, `kCreateNodeArgsPropPluginVersion` = major, then `g_target->getGui()->getApp()->createNode(args)`.
      - The default args keep today's auto-connect to the selected node, the undo entry and the open settings panel. This is the insert-after-selected decision.
    - `void launch(NodeGraph* graph)`:
      - sets `g_target = graph` and `g_scenePos = graph->mapToScene(graph->mapFromGlobal(QCursor::pos()))`. This is the position when Tab was pressed; the old code read the cursor after the dialog closed.
      - then runs `NATRON_PYTHON_NAMESPACE::interpretPythonScript("tabtabtab_natron.launch(" + appIDString + ")\n", &err, &out)`. `appIDString` comes from `getApp()->getAppIDString()` and names the `GuiApp` global (`app1`, ...).
      - On failure it appends `err` to the script editor and writes it to `std::cerr`. There is no fallback dialog (Decisions).
    - `void bootstrap()`:
      - computes `dir = QCoreApplication::applicationDirPath() + "/../Resources/tabtabtab"`, made canonical;
      - if `dir` is missing, prints one error line naming it, and returns;
      - otherwise runs `import sys` and `sys.path.insert(0, r'<dir>')` (only if it is not there yet), then `import tabtabtab_natron`, through `interpretPythonScript` in `__main__`.
      - It adds no menu entry (user, 2026-10-12: the preferences live in Natron's Preferences), and it does not touch `Gui.cpp`, which M56 is also editing.
    - Bindings: on `GuiApp` (`PyGuiApp.h`, after `renderBlocking`), add:
      - `QString getTabMenuNodeItems() const`
      - `bool createNodeFromTabMenu(const QString& pluginID, int majorVersion)`
      - Each forwards to `TabMenuBridge`.
      - These are plain `QString`/`int`/`bool` signatures on an existing `object-type`, so shiboken generates them with no `typesystem_natronGui.xml` change and no `sourceList.py` change. Do not include `TabMenuBridge.h` from `PyGuiApp.h`; include it from `PyGuiApp.cpp` only.
  - verify: at the gate, M45.P8.T1 exercises every function through the live menu. Also, `python -c` in the built Natron's script editor (by hand in the UAT): `json.loads(app1.getTabMenuNodeItems())` lists Blur with a non-empty colour.
  - size: L

- [x] M45.P6.T3 — Route the node graph's create-node shortcut and right-click "Create Node" to the bridge, and run the bootstrap at startup
  - files: `Gui/NodeGraph25.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphPrivate.cpp`, `Gui/GuiApplicationManager10.cpp`
  - approach:
    - In `NodeGraph::event` (`NodeGraph25.cpp:169-187`), replace `ke->key() == Qt::Key_Tab` with `isKeybind(kShortcutGroupNodegraph, kShortcutIDActionGraphCreateNode, ke->modifiers(), (Qt::Key)ke->key())`, so a rebinding in the shortcut editor takes effect. It must stay in `event()`, because `QGraphicsView` takes Tab for focus traversal before `keyPressEvent`. Keep the `_nodeCreationShortcutEnabled` gate (`NodeGraph30.cpp:141-156`) and `takeClickFocus()`.
    - `NodeGraph::popCreateNodeDialog()` becomes `TabMenuBridge::launch(this)`. Keep the name, so the right-click menu (`NodeGraph35.cpp:491`) needs no change.
    - Delete `onCreateNodeDialogFinished` (its slot declaration at `NodeGraph.h:222` and its body) and `_lastNodeCreatedName` (`NodeGraphPrivate.h:172`, `NodeGraphPrivate.cpp:64`). tabtabtab's line edit keeps the last choice.
    - Delete the unreachable branch and its `assert(false)` in `keyPressEvent` (`NodeGraph25.cpp:386-388`).
    - Drop `#include "Gui/NodeCreationDialog.h"` from `NodeGraph25.cpp` and include `Gui/TabMenuBridge.h`.
    - In `GuiApplicationManager::onAllPluginsLoaded` (`GuiApplicationManager10.cpp:340`), call `TabMenuBridge::bootstrap()` after `AppManager::onAllPluginsLoaded()`. This is after `loadPythonGroups` and before any `Gui` builds its menus (`Gui.cpp:765`); confirm that order when reading `GuiApplicationManager::initGui`.
  - verify: at the gate:
    - Tab over the graph opens tabtabtab, and Blur is created at the cursor and connected below the selected Read (M45.P8.T1).
    - After rebinding Create Node in the shortcut editor, the new key opens it and Tab does not (by hand in the UAT).
  - size: M

- [x] M45.P6.T4 — Remove NodeCreationDialog, keeping CompleterLineEdit for its other users
  - files: `Gui/NodeCreationDialog.h`, `Gui/NodeCreationDialog.cpp`, `Gui/Resources/Stylesheets/mainstyle.qss`
  - approach:
    - Delete the `NodeCreationDialog` class, its private struct, and the `getPluginWeight`/`incrementPluginWeight` statics (weights start fresh in tabtabtab's JSON, so nothing replaces them).
    - Keep `CompleterLineEdit` in the same files, because `PickKnobDialog.cpp`, `LinkToKnobDialog.cpp`, `KnobGuiPrivate.h`, `EditScriptDialog.cpp` and `EditExpressionDialog.cpp` include it.
    - In `CompleterLineEdit`, drop the `dynamic_cast<NodeCreationDialog*>` branches in `setTextFromIndex` and `keyPressEvent`, so quick-exit always calls `_imp->dialog->accept()`. Leave the list-positioning code alone.
    - Remove the `NodeCreationDialog, Natron--NodeCreationDialog` rule (`mainstyle.qss:321-326`).
  - verify: at the gate build, `grep -rn NodeCreationDialog Gui/` finds nothing, and the Pick Knob and Link-to-Knob dialogs still complete node names (by hand in the UAT).
  - size: M

- [x] M45.P6.T5 — Add the "Tab menu" preferences (search-mode remap, scroll toggle) to the Nodegraph page
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Tests/TabMenuSettings_Test.cpp` (new), `Tests/CMakeLists.txt` (one line in the `Tests` binary's source list, beside `NodeCategory_Test.cpp`)
  - approach:
    - This reuses the cancelled M45.P1.T3 brief, with two changes: the option IDs are tabtabtab's mode names, and there are no C++ accessors, because only the Python adapter reads these knobs (M45.P5.T3).
    - Keep every edit additive, because M56 - Visual And Menu Polish also edits these files in another worktree: it removes `_useBWIcons` from the Nodegraph members and defaults, and adds an accessor near `Settings.h:306`. Append; do not reorder or reformat existing lines.
    - In `Settings::initializeKnobsNodeGraph()`, after `_nodegraphTab->addKnob(_useInputAForMergeAutoConnect);` (`Settings.cpp:1257`), add:
      - `_tabMenuSeparator` (`KnobSeparator`, label "Tab menu", name `tabMenuSeparator`) as the heading;
      - `_tabMenuSearchModes` (`KnobChoice`, label "Search modes (no / one / two leading spaces)", name `tabMenuSearchModes`). It has six `ChoiceOption`s, one per ordering of the three modes:
        - each option's ID is the ordering of tabtabtab's mode names joined by commas, starting with `anchored_fuzzy,non_anchored_fuzzy,consecutive` (these are `tabtabtab_nuke_core.MODE_*`; the adapter splits the ID on ",");
        - each label is like "Anchored fuzzy / Non-anchored fuzzy / Consecutive".
        - A single choice can only hold a valid permutation, so no validation is needed.
        - The tooltip explains that the number of leading spaces in the query picks the mode, and that `[` also searches the category. Reuse tabtabtab's own wording for the three modes (its prefs dialog's `MODE_DESCRIPTIONS`, with "Blur"/"MotionBlur" changed to "Blur"/"DirBlur").
      - `_tabMenuScroll` (`KnobBool`, label "Scroll through all results", name `tabMenuScroll`). Its tooltip says that when it is off, only the first screenful of matches is kept.
      - Add each with `_nodegraphTab->addKnob`.
    - Declare the three `Knob*Ptr` members after `_useAntiAliasing` in the `// Nodegraph` block (`Settings.h:576`).
    - In `setDefaultValues()`, add a separate `// Tab menu` block after `_useAntiAliasing->setDefaultValue(true);` (`Settings.cpp:1611`): choice 0 (`anchored_fuzzy,non_anchored_fuzzy,consecutive`) and scroll `true`. These match tabtabtab's own defaults.
    - Nothing goes in `onKnobValueChanged`: the adapter reads the values on every open.
  - verify: `ctest -R TabMenuSettings` checks, on `appPTR->getCurrentSettings()` (look the knobs up by name with `getKnobByNameAndType`, as `GLScheduler_Test.cpp:114` does, and restore the old values afterwards):
    - the defaults are index 0, whose option ID is `anchored_fuzzy,non_anchored_fuzzy,consecutive`, and scrolling on;
    - the six option IDs are distinct, and each splits into a permutation of the three mode names;
    - `tabMenuScroll` can be turned off.
    - At the gate, M45.P8.T1 checks that a change through `natron.getSettings()` alters the live menu. The page layout (a "Tab menu" heading with both controls) is checked by eye in the UAT.
  - size: M

## Phase 45.7: Shipping the Python files

- [x] M45.P7.T1 — Copy the vendored folder next to the binary in dev builds and into the AppImage
  - files: `App/CMakeLists.txt`, `tools/release/stage-bundle.sh`
  - approach:
    - `App/CMakeLists.txt`, after `target_include_directories(Natron ...)`:
      - `add_custom_command(TARGET Natron POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Gui/Resources/TabTabTab $<TARGET_FILE_DIR:Natron>/../Resources/tabtabtab VERBATIM)`
      - With `build/fast/App/Natron`, that lands in `build/fast/Resources/tabtabtab`, which is where `TabMenuBridge::bootstrap()` looks.
      - The `tests/` subfolder is copied too, which is harmless in a dev tree.
      - Add no `install()` rule, as for PyPlugs, which have none either: an FHS prefix would put it under `/usr/Resources`.
    - `stage-bundle.sh`, beside the PyPlugs line (`:293`): `copy_tree "$SOURCE_DIR/Gui/Resources/TabTabTab" "$STAGE_DIR/Resources/tabtabtab" --exclude='./tests' --exclude='__pycache__'`. Use the same `copy_tree` exclusion syntax as the stdlib call at `:229`. Add a two-line comment naming `TabMenuBridge::bootstrap()` as the consumer.
  - verify: at the gate:
    - `ls build/fast/Resources/tabtabtab/tabtabtab_natron.py` succeeds after the build;
    - the packaged AppImage, extracted, has `Resources/tabtabtab/tabtabtab_nuke_core.py` and no `tests/`;
    - the AppImage's Tab menu works in the UAT.
  - size: S

## Phase 45.8: Tests, the one build, and UAT

- [x] M45.P8.T1 — Write the GUI test script for the tab menu
  - files: `Tests/gui/m45_tab_menu.py` (new)
  - approach:
    - Model it on `Tests/gui/m55_node_graph.py` (watchdog, `gt.check`/`gt.shot`, `create()`).
    - Before anything opens, call `tabtabtab_natron.set_storage_dir(<NATRON_GUI_TEST_OUT>/tabtabtab)`, so the test never touches `~/.Natron`.
    - Opening the menu:
      - Activate the main window.
      - `QCursor.setPos` to the graph viewport's centre. Xvfb honours pointer warps.
      - Send a `QEnterEvent`, then `QTest.keyClick(view, Qt.Key_Tab)`, as `m75_read.py:392-404` does.
      - The popup is `tabtabtab_nuke_core._tabtabtab_instance`, found through `gt.widgets_of("TabTabTabWidget")`.
    - Before the first Tab, an application-wide event filter records Show and Move events on that class, together with the widget's geometry.
    - Checks:
      - Position (M69): the geometry at the first Show already contains the cursor, there is no Move after the first Show, and the same holds on a second open.
      - Search:
        - "blr" ranks Blur first;
        - every anchored result's name starts with "b";
        - " blr" also lists DirBlur;
        - "  blr" lists neither.
      - Category search: `[` plus part of a category (for example "gr[col") lists only rows from that category.
      - Keys: Up from row 0 selects the last row. Escape closes the menu, and so does activating the main window; neither creates a node (the node count is unchanged).
      - Insert after selected: with a Read selected, choosing Blur creates a Blur whose input 0 is the Read, positioned within 150 px of the cursor's scene position.
      - Tab-Tab: reopening shows " Blur [Filter]", fully selected, in the line edit, and Tab creates a second Blur.
      - Group: Tab inside a Group's graph creates the node in that group (`group.getChildren()`).
      - Commands:
        - "pref" lists the Preferences action under Edit, and choosing it opens the Preferences panel (then close it);
        - a menu-bar leaf that is disabled at that moment, picked at run time, is absent from its own query;
        - nothing named "Tab Menu Preferences" or "Tabtabtab" is listed (there is no Edit-menu entry).
      - Colour: Blur's row has a `Qt.UserRole` colour equal to the Filter category colour, and a command row has none.
      - Weights: after the creations, `tabtabtab_weights.json` in the scratch folder has a positive entry for Blur's menupath.
      - Preferences, through Natron's settings (`natron.getSettings().getParam(...)`, not saved):
        - Set `tabMenuSearchModes` to the option whose ID is `consecutive,anchored_fuzzy,non_anchored_fuzzy`, found by scanning `getOption(i)`. On the next open, "blr" lists neither Blur nor DirBlur, and " blr" lists Blur only.
        - Set `tabMenuScroll` to False. On the next open, "a" gives at most 18 rows.
        - Restore both afterwards.
        - No `tabtabtab_prefs.json` is created in the scratch folder or in `~/.Natron`.
    - Shots: the open menu with coloured rows; the `[` filter; a command query; the graph after a Blur is inserted below the selected Read.
  - verify: `NATRON_GUI_TEST_OUT=build/m45-shots Tests/gui/run-gui-test.sh Tests/gui/m45_tab_menu.py build/fast/App/Natron` reports 0 failures at the gate build.
  - size: M

- [x] M45.P8.T2 — Point `m75_read.py`'s Tab-menu helper at the new popup
  - files: `Tests/gui/m75_read.py`
  - approach:
    - `open_node_dialog()` (`:392-404`) waits for `gt.widgets_of("TabTabTabWidget")` instead of `NodeCreationDialog`. It returns the popup, its `QLineEdit` and its `QListView`, as before.
    - Rows read "Read [Image]" or "Read [Image] (R)", and `e.split("[")[0].strip()` still yields "Read", so `listed()` and the checks are unchanged.
    - In `dialog_section`, after `keyClick(Return)`, process events until the file dialog appears. The adapter creates the node one tick after the popup closes, and the existing `inspect()` already polls, so the only change is to replace the single `processEvents()` with a short `until` on `seen["dialog"]`.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m75_read.py build/fast/App/Natron` reports 0 failures at the gate build.
  - size: M

- [x] M45.P8.T3 — Run the one build, the tests, and package the AppImage for UAT
  - files: none (build, run, package)
  - approach:
    - Configure (new files are globbed, so re-run cmake), then build.
    - Run `ctest` (including `TabTabTabPython`, `TabMenuSettings` and `NodeCategory`), then `m45_tab_menu.py`, `m75_read.py`, `m55_node_graph.py`, `m55_input_pipes.py` and `build/m24-shots/m24_border.py` (untracked; run it from that path).
    - Package `build/appimages/M45-<sha>.AppImage`.
  - verify: everything is green, and the AppImage path is posted for the user's UAT.
  - size: M

**Verification gate:** CI `format`, `lint-ci` and `build-and-test` are green, including `TabTabTabPython` (vendored upstream core tests plus the adapter tests), the new `TabMenuSettings` cases and the extended `NodeCategory` cases. `Tests/gui/m45_tab_menu.py`, `m75_read.py`, `m55_node_graph.py`, `m55_input_pipes.py` and `build/m24-shots/m24_border.py` report 0 failures on the gate build. `grep -rn NodeCreationDialog Gui/` is empty. The user signs off the shots and the AppImage UAT, which covers:
- the menu opening under the cursor with no jump;
- "blr" → Blur;
- Tab-Tab repeat;
- an insert below a selected node;
- a menu-command search;
- category colours and icons;
- the "Tab menu" section of Preferences > Nodegraph (a remapped search mode, and scrolling off, both applying on the next open);
- a rebound Create Node shortcut;
- Pick Knob / Link-to-Knob completion still working.

Acceptance sketch:
- Pressing the create-node shortcut (Tab by default, rebindable) in any node graph opens tabtabtab, vendored from tabtabtab-nuke with a Natron adapter. It fuzzy-searches nodes and main-menu commands, colours rows by M24 category, and inserts the chosen node after the selected one.
- Its search-mode and scroll preferences are a "Tab menu" section on Natron's Preferences > Nodegraph page. Usage weights start fresh in tabtabtab's own weights file.

## Decisions

- 2026-10-11 — **Deps `M55` (user-confirmed).** M45 also takes over M69 - Minor Cleanup's "Tab menu position" jump (M69.P2.T5 is cancelled); P2.T1's no-move-after-show positioning fixes it.
- 2026-10-11 — **User answers to the elaboration questions** (`PLAN/DESIGN/2026-10-11-m45-m46-questions.md`):
  - **Insertion:** today's insert-after-the-selected-node only. **M45.P3.T1 (pipe splice) is cancelled.** The M55 dependency now rests only on the post-M55 node-graph files and `m55_node_graph.py` in the gate.
  - **Command search:** the main menu bar only (P3.T2 as written).
  - **Preferences: port tabtabtab's preferences now** (search-mode remap and scroll toggle). A new task in `Engine/Settings.*` is needed; it is added at promotion. Note it overlaps M56 - Visual And Menu Polish's P4.T6 in `Engine/Settings.*`.
  - **Generators** are coloured by their grouping (no probe instance), and there is **no fallback** to the old dialog.
- 2026-10-11 — **Promotion freshness check** (branch `milestone/m45-native-tab-menu` from `main` at `a0a6336e8`, after M55 and M44). All files exist and the M24 APIs keep their names. Line numbers were corrected in P2.T2 and P3.T2. tabtabtab is now cited by function name. `m24_border.py` is untracked at `build/m24-shots/m24_border.py`; P4.T2 and the gate run it from there, alongside the added `m55_input_pipes.py`. The pipe-splice references were removed from the verify text, the gate and the acceptance sketch. **M45.P1.T3** was added for tabtabtab's preferences.
- 2026-10-11 — **Preference defaults (coordinator):** P1.T3 covers only the `Settings` side and P2.T1 reads the preferences on every open, so P1.T3's verify does not depend on P2.T1. The search-mode remap is a single six-way choice, which can only hold a valid permutation. tabtabtab's "Enable tabtabtab" toggle is not ported, because there is no fallback dialog.
- 2026-10-12 — **Lift tabtabtab-nuke instead of reimplementing it in C++ (user).** "TabTabTab is built to be modular and work in any Qt app with minimal work … just lift tabtabtab-nuke directly and just change what needs changing." Phases 45.1 to 45.4 are cancelled and replaced by 45.5 to 45.8. The cancelled briefs are kept as struck-through task lines only; their full text is in this file's git history on the `plan` branch. The 2026-10-11 decisions still hold, except where the calls below replace them. Consultant design calls:
  - **What is vendored:** `tabtabtab_nuke_core.py`, `tabtabtab_prefs.py`, `tabtabtab_prefs_dialog.py`, `LICENSE` and the four core test files, at upstream `48de32d`, into `Gui/Resources/TabTabTab/`. The upstream file names are kept. The core is already host-agnostic and imports PySide6 first, so no Qt.py or PySide2 work is needed. `tabtabtab_nuke.py`, `menu.py` and the Nuke fingerprint test are replaced by `tabtabtab_natron.py`. Vendored code is exempt from the comment policy and stays byte-identical except for two local patches recorded in `UPSTREAM`: a pluggable main-window finder in the core, and an optional Enable toggle with a lazy `tabtabtab_nuke` import in the prefs dialog. Both keep Nuke's behaviour unchanged and can go upstream.
  - **Hook:** a built-in module called from C++, not a registration API. `NodeGraph::event` matches the rebindable `kShortcutIDActionGraphCreateNode` keybind and calls `TabMenuBridge::launch(graph)`, which records the graph and the cursor's scene position and runs `tabtabtab_natron.launch(appN)`. The adapter calls back into three new `GuiApp` methods (`getTabMenuNodeItems`, `getTabMenuLegacyWeights`, `createNodeFromTabMenu`). Node creation therefore stays in C++ with today's exact `CreateNodeArgs` (auto-connect to the selection, undo, the target group). The Python `app.createNode` was not used, because it turns auto-connect and undo off and does not know which graph or group has focus. A `setTabMenuCallback(callable)` registration API was rejected: it adds state that a failing user script can lose, and nothing needs it.
  - **Data across the binding:** the node list crosses as a JSON string. That needs no typesystem or `sourceList.py` change and no shiboken container conversions. The M24 category colour is computed in C++ through a new `Node::categoryForPlugin`, so no separate colour binding is needed.
  - **Where the files live:** `<bin>/../Resources/tabtabtab`, put on `sys.path` by `TabMenuBridge::bootstrap()` at `onAllPluginsLoaded`. They are not under `Plugins/PyPlugs`: dev builds have no `Plugins/PyPlugs`, and the "load bundled plugins" preference would otherwise be able to turn the tab menu off.
  - **Preferences:** tabtabtab's own JSON (`~/.Natron/tabtabtab_prefs.json`) and its own dialog, under Edit > Tab Menu Preferences..., registered through `appPTR->addCommand`. This replaces the cancelled M45.P1.T3 (Natron `Settings` knobs): it is simpler, needs no C++, and avoids M56's edits to `Engine/Settings.*`. The Enable toggle is hidden, because there is no fallback.
  - **Weights:** tabtabtab's own JSON (`~/.Natron/tabtabtab_weights.json`, keyed by menupath), so the core's `NodeWeights` is used unmodified and menu commands get weights too. The first time it runs, the adapter seeds the file from the old `QSettings` `<id><major>_tab_weight` counts, so history carries over. The old keys are then left untouched.
  - **Deferred invoke:** the adapter runs both node creation and menu actions one event-loop tick after the popup closes, so a modal dialog opened by the choice (the Read file dialog, Preferences) never overlaps the popup.
  - **Shortcut group fix:** a plugin's create shortcut is looked up in `kShortcutGroupNodes` plus `/<grouping>`, with the plugin ID as the action ID. The cancelled P1.T2's `Plugin::getPluginShortcutGroup()` is the viewer-interface group.
- 2026-10-12 — **User answers to the consultant's three questions.** These replace the matching consultant calls in the decision above.
  - **Patches:** patch the vendored copy and record it in `UPSTREAM`. Only the core's main-window-finder patch remains (M45.P5.T2). The prefs-dialog patch went away with the next answer.
  - **Preferences live in Natron's Preferences**, not in tabtabtab's dialog.
    - New task **M45.P6.T5** adds a "Tab menu" section to the Nodegraph page in `Engine/Settings.*`, with edits kept additive because M56 is also editing those files. It reuses the cancelled P1.T3 six-way choice, but the option IDs are tabtabtab's mode names joined by commas. There are no C++ accessors.
    - The adapter reads the two knobs on every open through `NatronGui.natron.getSettings().getParam(...)`. This API exists: `AppSettings::getParam` returns typed wrappers, and `ChoiceParam::getOption(i)` returns the option ID. It passes them straight into the core's `launch(space_mode_order=..., scroll_enabled=...)`.
    - `tabtabtab_prefs.py` and `tabtabtab_prefs_dialog.py` are therefore not vendored. The core never imports them, and leaving them out modifies upstream least. The Edit > Tab Menu Preferences... entry and the bootstrap's `addCommand` are dropped.
  - **Weights start fresh:** there is no seeding from the old `QSettings` `<id><major>_tab_weight` counts. `getTabMenuLegacyWeights`/`legacyWeightsJson` are dropped from M45.P6.T2, and the seeding logic and its tests from M45.P5.T3/T4.
- 2026-10-12 — **P5.T3:** the adapter (`tabtabtab_natron.py`) calls the core's `tabtabtab_nuke_core.launch(...)` by module, so tests stub `launch` on the core module. It validates the search-mode order itself, because a reused popup in the core keeps an invalid order. It expects `GuiApp.getTabMenuNodeItems()` to return a JSON list of `{id, major, label, grouping[], icon, color, shortcut}`, plus `GuiApp.createNodeFromTabMenu(id, major)`, and the settings knobs `tabMenuSearchModes` (a choice whose option IDs are comma-joined mode names) and `tabMenuScroll`.
- 2026-10-12 — **P6.T3–T5:** `Gui/NodeCreationDialog.*` was renamed to `Gui/CompleterLineEdit.*`, because the remaining class is the completer and the brief's "no NodeCreationDialog left" check needed it. `m75_read.py` now drives the tab menu (`TabTabTabWidget`). P7.T1 is re-sized from S to M, since it is CMake post-build logic plus the bundle script.
- 2026-10-12 — **Gate (P8.T3):** ctest 1669/1669. GUI scripts on the final binary: `m45_tab_menu` 97/0, `m75_read` 75/0, `m55_node_graph` 162/0, `m55_input_pipes` 118/0, `m24_border` 47/0. Fixed a long-standing product bug: validating a rebound shortcut cleared the old modifiers but kept the old key (`PreferencesPanel.cpp`, `88396da7d`). The popup was about 757 px tall and got pushed above the cursor, so the user approved vendored patch 0002 (`222178856`): host row metrics (Natron row height, 15 rows) and the list flips above the search box near the screen bottom. The popup is now 448 px, with the search box at the cursor. UAT AppImage: `build/appimages/M45-222178856.AppImage`, script `build/appimages/M45-uat.md`.
- 2026-10-12 — **UAT round 1 (user) on `M45-222178856.AppImage`, not yet passed.**
  - **Popup flip:** the user rejects the search box flipping to the bottom. It's confusing and not how tabtabtab works, so the flip comes out of patch 0002. The host row metrics (Natron's row height, 15 rows) stay, and placement goes back to upstream's.
  - **Missing icons:** icons don't show reliably, e.g. Grade and Add. The bridge's icon resolution is being fixed to match how Natron shows those icons elsewhere.
- 2026-10-12 — **UAT round 1 fixes** (`52405c3d0` after the rebase onto `main` with M84, no conflicts):
  - **Popup flip:** the flip is removed from patch 0002. Placement is upstream's again, and Natron's row metrics stay.
  - **Missing icons:** the adapter checked icon paths with `os.path.isfile`, which cannot see `:/` resource paths, so native nodes had no icon. It now uses `QFile.exists`.
  - **SVG icons:** the bundle now also ships the `iconengines` Qt plugins, so SVG icons render sharp.
  - **Tests:** a new `TabMenuBridge.NativeGradeCarriesItsResourceIcon` test was added. The Python suites pass (39 tests).
  - **Waiting on the build:** the rebuild, the GUI re-run and the AppImage wait for M69's end-build agent.
- 2026-10-12 — **Gate re-run on `52405c3d0`** (rebased onto `main` with M84): `m45_tab_menu` 97/0, `m75_read` 90/0, `m55_node_graph` 162/0; ctest 2050/2051. The one failure, `DeepNodesTest.DeepExpressionIsRegisteredAndInstantiable`, is **not M45's**. gtest reported PASS, then the process segfaulted on a background thread during AppManager/Qt shutdown. It passed 8/8 when run alone, and it was the only segfault in 2053 tests. The `Tests` binary does not link NatronGui. This is a rare, pre-existing shutdown race. UAT AppImage round 2: `build/appimages/M45-52405c3d0.AppImage`.
