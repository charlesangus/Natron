# M45 - Native Tab Menu

Full title: Port tabtabtab-nuke as the native tab menu

Replace Natron's node-creation dialog (`Gui/NodeCreationDialog.*`, a frameless `QDialog` around a `CompleterLineEdit` that matches `*a*b*c*` wildcards over plugin names) with a native C++ reimplementation of github.com/charlesangus/tabtabtab-nuke (Unlicense; behaviour carried over, no code). It brings:
- anchored fuzzy search, with leading-space prefixes for the other search modes and `[` to search the category;
- ranking by consecutive match first, then usage weight, then name;
- per-category colour blocks (from M24), plugin icons and shortcuts in each row;
- Tab-Tab to repeat the last node;
- a correct position the first time it opens (this takes over M69's "Tab menu position" item);
- insertion after the selected node (today's behaviour, kept);
- tabtabtab's preferences: the search-mode remap and the scroll toggle (P1.T3).

One build at the end (user rule): implementers do not compile.

## Phase 45.1: Matching, ranking and items

- [ ] M45.P1.T1 — Add a pure tab-menu matcher and ranker, with unit tests
  - files: `Gui/TabMenuMatch.h` (new), `Gui/TabMenuMatch.cpp` (new), `Tests/TabMenuMatch_Test.cpp` (new), `Tests/CMakeLists.txt` (one line in `GuiTests_SOURCES`)
  - approach:
    - Port tabtabtab's `consec_find` / `nonconsec_find` and `NodeModel.update` (`tabtabtab_nuke_core.py`: `consec_find`, `nonconsec_find`, `NodeModel.__init__`/`update`; cite by name, upstream line numbers drift) as Qt-core-only free functions over `QString`, with no Natron types.
    - Inputs: a query and a list of `{key, displayName, category, weight}`. Output: the ordered matching keys.
    - Search modes:
      - 0 leading spaces: anchored fuzzy (the first letter must match the start of the name; the rest in order).
      - 1 leading space: non-anchored fuzzy.
      - 2 leading spaces: consecutive substring only.
      - A query starting with `[`, or containing one, also searches the ` [Category]` suffix, which is ignored otherwise.
    - Matching is case-insensitive. Consecutive matching also tries the name with spaces, `-` and `_` stripped.
    - Ranking: tier A (consecutive matches), then tier B (fuzzy-only matches). Within each tier, sort by weight descending, then by name ascending.
    - The space-prefix-to-mode mapping is a parameter: `enum class TabMenuMatchMode { Anchored = 0, NonAnchored = 1, Consecutive = 2 }` and a `std::array<TabMenuMatchMode, 3>` indexed by the leading-space count, defaulting to {Anchored, NonAnchored, Consecutive}. An array that is not a permutation falls back to the default, as tabtabtab does. The int values are fixed, because P1.T3's `Settings` accessor returns them as ints (Engine cannot include Gui headers).
  - verify: `ctest -R TabMenuMatch` covers:
    - tabtabtab's doctest cases ("m2"/"move2d", "mm"/"matchmove", anchored "atch" fails, " ov" consecutive);
    - "blr" → Blur but not MotionBlur;
    - " blr" → both;
    - "  blr" → neither;
    - "ax[3" → only the 3D-category item;
    - a tier-A item with weight 0 ranks above a tier-B item with weight 99;
    - a weight tie breaks alphabetically;
    - with the order {Consecutive, Anchored, NonAnchored}, "blr" matches neither Blur nor MotionBlur, " blr" matches Blur only, and "  blr" matches both;
    - a non-permutation order behaves like the default.
  - size: M

- [ ] M45.P1.T2 — Build the tab-menu item list from the plugin registry, with category colour, icon, shortcut and weight
  - files: `Gui/TabMenuItems.h` (new), `Gui/TabMenuItems.cpp` (new), `Engine/Node.h`, `Engine/Node.cpp`, `Tests/NodeCategory_Test.cpp`
  - approach:
    - Move the list-building loop from the `NodeCreationDialog` constructor (`Gui/NodeCreationDialog.cpp`, the `PluginsMap` walk) into `std::vector<TabMenuItem> buildPluginTabMenuItems()`, with the same rules:
      - skip `!getIsUserCreatable()`;
      - for multi-version plugins, the newest version uses `generateUserFriendlyPluginID()` and older ones use `generateUserFriendlyPluginIDMajorEncoded()`;
      - PyPlugs and toolsets stay in the list.
    - Each `TabMenuItem` holds: kind (node/command), plugin ID, major version, display name, category text, icon path (`Plugin::getIconFilePath()`), shortcut text, an optional `QColor`, and the weight.
    - Weights: keep today's `QSettings` keys (`<pluginID><major>_tab_weight`, `getPluginWeight`/`incrementPluginWeight`), so users keep their history. Move both helpers here.
    - Category colour:
      - Add `static NodeCategoryEnum Node::categoryForPlugin(const Plugin&)`. It mirrors `Node::getNodeCategory()` (`Engine/Node.cpp:4403-4432`) without an effect instance: `domainCategoryFromGrouping`, then `Plugin::isReader()`/`isWriter()`, then `categoryFromGroupingAndLabel`.
      - Generators can't be detected without an instance. They fall through to the grouping heuristic. Accept that, and note it in a comment on the function.
      - Get the colour from `Settings::getNodeCategoryColor`.
    - Shortcut: the plugin's keybind in its shortcut group (`Plugin::getPluginShortcutGroup()`, `getHasShortcut()`), looked up through the `GuiApplicationManager` keybind API that `isKeybind` uses. Leave it empty when the plugin has none.
  - verify:
    - `ctest -R NodeCategory`: new cases show that `categoryForPlugin` agrees with `getNodeCategory()` for a Read, a Write, a Deep node, a Grade, a Blur and a Merge created in the test app.
    - At the gate, P4.T1's script finds Blur's row coloured with the Filter category colour.
  - size: M

- [ ] M45.P1.T3 — Add the Tab menu preferences (search-mode remap, scroll toggle) to the Nodegraph page, with accessors
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Tests/TabMenuSettings_Test.cpp` (new), `Tests/CMakeLists.txt` (one line in the `Tests` binary's source list, beside `NodeCategory_Test.cpp`)
  - approach:
    - These are tabtabtab's `space_mode_order` and `scroll_enabled`. Its `tabtabtab_enabled` toggle is not ported, because there is no fallback dialog (Decisions).
    - Keep every edit additive, because M56 - Visual And Menu Polish also edits these files: it removes `_useBWIcons` from the Nodegraph members and defaults, and adds an accessor near `Settings.h:306`. Append; do not reorder or reformat existing lines.
    - In `Settings::initializeKnobsNodeGraph()` (`Settings.cpp:1210`), after `_nodegraphTab->addKnob(_useInputAForMergeAutoConnect);`, add:
      - `_tabMenuSeparator = AppManager::createKnob<KnobSeparator>(this, tr("Tab menu"))`, named `tabMenuSeparator`, as the heading;
      - `_tabMenuSearchModes` (`KnobChoice`, label "Search modes (no / one / two leading spaces)", name `tabMenuSearchModes`). It has six `ChoiceOption`s, one per ordering of Anchored fuzzy / Non-anchored fuzzy / Consecutive. Use stable IDs such as `anchored,nonanchored,consecutive` and labels such as "Anchored / Non-anchored / Consecutive". A single choice can only hold a valid permutation, so no validation is needed. The tooltip explains that the number of leading spaces in the query picks the mode, and that `[` searches the category;
      - `_tabMenuScroll` (`KnobBool`, label "Scroll through all results", name `tabMenuScroll`). Its tooltip says that when it is off, only the visible rows are kept, as in tabtabtab.
      - Add each with `_nodegraphTab->addKnob`.
    - Declare the three `Knob*Ptr` members after `_useAntiAliasing` in the `// Nodegraph` block (`Settings.h:576`).
    - In `setDefaultValues()`, add a separate `// Tab menu` block after `_useAntiAliasing->setDefaultValue(true);`: choice 0 (Anchored / Non-anchored / Consecutive) and scroll `true`.
    - Accessors, declared after `isNodeGraphAntiAliasingEnabled()` (`Settings.h:332`) and defined after its body (`Settings.cpp:~3427`):
      - `void getTabMenuSpaceModes(int* noSpace, int* oneSpace, int* twoSpaces) const`, which maps the choice index to the permutation using 0 = Anchored, 1 = NonAnchored, 2 = Consecutive (the same values as P1.T1's `TabMenuMatchMode`);
      - `bool isTabMenuScrollEnabled() const`.
    - Nothing goes in `onKnobValueChanged`, because the TabMenu reads the values each time it opens (P2.T1).
  - verify: `ctest -R TabMenuSettings` checks, on `appPTR->getCurrentSettings()` (look the knobs up by name with `getKnobByNameAndType`, as `GLScheduler_Test.cpp:114` does, and restore the old value afterwards):
    - the defaults give modes (0,1,2) and scrolling on;
    - each of the six choice indices gives a distinct permutation of {0,1,2};
    - turning `tabMenuScroll` off makes the accessor return false.
    - The page layout is checked by eye in the UAT: a "Tab menu" heading with both controls on the Nodegraph page.
  - size: M

## Phase 45.2: The palette

- [ ] M45.P2.T1 — Write the TabMenu popup: line edit, coloured result list, keys, and correct first-show position
  - files: `Gui/TabMenu.h` (new), `Gui/TabMenu.cpp` (new)
  - approach:
    - A frameless top-level `QWidget` (`Qt::Popup` or `Qt::Dialog|FramelessWindowHint`, whichever closes reliably on an outside click under xcb), containing:
      - a `LineEdit`;
      - a `QListView` on a `QAbstractListModel` over P1.T1's ranked results.
    - It is one window, not today's separate `Qt::ToolTip` list window. That removes the two-window position race.
    - A delegate paints each row, as in tabtabtab's `_ItemDelegate` (`tabtabtab_nuke_core.py`):
      - a solid category block at the left;
      - the icon;
      - the row background washed with the category colour at about 31% alpha;
      - the text colour from `NodeGui::contrastingLabelTextColor`;
      - "Name [Category]" with the shortcut right-aligned.
      - Rows with no category colour get no block and no wash.
    - Fixed height of about 18 visible rows. Read P1.T3's preferences from `appPTR->getCurrentSettings()` on every open, not in the constructor (P2.T2 reuses one instance), so a change applies the next time it opens:
      - pass `getTabMenuSpaceModes()` to the P1.T1 matcher as its mode order;
      - if `isTabMenuScrollEnabled()` (the default), keep every match and let the list scroll; if not, keep only the first 18 ranked rows and turn the scroll bar off (tabtabtab's `scroll_enabled`).
    - Keys:
      - Tab, Enter or Return → choose the current row;
      - Up and Down wrap around;
      - Escape closes;
      - losing activation closes;
      - typing re-filters and selects row 0.
    - Signal: `itemChosen(TabMenuItem)`.
    - Position: compute the final geometry before the first `show()`. Put the line edit under the cursor (x = cursor − width/2, y = cursor − ~13 px) and clamp to `QGuiApplication::screenAt(cursor)`'s available geometry. Never call `move()` after `show()`, and never show the list before the popup is positioned. That fixes M69's jump.
    - On open: pre-fill `" " + lastChosenDisplayName` and select all, so Tab-Tab repeats the last node (tabtabtab's leading-space trick).
    - Stylesheet: the existing `LineEdit`/`QListView` rules. Add a `TabMenu` object name only if the default look is wrong.
  - verify: P4.T1 at the gate:
    - the popup's first mapped geometry already contains the cursor, with no Move event after the first Show;
    - "blr" puts Blur first;
    - Up from row 0 wraps to the last row;
    - Escape and an outside click both close it without creating anything;
    - with scrolling turned off in Preferences, "a" shows at most 18 rows and no scroll bar;
    - shots of the coloured list.
  - size: L

- [ ] M45.P2.T2 — Wire the TabMenu into the node graph and honour the rebindable shortcut
  - files: `Gui/NodeGraph25.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphPrivate.cpp`
  - approach:
    - Rewrite `NodeGraph::popCreateNodeDialog` and `onCreateNodeDialogFinished` (`NodeGraph25.cpp:190-225`) around `TabMenu`. Keep one instance per graph and hide it rather than delete it, so the pre-fill and the list survive between opens.
    - On `itemChosen` for a node item:
      - build `CreateNodeArgs` exactly as today (the position hint at the cursor, `kCreateNodeArgsPropPluginVersion`, `getGroup()`);
      - increment the weight;
      - store `_lastNodeCreatedName`.
    - Command items are handled by P3.T2.
    - `NodeGraph::event` (`:169-187`) hard-codes `Qt::Key_Tab`, so rebinding "Create Node" in the shortcut editor does nothing today. Replace that check with `isKeybind(kShortcutGroupNodegraph, kShortcutIDActionGraphCreateNode, modifiers, key)`. It has to stay in `event()`, because `QGraphicsView` takes Tab for focus traversal before `keyPressEvent`. Delete the unreachable branch and its `assert(false)` in `keyPressEvent` (`:386-388`).
    - Keep the `_nodeCreationShortcutEnabled` enter/leave gating (`NodeGraph30.cpp:141-156`).
    - The right-click menu's "Create Node" (`NodeGraph35.cpp:491`) still calls `popCreateNodeDialog`.
  - verify: P4.T1 at the gate:
    - Tab over the graph opens the menu, and choosing Blur creates a Blur at the cursor;
    - with a Read selected, the Blur is connected below it (today's `moveNodesForIdealPosition` behaviour);
    - Tab, Tab immediately recreates the last node;
    - Tab inside a Group's graph creates the node in that group;
    - after rebinding Create Node to another key in the shortcut editor, that key opens the menu and Tab does not.
  - size: M

- [ ] M45.P2.T3 — Remove NodeCreationDialog, keeping CompleterLineEdit for its other users
  - files: `Gui/NodeCreationDialog.h`, `Gui/NodeCreationDialog.cpp`
  - approach:
    - Delete the `NodeCreationDialog` class and its private struct. Keep `CompleterLineEdit` in the same files, because `PickKnobDialog.cpp`, `LinkToKnobDialog.cpp`, `KnobGuiPrivate.h`, `EditScriptDialog.cpp` and `EditExpressionDialog.cpp` include it. Renaming the files is not worth touching those five.
    - In `CompleterLineEdit`, drop the `dynamic_cast<NodeCreationDialog*>` branches in `setTextFromIndex` and `keyPressEvent`, so quick-exit always calls `_imp->dialog->accept()`.
    - Leave the list-positioning code alone. Its remaining users are modal dialogs that are already shown before any typing happens.
  - verify: at the gate build, `grep -rn NodeCreationDialog Gui/` finds nothing, and the Pick Knob and Link-to-Knob dialogs still complete node names (checked by hand in the UAT).
  - size: M

## Phase 45.3: Smart insertion and menu commands

- [ ] ~~M45.P3.T1 — With nothing selected, splice the new node into the pipe under the cursor~~ (cancelled 2026-10-11 by the user: insert-after-selected only)
  - files: `Gui/NodeGraph25.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach:
    - When the menu opens, and only if the selection is empty, call `hasItemNearbyMouse` at the cursor. If the result is `eNearbyItemEdge` on an input edge with a source, record the edge as `(destination NodeGui weak pointer, input index)`, not as an `Edge*`, because the graph may change while the menu is open.
    - On `itemChosen`, if a pipe was recorded and still exists:
      - set `kCreateNodeArgsPropAutoConnect` to false;
      - set the position hint to the pipe point closest to the cursor;
      - `getUndoStack()->beginMacro(tr("Create and insert %1"))`;
      - `createNode`;
      - push M55's generalised `InsertNodeCommand(graph, edge, newGui)`, with the entry and exit both set to the new node and the entry input set to its preferred input;
      - `endMacro()`.
    - If `canConnectInput` rejects either side, create the node unconnected at the cursor.
    - A selection always wins over a pipe, which keeps today's behaviour.
    - Optional: tint the recorded pipe while the menu is open, using M55's highlight, so the user can see where the node will go. Do it only if it's a one-line call to the existing hint API.
  - verify: P4.T1 at the gate:
    - with nothing selected and the cursor on a Read→Viewer pipe, Tab → Blur gives Read→Blur→Viewer;
    - one Ctrl+Z removes the Blur and restores Read→Viewer;
    - with the cursor over empty space, Blur is created unconnected;
    - a Read (no inputs) chosen over a pipe is created unconnected.
  - size: M

- [ ] M45.P3.T2 — Add main-menu commands to the palette
  - files: `Gui/TabMenuItems.h`, `Gui/TabMenuItems.cpp`, `Gui/NodeGraph25.cpp`
  - approach:
    - `buildMenuCommandItems(QMenuBar*)` walks `getGui()->menuBar()` recursively (`Gui` is a `QMainWindow`; the menu bar is built in `Gui::createMenuActions`, `Gui/Gui.cpp:438-450`; Preferences is added to Edit at `:696`) and adds every enabled, non-separator, non-submenu `QAction`:
      - display name "Exit [File]"; deeper menus give "Item [File/Sub]";
      - category = the menu path;
      - shortcut = `action->shortcut().toString(QKeySequence::NativeText)`;
      - no colour; weight key `menu:<path>/<text>`.
    - Strip `&` from all text.
    - Rebuild the list on every open: it's cheap, and Python-added menu commands (`app.addMenuCommand`) appear without a cache.
    - On choosing a command item, close the menu, then trigger the action on the next event-loop tick (`QTimer::singleShot(0, action, &QAction::trigger)`, guarded by a `QPointer`), so modal dialogs it opens don't fight the closing popup.
  - verify: P4.T1 at the gate:
    - "pref" lists "Preferences... [Edit]", and choosing it opens the Preferences panel;
    - a node query ("blr") still ranks Blur first;
    - an action disabled in the current state is absent.
  - size: M

## Phase 45.4: Tests and UAT

- [ ] M45.P4.T1 — Write a GUI test script for the tab menu
  - files: `Tests/gui/m45_tab_menu.py` (new)
  - approach:
    - Model it on `Tests/gui/m55_node_graph.py` (watchdog, `gt.check`/`shot`).
    - Drive the graph with `QTest.keyClick` on the `NodeGraph` viewport after an enter event, as `m75_read.py:398` does.
    - Record the popup's first-show position with an event filter that logs Show/Move events, so the M69 jump is a hard failure.
    - Cover every verify bullet in P2.T1, P2.T2 and P3.T2, plus P1.T3 in the live menu: after remapping the search modes in Preferences, " blr" behaves as the new mode.
    - Take shots of: the open menu with coloured rows, the `[` category filter, a menu-command query, and the graph after a Blur is inserted below a selected Read.
  - verify: `NATRON_GUI_TEST_OUT=build/m45-shots Tests/gui/run-gui-test.sh Tests/gui/m45_tab_menu.py build/fast/App/Natron` reports 0 failures at the gate build.
  - size: M

- [ ] M45.P4.T2 — Run the one build, the tests, and package the AppImage for UAT
  - files: none (build, run, package)
  - approach: configure (new files are globbed, so re-run cmake), build, then run `ctest`, `m45_tab_menu.py`, `m55_node_graph.py`, `m55_input_pipes.py` and `build/m24-shots/m24_border.py` (untracked; run it from that path). Package `build/appimages/M45-<sha>.AppImage`.
  - verify: everything is green, and the AppImage path is posted for the user's UAT.
  - size: M

**Verification gate:** CI `format`, `lint-ci` and `build-and-test` are green, including the new `TabMenuMatch`, `TabMenuSettings` and extended `NodeCategory` cases. `Tests/gui/m45_tab_menu.py`, `m55_node_graph.py`, `m55_input_pipes.py` and `build/m24-shots/m24_border.py` report 0 failures on the gate build. The user signs off the shots and the AppImage UAT, which covers: the menu opening under the cursor with no jump, "blr" → Blur, Tab-Tab repeat, an insert below a selected node, a menu-command search, a rebound shortcut, and the two Tab menu preferences (a remapped search mode, and scrolling off).

Acceptance sketch:
- Pressing the tab-menu shortcut in the node graph opens a fuzzy-searchable
  create-node menu that inserts after the selected node, with tabtabtab's
  search-mode and scroll preferences.

## Decisions

- 2026-10-11 — **Deps `M55` (user-confirmed).** M45 also takes over M69 - Minor Cleanup's "Tab menu position" jump (M69.P2.T5 is cancelled); P2.T1's no-move-after-show positioning fixes it.
- 2026-10-11 — **User answers to the elaboration questions** (`PLAN/DESIGN/2026-10-11-m45-m46-questions.md`):
  - **Insertion:** today's insert-after-the-selected-node only. **M45.P3.T1 (pipe splice) is cancelled.** The M55 dependency now rests only on the post-M55 node-graph files and `m55_node_graph.py` in the gate.
  - **Command search:** the main menu bar only (P3.T2 as written).
  - **Preferences: port tabtabtab's preferences now** (search-mode remap and scroll toggle). A new task in `Engine/Settings.*` is needed; it is added at promotion. Note it overlaps M56 - Visual And Menu Polish's P4.T6 in `Engine/Settings.*`.
  - **Generators** are coloured by their grouping (no probe instance), and there is **no fallback** to the old dialog.
- 2026-10-11 — **Promotion freshness check** (branch `milestone/m45-native-tab-menu` from `main` at `a0a6336e8`, after M55 and M44). All files exist and the M24 APIs keep their names. Line numbers were corrected in P2.T2 and P3.T2. tabtabtab is now cited by function name. `m24_border.py` is untracked at `build/m24-shots/m24_border.py`; P4.T2 and the gate run it from there, alongside the added `m55_input_pipes.py`. The pipe-splice references were removed from the verify text, the gate and the acceptance sketch. **M45.P1.T3** was added for tabtabtab's preferences.
- 2026-10-11 — **Preference defaults (coordinator):** P1.T3 covers only the `Settings` side and P2.T1 reads the preferences on every open, so P1.T3's verify does not depend on P2.T1. The search-mode remap is a single six-way choice, which can only hold a valid permutation. tabtabtab's "Enable tabtabtab" toggle is not ported, because there is no fallback dialog.
