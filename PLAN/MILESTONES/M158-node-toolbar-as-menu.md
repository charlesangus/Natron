# M158 - Node Toolbar As Menu

The left-hand node toolbar becomes a view over one ordinary, process-wide `QMenu` tree, with a menu per grouping and an action per plug-in. A custom widget draws that tree's top-level menus as today's icon buttons. Python adds arbitrary items to it the Nuke way, with plain PySide `QAction`s that run a function. The tab menu then gets every node and every Python item by walking that menu, as it already walks the menubar. The first user is M101 - Clean-Sheet Merge Node, whose operator presets (Plus, Matte, Multiply…) become Python menu items that create `Merge` and set `operation` (user, 2026-10-22).

Scouted facts (2026-10-22, `main` `1eed87752`):
- **Plug-in tree:** `PluginGroupNode` (`Engine/Plugin.h:51`) is built in `GuiApplicationManager::onPluginLoaded` (`Gui/GuiApplicationManager.cpp:~1068`) through `findPluginToolButtonOrCreate` (`:1140`). `ignorePlugin` (`:1133`) removes a node, and `getTopLevelPluginsToolButtons()` (`:913`) lists the top level.
- **ToolButton** (`Gui/ToolButton.h:46`) is a `QObject`, not a widget. A group holds a `Menu*`, and a leaf holds a `QAction*`. `Gui::findOrCreateToolButton` (`Gui/Gui20.cpp:826-941`) builds them per window:
  - Icon rules: SVG goes through `ThemedIcon::icon`, a top-level PNG through `tintedIcon`, and a group with no icon through `getIconForGrouping`.
  - A plug-in with no grouping goes under "Misc" (`PLUGIN_GROUP_DEFAULT`, `Global/Macros.h:281`).
  - `&` is doubled, and older majors use `getLabelVersionMajorEncoded()`.
- **Click:** `ToolButton::onTriggered` (`Gui/ToolButton.cpp:185-199`) creates the node in `getLastSelectedNodeCollection()` with the plug-in's major and minor.
- **Toolbar widget:** `GuiAppInstance::loadInternal` (`:~281`) builds the per-window buttons and emits `pluginsPopulated`. That runs `Gui::addToolButttonsToToolBar` (`Gui20.cpp:981`), which calls `GuiPrivate::addToolButton` (`Gui/GuiPrivate.cpp:550`) to make an `AutoRaiseToolButton` per top-level group, `InstantPopup`. Order comes from `namedGroupsOrdered[16]` (`Gui20.cpp:87`).
- **Right-click menu:** the graph menu (`Gui/NodeGraph35.cpp:480-484`) re-adds the same top-level QMenus. Sharing one tree is already proven there.
- **Colours and shortcuts:** colours are per category (`Settings::getNodeCategoryColor`, `Node::categoryForPlugin`), not per button. Plug-in shortcuts are global keybinds in `kShortcutGroupNodes/<grouping>` (`GuiApplicationManager.cpp:1081-1129`).
- **Python startup:** `AppManager::loadPythonGroups` (`Engine/AppManager.cpp:1905`) runs `init.py` and then, GUI only, `initGui.py` per search path, before any GUI exists. `natron.addMenuCommand` (`Gui/PyGlobalGui.h:110`) therefore only queues a `PythonUserCommand` (`GuiApplicationManager10.cpp:1169`), applied at GUI creation by `Gui::createMenuActions` and `addMenuEntry` (`Gui/Gui.cpp:764`, `Gui/Gui50.cpp:1007`). No `init.py` or `initGui.py` is tracked in the repo.
- **Tab menu:** node items come from `GuiApp::getTabMenuNodeItems()` and `TabMenuBridge::nodeItemsJson()` (`Gui/PyGuiApp.cpp:618`, `Gui/TabMenuBridge.cpp:104-158`, fields id, major, label, grouping, icon, color, shortcut). Nodes are created by `createNodeFromTabMenu` (`TabMenuBridge.cpp:161-187`, at the cursor in the static `g_target` graph). Command items come from `find_qt_menu_items(menubar)` (`tabtabtab_nuke_core.py:141`), and weights are keyed by menupath.
- **Tests:** `Tests/gui/m45_tab_menu.py`, `Tests/python/test_tabtabtab_natron.py`, `Gui/Resources/TabTabTab/tests/`, `Tests/TabMenuBridge_Test.cpp`, and `Tests/gui/m56_visual.py:973` (left toolbox).

## Scope

**In:**
- One process-wide node `QMenu` tree, objectName `NodesMenu`, owned by `GuiApplicationManager`. Every window's toolbar and graph right-click menu display it.
- A Python accessor, `NatronGui.natron.nodesMenu()`, returning that `QMenu`.
- `initGui.py` runs after the first GUI window and the node menu exist, and `addMenuCommand` inserts live once a GUI exists.
- The tab menu takes node and Python items from walking `NodesMenu`. `getTabMenuNodeItems`/`createNodeFromTabMenu` are deleted.
- A creation context: nodes created while the tab menu triggers an action land at the cursor in the graph it was opened over.

