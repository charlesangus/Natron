"""tabtabtab adapter for Natron.

Node items come from the process-wide nodes menu (natron.nodesMenu()),
command items from the main window's menu bar, and the preferences from
Natron's Settings. Every item is a QAction; GuiApp triggers it so that the
nodes it creates land in the graph the menu was opened over.
"""

import os
import re

import NatronGui
import shiboken6
from PySide6 import QtCore, QtGui, QtWidgets

import tabtabtab_nuke_core
from tabtabtab_nuke_core import (
    DEFAULT_SPACE_MODE_ORDER,
    TabTabTabPlugin,
    find_qt_menu_items,
    set_main_window_finder,
    set_row_metrics,
)


_NATRON_MAIN_WINDOW_CLASSNAME = "Natron::Gui"
_WEIGHTS_FILENAME = "tabtabtab_weights.json"
_DEFAULT_STORAGE_DIR = "~/.Natron"

_SEARCH_MODES_PARAM = "tabMenuSearchModes"
_SCROLL_PARAM = "tabMenuScroll"

_PATH_SEPARATOR_STANDIN = "∕"


def _find_natron_main_window():
    """Return the Natron main window owning the active window, or None.

    Walking up from the active window also covers a node graph in a
    floating pane, and picks the right window when several are open.
    """
    app = QtWidgets.QApplication.instance()
    if app is None:
        return None

    widget = app.activeWindow()
    while widget is not None:
        if widget.metaObject().className() == _NATRON_MAIN_WINDOW_CLASSNAME:
            return widget
        widget = widget.parentWidget()

    for widget in app.topLevelWidgets():
        if (isinstance(widget, QtWidgets.QMainWindow)
                and widget.metaObject().className() == _NATRON_MAIN_WINDOW_CLASSNAME):
            return widget

    return None


_ROW_HEIGHT_PADDING = 6
_VISIBLE_ROWS = 15


def _natron_row_height():
    """Font height plus a little padding — the row height Natron's own
    lists and menus use — instead of the core's default (twice the font
    height), which makes the popup far taller than fits most screens.
    QFontMetrics is already DPI-scaled, so no extra scaling is needed here.
    """
    metrics = QtGui.QFontMetrics(QtWidgets.QApplication.font())
    return metrics.height() + _ROW_HEIGHT_PADDING


def _is_alive(obj):
    try:
        return obj is not None and shiboken6.isValid(obj)
    except RuntimeError:
        return False


def _launch_window():
    return _plugin.main_window()


set_main_window_finder(_launch_window)
set_row_metrics(row_height=_natron_row_height(), visible_rows=_VISIBLE_ROWS)


def _path_element(text):
    # The core splits each menupath on its last "/" to build "Name [Group]".
    return text.replace("/", _PATH_SEPARATOR_STANDIN)


_COLOR_PROPERTY = "natronCategoryColor"
_SHORTCUT_PROPERTY = "natronShortcut"
_INPUT_KIND_PROPERTY = "natronInputKind"
_PRIM_PATH_KNOB_PROPERTY = "natronPrimPathKnob"

# A first input of this kind takes any kind.
_ANY_KIND = "polymorphic"

_MNEMONIC = re.compile(r"&(&?)")


def _action_text(action):
    # Menu text doubles a literal "&" and marks a mnemonic with a single one.
    return _MNEMONIC.sub(r"\1", action.text())


def _string_property(action, name):
    try:
        value = action.property(name)
    except Exception:
        return ""
    return value if isinstance(value, str) else ""


def _walk_menu(menu, parts):
    items = []
    for action in menu.actions():
        if not (action.isVisible() and action.isEnabled()):
            continue
        text = _action_text(action)
        if not text:
            continue
        path = parts + [_path_element(text)]
        submenu = action.menu()
        if submenu is not None:
            if submenu.isEnabled():
                items.extend(_walk_menu(submenu, path))
            continue
        item = {'menuobj': action, 'menupath': "/".join(path)}
        shortcut = _string_property(action, _SHORTCUT_PROPERTY)
        if not shortcut:
            shortcut = action.shortcut().toString(QtGui.QKeySequence.NativeText)
        if shortcut:
            item['shortcut'] = shortcut
        items.append(item)
    return items


def _node_items():
    try:
        menu = NatronGui.natron.nodesMenu()
    except Exception:
        return []
    if not _is_alive(menu):
        return []
    return _walk_menu(menu, [])


def _accepts(action, context):
    """Whether the node type of `action` fits a launch `context`: its first
    input takes context['inputKind'], and it declares a prim path knob when
    context['needsPrimPathKnob'] says so."""
    kind = context.get('inputKind')
    if kind:
        accepted = _string_property(action, _INPUT_KIND_PROPERTY)
        if accepted not in (kind, _ANY_KIND):
            return False
    if context.get('needsPrimPathKnob') and not _string_property(action, _PRIM_PATH_KNOB_PROPERTY):
        return False
    return True


