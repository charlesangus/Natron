"""tabtabtab adapter for Natron.

Node items come from the C++ bridge on GuiApp, command items from the main
window's menu bar, and the preferences from Natron's Settings.
"""

import json
import os

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


class _NodeEntry(object):
    __slots__ = ("id", "major", "icon", "color")

    def __init__(self, plugin_id, major, icon, color):
        self.id = plugin_id
        self.major = major
        self.icon = icon
        self.color = color


def _node_items(app):
    if app is None:
        return []

    items = []
    for entry in json.loads(app.getTabMenuNodeItems()):
        parts = [_path_element(g) for g in entry.get("grouping", [])]
        parts.append(_path_element(entry["label"]))
        item = {
            'menuobj': _NodeEntry(entry["id"], entry["major"],
                                  entry.get("icon", ""), entry.get("color", "")),
            'menupath': "/".join(parts),
        }
        shortcut = entry.get("shortcut", "")
        if shortcut:
            item['shortcut'] = shortcut
        items.append(item)
    return items


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


def _trigger_action(action):
    try:
        if shiboken6.isValid(action):
            action.trigger()
    except RuntimeError:
        pass


class NatronPlugin(TabTabTabPlugin):
    def __init__(self):
        self._app = None
        self._window = None
        self._storage_dir = None
        self._icon_cache = {}
        self._pending = []

    def set_app(self, app):
        self._app = app

    def set_window(self, window):
        self._window = window

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
        return _make_menupaths_unique(_node_items(self._app) + _command_items(self.main_window()))

    def get_weights_file(self):
        if self._storage_dir is not None:
            return os.path.join(self._storage_dir, _WEIGHTS_FILENAME)
        return os.path.join(os.path.expanduser(_DEFAULT_STORAGE_DIR), _WEIGHTS_FILENAME)

    def invoke(self, thing):
        menuobj = thing['menuobj']
        if isinstance(menuobj, _NodeEntry):
            app = self._app
            if app is None:
                return
            callback = lambda: app.createNodeFromTabMenu(menuobj.id, menuobj.major)
        else:
            callback = lambda: _trigger_action(menuobj)

        # The core calls invoke() before it closes the popup. Running the
        # choice now would open its windows (a Read's file dialog, the
        # Preferences) while the popup is still up.
        self._pending.append(callback)
        QtCore.QTimer.singleShot(0, self._run_pending)

    def _run_pending(self):
        while self._pending:
            self._pending.pop(0)()

    def get_icon(self, menuobj):
        if not isinstance(menuobj, _NodeEntry):
            return super(NatronPlugin, self).get_icon(menuobj)

        path = menuobj.icon
        if not path:
            return None
        icon = self._icon_cache.get(path)
        if icon is None:
            # Native nodes' icons live in Natron's Qt resources (":/..."),
            # which only QFile can see.
            if not QtCore.QFile.exists(path):
                return None
            icon = QtGui.QIcon(path)
            self._icon_cache[path] = icon
        return icon

    def get_color(self, menuobj):
        if isinstance(menuobj, _NodeEntry) and menuobj.color:
            return (QtGui.QColor(menuobj.color), QtGui.QColor(menuobj.color))
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


def launch(app):
    """Open the tab menu for `app`, the GuiApp whose node graph has focus."""
    window = _find_natron_main_window()
    _plugin.set_app(app)
    _plugin.set_window(window)
    _attach_popup_to(window)
    space_mode_order, scroll_enabled = _read_preferences()
    tabtabtab_nuke_core.launch(
        _plugin,
        space_mode_order=space_mode_order,
        scroll_enabled=scroll_enabled,
    )