**Out:**
- Merge's preset menu items: M101 - Clean-Sheet Merge Node.
- Toolbar buttons for Python-added top-level menus. They are reachable from the right-click and tab menus only.
- A "Nodes" entry in the menubar.
- A Nuke-style `menu.py` stage: none.

**Edge cases:**
- Enabling or disabling a plug-in in Preferences, or reloading PyPlugs, rebuilds only the plug-in-owned actions, which carry a marker property. Python-added actions survive.
- A Python item whose path equals a plug-in's is kept, and `_make_menupaths_unique` numbers it in the tab menu.
- Several project windows share one tree. A window opened later shows the Python items added earlier.
- Headless runs build no menu and run no `initGui.py`, as today.
- Older plug-in majors keep their version-encoded labels.
- A tab-menu action that creates several nodes: all of them go into the context's graph, and the first lands at the cursor.

**Assumptions:**
- A `QMenu` shown by several windows' tool buttons behaves as it does in the right-click menu today.
- Unconfirmed: a Python-added top-level menu gets no toolbar button.

## Phase 158.1: One shared node menu

- [x] M158.P1.T1 — Build the plug-in tree as one process-wide QMenu tree
  - files: `Gui/GuiApplicationManager.{h,cpp}`, `Gui/ToolButton.{h,cpp}`, `Gui/Gui20.cpp`
  - approach:
    - `GuiApplicationManager` owns a `QMenu` with objectName `NodesMenu`, built from the `PluginGroupNode` tree once plug-ins load. Use a `Menu` per grouping, in `namedGroupsOrdered` order, and a `QAction` per user-creatable plug-in.
    - Move the icon, "Misc", `&`-doubling and version-label rules out of `Gui::findOrCreateToolButton`.
    - Each plug-in action carries properties for its plug-in ID, major, category colour name and shortcut text, plus a plug-in-owned marker.
    - Triggering an action creates the node as `ToolButton::onTriggered` does, in the active window's last-selected collection.
    - `ignorePlugin` and enable/disable rebuild only marker-carrying actions.
    - Shrink or delete `ToolButton` to whatever is still needed.
  - verify: a GUI test under Xvfb checks the tree is complete: every `getIsUserCreatable()` plug-in has exactly one action under its grouping path. It also checks that disabling a plug-in removes only its action and keeps a foreign action added to the same submenu.
  - size: L

- [x] M158.P1.T2 — Draw each window's toolbar and right-click menu from the shared menu
  - files: `Gui/GuiPrivate.cpp`, `Gui/Gui20.cpp`, `Gui/Gui.cpp`, `Gui/GuiAppInstance.cpp`, `Gui/NodeGraph35.cpp`
  - approach:
    - Per window, make one `AutoRaiseToolButton` per top-level submenu of `NodesMenu` that the plug-in tree created, with the same icon, size, order and `InstantPopup`. The look is unchanged.
    - The graph's right-click menu adds the shared top-level menus.
    - The per-window ToolButton build in `loadInternal` goes away.
  - verify: `Tests/gui/m56_visual.py`'s left-toolbox screenshot matches the pre-change one. Share the before and after shots with the user. A second window shows the same menus.
  - size: M