def _command_items(window):
    if window is None:
        return []
    menubar = window.menuBar()
    if menubar is None:
        return []

    items = find_qt_menu_items(menubar)
    for item in items:
        shortcut = item['menuobj'].shortcut().toString(QtGui.QKeySequence.NativeText)
        if shortcut:
            item['shortcut'] = shortcut
    return items


def _make_menupaths_unique(items):
    # The core's row diff and the weights file are both keyed on menupath.
    seen = {}
    for item in items:
        path = item['menupath']
        count = seen.get(path, 0) + 1
        seen[path] = count
        if count > 1:
            item['menupath'] = "%s (%d)" % (path, count)
    return items


def _trigger_action(app, action):
    try:
        if not shiboken6.isValid(action):
            return
    except RuntimeError:
        return
    if app is not None:
        app.triggerTabMenuAction(action)
    else:
        action.trigger()


class NatronPlugin(TabTabTabPlugin):
    def __init__(self):
        self._app = None
        self._window = None
        self._storage_dir = None
        self._pending = []
        self._context = None

    def set_app(self, app):
        self._app = app

    def set_window(self, window):
        self._window = window

    def set_context(self, context):
        self._context = context

    def main_window(self):
        # The popup itself becomes the active window once shown, so the
        # deferred refresh must not rediscover the window from focus: that
        # walks the popup's parent chain, which may lead to another project.
        if _is_alive(self._window):
            return self._window
        return _find_natron_main_window()

    def set_storage_dir(self, path):
        self._storage_dir = path

    def get_items(self):
        if self._context:
            # A filtered launch lists node types only.
            nodes = [item for item in _node_items() if _accepts(item['menuobj'], self._context)]
            return _make_menupaths_unique(nodes)
        return _make_menupaths_unique(_node_items() + _command_items(self.main_window()))

    def get_weights_file(self):
        if self._storage_dir is not None:
            return os.path.join(self._storage_dir, _WEIGHTS_FILENAME)
        return os.path.join(os.path.expanduser(_DEFAULT_STORAGE_DIR), _WEIGHTS_FILENAME)

    def invoke(self, thing):
        action = thing['menuobj']
        app = self._app

        # The core calls invoke() before it closes the popup. Running the
        # choice now would open its windows (a Read's file dialog, the
        # Preferences) while the popup is still up.
        self._pending.append(lambda: _trigger_action(app, action))
        QtCore.QTimer.singleShot(0, self._run_pending)

    def _run_pending(self):
        while self._pending:
            self._pending.pop(0)()

    def get_color(self, menuobj):
        color = _string_property(menuobj, _COLOR_PROPERTY)
        if color:
            return (QtGui.QColor(color), QtGui.QColor(color))
        return (None, None)


_plugin = NatronPlugin()


def set_storage_dir(path):
    """Keep the weights file in `path` instead of ~/.Natron."""
    _plugin.set_storage_dir(path)


def _read_preferences():
    space_mode_order = list(DEFAULT_SPACE_MODE_ORDER)
    scroll_enabled = True
    try:
        settings = NatronGui.natron.getSettings()

        param = settings.getParam(_SEARCH_MODES_PARAM)
        if param is not None:
            order = param.getOption(param.getValue()).split(",")
            if sorted(order) == sorted(DEFAULT_SPACE_MODE_ORDER):
                space_mode_order = order

        param = settings.getParam(_SCROLL_PARAM)
        if param is not None:
            scroll_enabled = bool(param.getValue())
    except Exception:
        return list(DEFAULT_SPACE_MODE_ORDER), True
    return space_mode_order, scroll_enabled


def _attach_popup_to(window):
    # The popup is cached process-wide by the core; keep it owned by, and
    # stacked with, the window it is opened from.
    popup = tabtabtab_nuke_core._tabtabtab_instance
    if window is None or not _is_alive(popup):
        return
    try:
        if popup.parent() is not window:
            popup.setParent(window, popup.windowFlags())
    except RuntimeError:
        pass


def launch(app, context=None):
    """Open the tab menu for `app`, the GuiApp whose node graph has focus.

    `context`, when given, lists only the node types that fit it, see
    _accepts(): a dict with 'inputKind' (a kind name such as "scene") and
    'needsPrimPathKnob'.
    """
    window = _find_natron_main_window()
    _plugin.set_app(app)
    _plugin.set_context(dict(context) if context else None)
    _plugin.set_window(window)
    _attach_popup_to(window)
    space_mode_order, scroll_enabled = _read_preferences()
    tabtabtab_nuke_core.launch(
        _plugin,
        space_mode_order=space_mode_order,
        scroll_enabled=scroll_enabled,
    )
