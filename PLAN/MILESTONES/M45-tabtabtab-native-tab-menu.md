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
- tabtabtab's preferences (the search-mode remap and the scroll toggle) in its own dialog, under Edit > Tab Menu Preferences...

Layout:
- The vendored files live in `Gui/Resources/TabTabTab/`. They ship to `<bin>/../Resources/tabtabtab/`, through a post-build copy for dev builds and `stage-bundle.sh` for the AppImage.
- Vendored files stay byte-identical to upstream apart from the two patches in M45.P5.T2, which are listed in `UPSTREAM`.

One build at the end (user rule): implementers do not compile. The pure-Python tests (M45.P5.T2, M45.P5.T4) need no build, and implementers may run them with the host `python3`.

## Phase 45.1: Matching, ranking and items (cancelled)

- [ ] ~~M45.P1.T1 — Add a pure tab-menu matcher and ranker, with unit tests~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user)
- [ ] ~~M45.P1.T2 — Build the tab-menu item list from the plugin registry, with category colour, icon, shortcut and weight~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; its item-list and category-colour logic moves to M45.P6.T1/T2)
- [ ] ~~M45.P1.T3 — Add the Tab menu preferences (search-mode remap, scroll toggle) to the Nodegraph page, with accessors~~ (cancelled 2026-10-12: replaced by lifting tabtabtab-nuke, user; tabtabtab's own prefs JSON and dialog are used instead)

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

- [ ] M45.P5.T1 — Vendor tabtabtab-nuke's core, prefs, prefs dialog, core tests and licence at a pinned commit
  - files: `Gui/Resources/TabTabTab/` (new): `tabtabtab_nuke_core.py`, `tabtabtab_prefs.py`, `tabtabtab_prefs_dialog.py`, `LICENSE`, `UPSTREAM`, `tests/__init__.py`, `tests/test_node_model_diff.py`, `tests/test_monitor_selection.py`, `tests/test_main_window_discovery.py`, `tests/test_cursor_restore_on_close.py`
  - approach:
    - `git clone https://github.com/charlesangus/tabtabtab-nuke` and `git checkout 48de32d`. If the user names a newer tag, use that instead and record it.
    - Copy the files above byte for byte. Keep the upstream file names: the modules import each other by name, and renaming them would turn the vendored copy into a fork.
    - Do not vendor:
      - `tabtabtab_nuke.py` and `menu.py` (the Nuke adapter, replaced by M45.P5.T3);
      - `tests/test_menu_fingerprint.py` (it tests the Nuke adapter);
      - `readme.md`, `docs/`, `Makefile`, `.github/`.
    - `UPSTREAM` is plain text: the source URL, the commit hash, the date, the list of files taken, the list of files left out, and a "Local patches" heading that M45.P5.T2 fills in.
    - These are vendored third-party files, so the project comment policy does not apply to them. Do not reformat or re-comment them.
  - verify: run `cd Gui/Resources/TabTabTab && python3 -m pytest -q tests` (host python, no build). All the vendored tests pass; upstream reports 38 passing, minus the fingerprint tests that were left out. Run `diff -r` against the clone, restricted to the vendored files; it shows no differences.
  - size: S

- [ ] M45.P5.T2 — Patch the vendored core and prefs dialog so the host is pluggable (two small patches that can go upstream)
  - files: `Gui/Resources/TabTabTab/tabtabtab_nuke_core.py`, `Gui/Resources/TabTabTab/tabtabtab_prefs_dialog.py`, `Gui/Resources/TabTabTab/UPSTREAM`
  - approach:
    - Patch 1 (core), the main-window finder:
      - Add a module global `_main_window_finder = None`, a public `set_main_window_finder(fn)`, and `_find_main_window()`, which returns `(_main_window_finder or _find_nuke_main_window)()`. The late lookup is deliberate: the upstream tests rely on module-global name resolution.
      - Replace the three `_find_nuke_main_window()` calls, in `_try_reparent_preloaded`, `_create_tabtabtab_widget` and `launch`, with `_find_main_window()`.
      - Leave `_find_nuke_main_window` itself unchanged.
    - Patch 2 (prefs dialog), an optional Enable toggle:
      - Move `import tabtabtab_nuke` from module level into `_on_accept`, and only import it when no callback is given.
      - `TabtabtabPrefsDialog.__init__(self, parent=None, show_enabled_toggle=True, on_enabled_changed=None)`. When `show_enabled_toggle` is false, skip adding the "Enable Tabtabtab" row; its checkbox still exists, so the populate code is unchanged.
      - In `_on_accept`, only save `tabtabtab_enabled` and call the enable hook when the toggle is shown. The hook is `on_enabled_changed(enabled)` if one was given, otherwise the current `tabtabtab_nuke.registerNukeAction`/`unregisterNukeAction`.
      - `show_prefs_dialog(parent=None, show_enabled_toggle=True, on_enabled_changed=None)` passes the arguments through.
      - Nuke's call sites (`menu.py`, no arguments) behave exactly as before.
    - Record both patches under "Local patches" in `UPSTREAM`, each with a one-line description, so they can be offered upstream (see the open question).
    - Keep upstream's style in the patched lines: 4-space indent and docstrings where upstream has them.
  - verify:
    - `python3 -m pytest -q tests` in `Gui/Resources/TabTabTab` still passes, including `test_main_window_discovery.py`, unmodified.
    - A new test file in the same folder, `tests/test_host_hooks.py`, uses upstream's stub pattern from `test_main_window_discovery.py`. It checks that:
      - with no finder set, `_find_main_window()` returns the Nuke finder's result;
      - after `set_main_window_finder(lambda: sentinel)`, `_create_tabtabtab_widget` passes `sentinel` as the parent;
      - importing `tabtabtab_prefs_dialog` with no `tabtabtab_nuke` module available succeeds.
    - `test_host_hooks.py` is the one Natron-added file in `tests/`; `UPSTREAM` lists it.
  - size: M

- [ ] M45.P5.T3 — Write the Natron adapter `tabtabtab_natron.py`
  - files: `Gui/Resources/TabTabTab/tabtabtab_natron.py` (new)
  - approach:
    - This replaces `tabtabtab_nuke.py`, and is modelled on its `NukePlugin`. Imports: `tabtabtab_nuke_core` (`TabTabTabPlugin`, `launch`, `find_qt_menu_items`, `set_main_window_finder`), `tabtabtab_prefs`, `tabtabtab_prefs_dialog`, and `PySide6` (`QtCore`, `QtGui`, `QtWidgets`).
    - Storage:
      - `_storage_dir` defaults to `os.path.expanduser("~/.Natron")`.
      - `set_storage_dir(path)` rebinds `tabtabtab_prefs.prefs_singleton = tabtabtab_prefs.TabtabtabPrefs(os.path.join(path, "tabtabtab_prefs.json"))` and points the weights path at `os.path.join(path, "tabtabtab_weights.json")`.
      - It is called once at import with the default. The GUI test calls it with a scratch folder.
      - `tabtabtab_prefs.py` itself is not patched. The dialog reads `tabtabtab_prefs.prefs_singleton` when it is called, so rebinding the singleton is enough.
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
      - `get_weights_file()` returns the weights path. The first time it is called in a session, if that file does not exist, seed it:
        - build `{menupath: weight}` for the current node items from `json.loads(app.getTabMenuLegacyWeights())`, which maps `"<id>|<major>"` to the old QSettings count;
        - write it only if at least one weight is non-zero.
        - This keeps users' history from the old dialog.
      - `invoke(thing)` must not act synchronously: the core calls `invoke` before it closes the popup, and both creating a Read (a modal file dialog) and menu actions (Preferences) open windows. Keep a reference to the callable and `QtCore.QTimer.singleShot(0, ...)` it:
        - for a `_NodeEntry`, call `app.createNodeFromTabMenu(id, major)`;
        - for a `QAction`, `action.trigger()` if the action still exists. Guard with `shiboken6.isValid` and catch `RuntimeError`.
      - `get_icon(menuobj)`:
        - for a `_NodeEntry`, a `QIcon(path)` cached per path, or `None` if the path is empty or missing;
        - for a `QAction`, the base-class default (`menuobj.icon()`).
      - `get_color(menuobj)`: for a `_NodeEntry` with a colour, `(QColor(c), QColor(c))`, which is the Nuke adapter's convention; otherwise `(None, None)`.
    - Module entry points:
      - `launch(app)`: calls `_plugin.set_app(app)`, then the core's `launch(_plugin, space_mode_order=prefs.get("space_mode_order"), scroll_enabled=prefs.get("scroll_enabled"))`, with the prefs read on every call so changes apply on the next open.
      - `show_prefs_dialog()`: calls `tabtabtab_prefs_dialog.show_prefs_dialog(parent=_find_natron_main_window(), show_enabled_toggle=False, on_enabled_changed=lambda enabled: None)`. There is no fallback dialog, so there is no Enable toggle (Decisions).
    - Do not port `schedule_preload`: the widget is built on the first Tab, and the item build is cheap.
    - The comment policy applies to this file, which is new Natron code.
  - verify: M45.P5.T4's unit tests at any time (no build); M45.P8.T1 at the gate.
  - size: L

- [ ] M45.P5.T4 — Unit-test the adapter with stubbed PySide6 and NatronGui, and register the Python tests with ctest
  - files: `Tests/python/test_tabtabtab_natron.py` (new), `Tests/python/run_tabtabtab_tests.py` (new), `Tests/CMakeLists.txt`
  - approach:
    - `test_tabtabtab_natron.py` is a stdlib `unittest` module. It loads the adapter by path, using the stub-module pattern of upstream `tests/test_main_window_discovery.py`: `PySide6`/`QtCore`/`QtGui`/`QtWidgets`/`shiboken6` stubs in `sys.modules`, restored afterwards.
    - `QTimer.singleShot` is stubbed so that it records calls. The `app` is a fake with canned `getTabMenuNodeItems`/`getTabMenuLegacyWeights` JSON and a recording `createNodeFromTabMenu`.
    - Cases:
      - node `menupath` and display formatting ("Blur [Filter]");
      - a "/" in a label is replaced;
      - a duplicate menupath gets " (2)";
      - command items are appended, with shortcut text;
      - `get_color` parses "#rrggbb" and returns `(None, None)` for commands and for an empty colour;
      - `invoke` on a node schedules exactly one call and does not call `createNodeFromTabMenu` until the recorded callback runs;
      - `invoke` on an action schedules `trigger`;
      - weight seeding writes `{menupath: n}` only when the file is absent and some weight is non-zero, and never overwrites an existing file;
      - `set_storage_dir` repoints both the prefs and the weights paths;
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

- [ ] M45.P6.T1 — Resolve a plugin's node category without an instance
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Tests/NodeCategory_Test.cpp`
  - approach:
    - Add `static NodeCategoryEnum Node::categoryForPlugin(const Plugin& plugin)`, declared beside `domainCategoryFromGrouping`/`categoryFromGroupingAndLabel` (`Node.h:797-808`). It mirrors `Node::getNodeCategory()` (`Node.cpp:4402-4432`) without an effect instance:
      - `domainCategoryFromGrouping` on the plugin's grouping (converted to `std::list<std::string>`);
      - then `plugin.isReader()` → Read and `plugin.isWriter()` → Write;
      - then `categoryFromGroupingAndLabel(first grouping, plugin label)`.
    - Generators can't be detected without an instance, so they fall through to the grouping heuristic. This is the user's decision ("generators coloured by grouping"). Say so in one comment line on the declaration.
  - verify: `ctest -R NodeCategory`. New cases show that `categoryForPlugin(*node->getPlugin())` equals `node->getNodeCategory()` for a Read, a Write, a Deep node, a Grade, a Blur and a Merge created in the test app.
  - size: M

- [ ] M45.P6.T2 — Write `TabMenuBridge`: the node item list, legacy weights, node creation, launch and startup bootstrap, exposed on `GuiApp`
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
    - `QString legacyWeightsJson()` returns a JSON object mapping `"<id>|<major>"` to the old `QSettings(NATRON_ORGANIZATION_NAME, NATRON_APPLICATION_NAME)` value of `<id><major>_tab_weight`. It uses one `QSettings` instance and includes only the non-zero values. Logic moved from `getPluginWeight` (`NodeCreationDialog.cpp:348-367`). Nothing writes those keys any more.
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
      - otherwise runs `import sys` and `sys.path.insert(0, r'<dir>')` (only if it is not there yet), then `import tabtabtab_natron`, through `interpretPythonScript` in `__main__`;
      - calls `appPTR->addCommand(tr("Edit/Tab Menu Preferences..."), "tabtabtab_natron.show_prefs_dialog", (Qt::Key)0, Qt::NoModifier)`. `Gui::addMenuEntry` wraps it as `app = appN; f()` (`Gui50.cpp:1017`).
      - This avoids `Gui.cpp` and `Engine/Settings.*`, which M56 is also editing.
    - Bindings: on `GuiApp` (`PyGuiApp.h`, after `renderBlocking`), add:
      - `QString getTabMenuNodeItems() const`
      - `QString getTabMenuLegacyWeights() const`
      - `bool createNodeFromTabMenu(const QString& pluginID, int majorVersion)`
      - Each forwards to `TabMenuBridge`.
      - These are plain `QString`/`int`/`bool` signatures on an existing `object-type`, so shiboken generates them with no `typesystem_natronGui.xml` change and no `sourceList.py` change. Do not include `TabMenuBridge.h` from `PyGuiApp.h`; include it from `PyGuiApp.cpp` only.
  - verify: at the gate, M45.P8.T1 exercises every function through the live menu. Also, `python -c` in the built Natron's script editor (by hand in the UAT): `json.loads(app1.getTabMenuNodeItems())` lists Blur with a non-empty colour.
  - size: L

- [ ] M45.P6.T3 — Route the node graph's create-node shortcut and right-click "Create Node" to the bridge, and run the bootstrap at startup
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

- [ ] M45.P6.T4 — Remove NodeCreationDialog, keeping CompleterLineEdit for its other users
  - files: `Gui/NodeCreationDialog.h`, `Gui/NodeCreationDialog.cpp`, `Gui/Resources/Stylesheets/mainstyle.qss`
  - approach:
    - Delete the `NodeCreationDialog` class, its private struct, and the `getPluginWeight`/`incrementPluginWeight` statics (moved to the bridge in M45.P6.T2).
    - Keep `CompleterLineEdit` in the same files, because `PickKnobDialog.cpp`, `LinkToKnobDialog.cpp`, `KnobGuiPrivate.h`, `EditScriptDialog.cpp` and `EditExpressionDialog.cpp` include it.
    - In `CompleterLineEdit`, drop the `dynamic_cast<NodeCreationDialog*>` branches in `setTextFromIndex` and `keyPressEvent`, so quick-exit always calls `_imp->dialog->accept()`. Leave the list-positioning code alone.
    - Remove the `NodeCreationDialog, Natron--NodeCreationDialog` rule (`mainstyle.qss:321-326`).
  - verify: at the gate build, `grep -rn NodeCreationDialog Gui/` finds nothing, and the Pick Knob and Link-to-Knob dialogs still complete node names (by hand in the UAT).
  - size: M

## Phase 45.7: Shipping the Python files

- [ ] M45.P7.T1 — Copy the vendored folder next to the binary in dev builds and into the AppImage
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

- [ ] M45.P8.T1 — Write the GUI test script for the tab menu
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
        - "Tab Menu Preferences..." is listed under Edit.
      - Colour: Blur's row has a `Qt.UserRole` colour equal to the Filter category colour, and a command row has none.
      - Weights: after the creations, `tabtabtab_weights.json` in the scratch folder has a positive entry for Blur's menupath.
      - Preferences (in memory, not saved): set `space_mode_order` to consecutive / anchored / non-anchored. After that, "blr" lists neither Blur nor DirBlur and " blr" lists Blur only. Set `scroll_enabled` to False: "a" gives at most 18 rows. Restore both afterwards.
    - Shots: the open menu with coloured rows; the `[` filter; a command query; the graph after a Blur is inserted below the selected Read.
  - verify: `NATRON_GUI_TEST_OUT=build/m45-shots Tests/gui/run-gui-test.sh Tests/gui/m45_tab_menu.py build/fast/App/Natron` reports 0 failures at the gate build.
  - size: M

- [ ] M45.P8.T2 — Point `m75_read.py`'s Tab-menu helper at the new popup
  - files: `Tests/gui/m75_read.py`
  - approach:
    - `open_node_dialog()` (`:392-404`) waits for `gt.widgets_of("TabTabTabWidget")` instead of `NodeCreationDialog`. It returns the popup, its `QLineEdit` and its `QListView`, as before.
    - Rows read "Read [Image]" or "Read [Image] (R)", and `e.split("[")[0].strip()` still yields "Read", so `listed()` and the checks are unchanged.
    - In `dialog_section`, after `keyClick(Return)`, process events until the file dialog appears. The adapter creates the node one tick after the popup closes, and the existing `inspect()` already polls, so the only change is to replace the single `processEvents()` with a short `until` on `seen["dialog"]`.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m75_read.py build/fast/App/Natron` reports 0 failures at the gate build.
  - size: M

- [ ] M45.P8.T3 — Run the one build, the tests, and package the AppImage for UAT
  - files: none (build, run, package)
  - approach:
    - Configure (new files are globbed, so re-run cmake), then build.
    - Run `ctest` (including `TabTabTabPython` and `NodeCategory`), then `m45_tab_menu.py`, `m75_read.py`, `m55_node_graph.py`, `m55_input_pipes.py` and `build/m24-shots/m24_border.py` (untracked; run it from that path).
    - Package `build/appimages/M45-<sha>.AppImage`.
  - verify: everything is green, and the AppImage path is posted for the user's UAT.
  - size: M

**Verification gate:** CI `format`, `lint-ci` and `build-and-test` are green, including `TabTabTabPython` (vendored upstream core tests plus the adapter tests) and the extended `NodeCategory` cases. `Tests/gui/m45_tab_menu.py`, `m75_read.py`, `m55_node_graph.py`, `m55_input_pipes.py` and `build/m24-shots/m24_border.py` report 0 failures on the gate build. `grep -rn NodeCreationDialog Gui/` is empty. The user signs off the shots and the AppImage UAT, which covers:
- the menu opening under the cursor with no jump;
- "blr" → Blur;
- Tab-Tab repeat;
- an insert below a selected node;
- a menu-command search;
- category colours and icons;
- Edit > Tab Menu Preferences... (a remapped search mode, and scrolling off, both applying on the next open);
- a rebound Create Node shortcut;
- Pick Knob / Link-to-Knob completion still working.

Acceptance sketch:
- Pressing the create-node shortcut (Tab by default, rebindable) in any node graph opens tabtabtab, vendored from tabtabtab-nuke with a Natron adapter. It fuzzy-searches nodes and main-menu commands, colours rows by M24 category, and inserts the chosen node after the selected one.
- Its search-mode and scroll preferences live in tabtabtab's own dialog under Edit, and usage weights carry over from the old dialog.

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