- [x] M158.P1.T3 — Add `natron.nodesMenu()` to the Python API
  - files: `Gui/PyGlobalGui.h`, the NatronGui typesystem XML, `Tests/gui/` (new test script)
  - approach:
    - A one-line accessor returning the shared `QMenu*`, with no ownership transfer to Python (check the typesystem's ownership rules; see how other `QWidget*` returns are bound).
    - Before the menu exists, or headless, it returns `None`.
  - verify: a GUI test calls `natron.nodesMenu()`, checks its objectName, and adds a `QAction` under `Merge/`. The action shows in the toolbar's Merge popup.
  - size: M

## Phase 158.2: Startup order

- [x] M158.P2.T1 — Run initGui.py after the first GUI window, and insert addMenuCommand live
  - files: `Engine/AppManager.{h,cpp}` (`loadPythonGroups`), `Gui/GuiAppInstance.cpp`, `Gui/GuiApplicationManager10.cpp` (`addCommand`), `Gui/Gui50.cpp` (`addMenuEntry`)
  - approach:
    - `init.py` keeps its timing.
    - `initGui.py`, per search path in the same order, runs once per process, right after the first `Gui` is built and `NodesMenu` is populated.
    - `addCommand` called while a GUI exists inserts the entry into every open window's menubar at once. Before a GUI exists it queues as today.
    - Check nothing in `loadPythonGroups`' GUI-only branch depends on running before the GUI. Report anything that moved.
  - verify: a GUI test with a temporary search-path `initGui.py` adds a `nodesMenu()` action and an `addMenuCommand` entry, and both are present once the window is up.
  - size: M

## Phase 158.3: Tab menu over the shared menu

- [x] M158.P3.T1 — Take tab-menu items from the node menu and drop the JSON node source
  - files: `Gui/Resources/TabTabTab/tabtabtab_natron.py`, `Gui/TabMenuBridge.{h,cpp}`, `Gui/PyGuiApp.{h,cpp}`, the NatronGui typesystem XML
  - approach:
    - Replace `_node_items` with `find_qt_menu_items` over `natron.nodesMenu()`.
    - Menupaths start below `NodesMenu` (`Merge/Plus`, `Filter/Blur`), so they read as today's `grouping/label`.
    - Icon comes from the action, colour and shortcut from the action's properties.
    - Delete `getTabMenuNodeItems`, `nodeItemsJson` and `createNodeFromTabMenu`, plus their bindings.
    - Menubar command items stay.
  - verify: covered by P3.T3's updated tests.
  - size: M

- [x] M158.P3.T2 — Place nodes created from the tab menu at the cursor
  - files: `Gui/TabMenuBridge.cpp`, `Engine/AppInstance.{h,cpp}` (node creation), `Gui/Resources/TabTabTab/tabtabtab_natron.py`
  - approach:
    - Around the deferred `action.trigger()`, the bridge sets a scoped creation context: the target graph's collection and the cursor's scene position.
    - Any `createNode`, from C++ or Python, with no explicit collection or position goes into that collection, and the first one lands at the position.
    - Clear the context after the trigger, even if the action raises.
  - verify: a GUI test opens the tab menu over a graph at a known point and triggers a Python action that calls `app.createNode("…")`. The node is in that graph at the point. A toolbar click outside the tab menu still uses default placement.
  - size: M

- [x] M158.P3.T3 — Update the tab-menu tests for the menu-walk source
  - files: `Tests/gui/m45_tab_menu.py`, `Tests/python/test_tabtabtab_natron.py`, `Gui/Resources/TabTabTab/tests/`, `Tests/TabMenuBridge_Test.cpp`
  - approach: replace the stubs and assertions on `getTabMenuNodeItems` with a stub or real `nodesMenu()`. Keep each test's intent: weights, uniqueness, shortcuts, icons and node creation. Add a case where a Python action under `Merge/` is listed and triggers.
  - verify: the four suites pass at the gate.
  - size: M

## Phase 158.4: Docs and UAT

- [x] M158.P4.T1 — Document the node menu, `nodesMenu()` and the new initGui.py timing
  - files: the Python scripting docs that cover `init.py`/`initGui.py` and `addMenuCommand`. These live on the docs orphan branch (`docs/decisions/2026-09-04-docs-to-orphan-branch.md`), not under `docs/` on main; locate the page there
  - approach: explain that `initGui.py` now runs after the first window exists. Give a Nuke-style example adding a preset item with `nodesMenu()`, and say that the tab menu lists every node-menu item.
  - verify: the docs build, if the docs pipeline has a check.
  - size: M

- [ ] M158.P4.T2 — UAT
  - files: none (user acceptance)
  - approach: hand the user an AppImage and a checklist:
    - the toolbar looks unchanged
    - the right-click menu works
    - a Python item added from `~/.Natron/initGui.py` shows in the toolbar menu and the tab menu
    - the tab menu creates nodes at the cursor
    - shortcuts still show in the tab menu
    - disabling a plug-in removes it from both
  - verify: the user signs off. Record the findings under `## Decisions`.
  - size: S

**Verification gate:** in one build:
- The full test suite passes, and `m45_tab_menu.py`/`m56_visual.py` pass under Xvfb.
- The GUI bench runs (`tools/bench/gui/run.sh --xvfb`, `compare.py` against `baselines/xvfb.jsonl`), because the diff touches `Gui/NodeGraph*` — only if M117 - GUI Perf Harness And Baseline has landed by then; otherwise record "N/A, no bench yet" here.
- The before/after toolbar screenshots are shared and OK'd.
- CI is green.
- The user signs off the UAT.

## Decisions

- 2026-10-22 — **Toolbar is a plain QMenu tree, and the tab menu walks it** (user, M101 grill): presets and studio tools are plain Python menu items, not a new Natron preset API. The tab menu picks them up because everything is a menu item.
- 2026-10-22 — **Own milestone, which M101 - Clean-Sheet Merge Node depends on** (user): it's general UI work that can run beside M101's maths and framework phases.
- 2026-10-22 — **initGui.py moves to after the first GUI window** (user): scripts can then add actions to the live menu directly. `addMenuCommand` inserts live to match. No backward compatibility for scripts that relied on the old timing.
- 2026-10-22 — **One shared menu with a `nodesMenu()` accessor** (user): one tree for every window and the right-click menu, so `initGui.py`, which runs once, reaches all of them.
- 2026-10-22 — **Tab-menu creations land at the cursor through a creation context** (user): Python presets need no positioning code, and today's tab-menu behaviour is kept.
- 2026-10-22 — **Toolbar looks identical** (user).
- 2026-10-22 — **Tab-menu paths start below `NodesMenu`** (planner): they read as today's `grouping/label` paths, so the display and the tests' path keys are unchanged.
- 2026-10-22 — **Claimed despite strict file overlap** (user): run alongside M80, M16 and M158 in parallel lanes, with M64 - Tiled Rendering (parked) not treated as a blocker; overlaps with active rows (M86, M107, M154) and sibling lanes are one or two shared files, handled by rebasing after each merge.
- 2026-10-22 — Freshness check (§5a) at claim: stale file lists and line numbers corrected in place; no premise change.
- 2026-10-22 — P1.T1's completeness check (every user-creatable plug-in has exactly one action) can't run in C++: GuiTests uses a plain `AppManager`, and `GuiApplicationManager::initGui` runs `exec()` and is `FINAL`. With `getTabMenuNodeItems` deleted (P3.T1), Python sees only plug-in IDs, so `m158_nodes_menu.py` now checks that every action names a registered plug-in, no version repeats, and Merge/Blur sit under their groupings; the startup tree is the snapshot for disable/enable. Accepted as is: the tree is built from the same `PluginGroupNode` walk as before.
- 2026-10-22 — Plug-ins with no grouping read `Misc/<label>` in the tab menu (the menu's placement), not a bare `<label>`. The tab-menu adapter has its own menu walker instead of `find_qt_menu_items`, which split labels containing `/` and skipped top-level leaves.
- 2026-10-22 — P1–P3 verified on the fast AppImage of `8570052b4`: ctest 331/331 on both legs, and `m158_nodes_menu`, `m158_init_gui`, `m45_tab_menu` and `m56_visual` pass under Xvfb. The earlier failures were in the tests (fixed in `35d62a642`). PySide 6.8 invalidates a menu returned by `QAction.menu()` once the action's Python wrapper is collected, even though the C++ menu lives on. The helper `gt.menu_of` keeps the owner alive. P4.T1's docs must warn script authors to hold the action, or to walk the menu through `findChildren(QMenu)`. The toolbox class `AutoHideToolBar` has no `Q_OBJECT`, so it reports as `QToolBar`. `m45_tab_menu`'s "group" case fails intermittently on main as well (the node never joins the group); it predates this milestone.
- 2026-10-22 — A startup segfault on an NVIDIA desktop (Qt compositor shader compile with an empty log) was seen once with this milestone's AppImage and not reproduced. No line in this milestone touches GL. It went to the inbox as a separate milestone request (GL context restore on the GUI thread).
- 2026-10-22 — P4.T1 docs are committed on the local `docs` branch as `b746872d4` (worktree `.worktrees/m158-docs`). They will be pushed when this milestone merges, so the published docs never describe unmerged behaviour. The Sphinx build passes; the only new warnings are the pre-existing duplicate `addMenuCommand` overload descriptions. Not yet run: the `findChildren(QMenu)` alternative in the PySide caveat. `Documentation/source/devel/initGui.py` still shows the queued `addMenuCommand` form, which still works.

## Handoff (2026-10-22, PM paused by user)

- Lane: project root, branch `milestone/m158-node-toolbar-as-menu`, HEAD `8570052b4` (P1–P3 code), pushed, working tree clean.
- Verified: P1 build 330/330 ctest; `m56_visual.py` toolbox pixel-identical to the beta43 (main `13f12edca`) baseline (`build/m158-shots/base-out/icons-toolbox.png` vs `p1-out/`). Remote fast build of `8570052b4` (P1–P3): **331/331 ctest both legs**; AppImage at `build/remote/fast/Natron-2.6.0-x86_64.AppImage`.
- Next: run the GUI tests on that AppImage — `m158_nodes_menu.py`, `m158_init_gui.py`, `m45_tab_menu.py`, `m56_visual.py` with `M56_TOOLBOX_BASELINE=build/m158-shots/base-out/icons-toolbox.png` — via `docker exec -e NATRON_GUI_TEST_OUT=build/<dir> -w /home/bosley/git/Natron natron-dev Tests/gui/run-gui-test.sh Tests/gui/<script>.py build/remote/fast/Natron-2.6.0-x86_64.AppImage`. Green → check off P1.T1–P3.T3. Then P4.T1 (docs on the docs orphan branch), P4.T2 UAT AppImage, gate, PR. Unopened: M101 - Clean-Sheet Merge Node waits on this merging.
