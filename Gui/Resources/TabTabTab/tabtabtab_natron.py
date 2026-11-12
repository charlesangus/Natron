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
_PLUGIN_ID_PROPERTY = "natronPluginID"
_INPUT_KIND_PROPERTY = "natronInputKind"
_PRIM_PATH_KNOB_PROPERTY = "natronPrimPathKnob"
_INPUT_KINDS_PROPERTY = "natronInputKinds"

# PLUGINID_NATRON_READ and PLUGINID_NATRON_WRITE, whose interactive creation
# asks for a file first.
_FILE_DIALOG_PLUGIN_IDS = frozenset(("Read", "Write"))

# An input of this kind takes any kind.
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


def _filter_info(action):
    """What _accepts() reads from a node action, read once per walk."""
    kinds = tuple(k for k in _string_property(action, _INPUT_KINDS_PROPERTY).split(",") if k)
    return (
        _string_property(action, _INPUT_KIND_PROPERTY),
        bool(_string_property(action, _PLUGIN_ID_PROPERTY)),
        kinds,
        bool(_string_property(action, _PRIM_PATH_KNOB_PROPERTY)),
    )


def _accepts_info(info, context):
    input_kind, has_plugin, input_kinds, has_prim_knob = info
    kind = context.get('inputKind')
    if kind and input_kind not in (kind, _ANY_KIND):
        return False
    selected = context.get('selectedKind')
    # A script's action declares no inputs, which must not read as a source
    # node's empty list: nothing says it cannot use the selection.
    if selected and has_plugin:
        if selected not in input_kinds and _ANY_KIND not in input_kinds:
            return False
    if context.get('needsPrimPathKnob') and not has_prim_knob:
        return False
    return True


def _accepts(action, context):
    """Whether the node type of `action` fits a launch `context`: its first
    input takes context['inputKind'], one of its inputs other than masks
    takes context['selectedKind'], and it declares a prim path knob when
    context['needsPrimPathKnob'] says so. An action with no plug-in behind
    it passes context['selectedKind'] as a menu command does."""
    return _accepts_info(_filter_info(action), context)


# The walked menus are watched for the events Qt sends a menu when one of its
# actions is added, removed or changed (text, shortcut, visibility, enabled
# state). A menu's walk is kept until such an event, the menu's destruction
# or invalidate_items() drops it, so an open re-walks only what changed: the
# Edit menu after an undo step, say, not the nodes menu. A menu that cannot be
# watched is walked on every call. The versions, one per kind of walk, tell
# the lists built from the walks when to be rebuilt.
#
# Menus are keyed by their C++ address, not by their wrapper. PySide makes the
# wrapper action.menu() returns a child of the action's wrapper and
# invalidates it once the action's wrapper is collected, although the menu
# lives on; asking again gives a new wrapper for the same menu. A walk
# therefore keeps each submenu's action, and asks it for the menu when the
# submenu is walked.
_segments = {}
_owners = {}
_watched = set()
_destroy_hooked = set()
_versions = {'nodes': 0, 'commands': 0}
_command_caches = {}
_menu_watcher = None
_menu_change_events = None


def _change_events():
    global _menu_change_events
    if _menu_change_events is None:
        names = ("ActionAdded", "ActionRemoved", "ActionChanged")
        _menu_change_events = tuple(
            getattr(QtCore.QEvent, name) for name in names if hasattr(QtCore.QEvent, name))
    return _menu_change_events


def _enabled_change_event():
    return getattr(QtCore.QEvent, "EnabledChange", None)


def _menu_id(menu):
    try:
        return shiboken6.getCppPointer(menu)[0]
    except (AttributeError, TypeError, IndexError, RuntimeError):
        return None


def _submenu_of(action):
    """The live menu `action` opens, or None."""
    try:
        if not shiboken6.isValid(action):
            return None
        menu = action.menu()
    except RuntimeError:
        return None
    return menu if _is_alive(menu) else None


def _menu_changed(menu_id):
    for key in _owners.get(menu_id, ()):
        _segments.pop(key, None)
        _versions[key[0]] += 1


def _menu_destroyed(menu_id):
    # A menu made later may be given the same address.
    _menu_changed(menu_id)
    _owners.pop(menu_id, None)
    _watched.discard(menu_id)
    _destroy_hooked.discard(menu_id)
    _command_caches.pop(menu_id, None)


def _hook_destroyed(menu, menu_id):
    if menu_id in _destroy_hooked:
        return
    try:
        menu.destroyed.connect(lambda *_args: _menu_destroyed(menu_id))
    except (AttributeError, TypeError, RuntimeError):
        return
    _destroy_hooked.add(menu_id)


def invalidate_items():
    """Drop every cached menu walk, for changes no menu event reports: the
    nodes menu rebuilt, or a node action's shortcut or colour property set."""
    _segments.clear()
    _owners.clear()
    _command_caches.clear()
    for kind in _versions:
        _versions[kind] += 1


def _watch(menu, owner):
    """Drop the walk cached under `owner` when `menu` changes; False when
    `menu` cannot be watched, and so its walk must not be cached."""
    global _menu_watcher
    menu_id = _menu_id(menu)
    if menu_id is None:
        return False
    if menu_id not in _watched:
        if _menu_watcher is None:
            class _MenuWatcher(QtCore.QObject):
                def eventFilter(self, watched, event):
                    kind = event.type()
                    if kind in _change_events():
                        _menu_changed(_menu_id(watched))
                    elif kind == _enabled_change_event():
                        # A disabled submenu drops out of its parent's walk.
                        invalidate_items()
                    return False

            _menu_watcher = _MenuWatcher()
        try:
            menu.installEventFilter(_menu_watcher)
        except (AttributeError, TypeError, RuntimeError):
            return False
        _watched.add(menu_id)
        _hook_destroyed(menu, menu_id)
    _owners.setdefault(menu_id, set()).add(owner)
    return True


def _node_segment(menu, parts):
    """The walk of `menu` alone: (item, filter info, submenu action, path) per
    entry, an entry with a submenu action standing for that submenu's own
    walk."""
    menu_id = _menu_id(menu)
    key = ('nodes', menu_id)
    cached = _segments.get(key) if menu_id is not None else None
    if cached is not None and cached[0] == parts:
        return cached[1], True
    entries = []
    for action in menu.actions():
        if not (action.isVisible() and action.isEnabled()):
            continue
        text = _action_text(action)
        if not text:
            continue
        path = parts + (_path_element(text),)
        submenu = action.menu()
        if submenu is not None:
            if submenu.isEnabled():
                entries.append((None, None, action, path))
            elif menu_id is not None:
                _watch(submenu, key)
            continue
        item = {'menuobj': action, 'menupath': "/".join(path)}
        shortcut = _string_property(action, _SHORTCUT_PROPERTY)
        if not shortcut:
            shortcut = action.shortcut().toString(QtGui.QKeySequence.NativeText)
        if shortcut:
            item['shortcut'] = shortcut
        entries.append((item, _filter_info(action), None, None))
    cacheable = menu_id is not None and _watch(menu, key)
    if cacheable:
        _segments[key] = (parts, entries)
    return entries, cacheable


def _collect_nodes(menu, parts, items, infos, rewalk=True):
    start = len(items)
    entries, cacheable = _node_segment(menu, parts)
    for item, info, action, path in entries:
        if action is None:
            items.append(item)
            infos.append(info)
            continue
        submenu = _submenu_of(action)
        if submenu is None:
            # The submenu is gone and its menu reported no change, so the
            # cached walk of the menu is stale.
            _segments.pop(('nodes', _menu_id(menu)), None)
            if rewalk:
                del items[start:]
                del infos[start:]
                return _collect_nodes(menu, parts, items, infos, False)
            cacheable = False
            continue
        cacheable = _collect_nodes(submenu, path, items, infos) and cacheable
    return cacheable


_node_cache = None

# Returned for "no items", never modified: get_items() tells a list unchanged
# by its identity.
_NO_ITEMS = []


def _node_items():
    """(items, filter infos) of the nodes menu; the same lists until a
    watched menu changes."""
    global _node_cache
    try:
        menu = NatronGui.natron.nodesMenu()
    except Exception:
        return _NO_ITEMS, _NO_ITEMS
    if not _is_alive(menu):
        return _NO_ITEMS, _NO_ITEMS
    version = _versions['nodes']
    menu_id = _menu_id(menu)
    if (_node_cache is not None and _node_cache[0] == version and menu_id is not None
            and _node_cache[1] == menu_id):
        return _node_cache[2], _node_cache[3]
    items = []
    infos = []
    if _collect_nodes(menu, (), items, infos) and version == _versions['nodes']:
        _node_cache = (version, menu_id, items, infos)
    return items, infos


def _with_shortcuts(items):
    for item in items:
        shortcut = item['menuobj'].shortcut().toString(QtGui.QKeySequence.NativeText)
        if shortcut:
            item['shortcut'] = shortcut
    return items


def _command_segment(menu, title):
    menu_id = _menu_id(menu)
    key = ('commands', menu_id)
    cached = _segments.get(key) if menu_id is not None else None
    if cached is not None and cached[0] == title:
        return cached[1]
    items = _with_shortcuts(tabtabtab_nuke_core._traverse_qt_menu(menu, _path=title))
    if menu_id is not None and _watch_tree(menu, key):
        _segments[key] = (title, items)
    return items


def _watch_tree(menu, owner):
    watched = _watch(menu, owner)
    for action in menu.actions():
        submenu = action.menu()
        if submenu is not None:
            watched = _watch_tree(submenu, owner) and watched
    return watched


def _command_items(window):
    """The menu bar's commands, as find_qt_menu_items() lists them; the same
    list until a watched menu changes."""
    if window is None:
        return _NO_ITEMS
    menubar = window.menuBar()
    if menubar is None:
        return _NO_ITEMS
    menubar_id = _menu_id(menubar)
    key = ('commands', menubar_id)
    if menubar_id is None or not _watch(menubar, key):
        return _with_shortcuts(find_qt_menu_items(menubar))
    cached = _command_caches.get(menubar_id)
    version = _versions['commands']
    if cached is not None and cached[0] == version:
        return cached[1]
    tops = _segments.get(key)
    if tops is None:
        tops = [(action, action.text()) for action in menubar.actions() if action.menu()]
        _segments[key] = tops
    items = []
    for action, title in tops:
        menu = _submenu_of(action)
        if menu is not None:
            items.extend(_command_segment(menu, title))
    if version == _versions['commands']:
        _command_caches[menubar_id] = (version, items)
    return items


def _make_menupaths_unique(items):
    # The core's row diff and the weights file are both keyed on menupath.
    # The items are cached between opens, so a renamed one is a copy.
    seen = {}
    unique = []
    for item in items:
        path = item['menupath']
        count = seen.get(path, 0) + 1
        seen[path] = count
        if count > 1:
            item = dict(item)
            item['menupath'] = "%s (%d)" % (path, count)
        unique.append(item)
    return unique


def _context_key(context):
    if not context:
        return None
    return tuple(sorted(context.items()))


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
        self._context = None
        self._chosen = False
        self._items_cache = None

    def set_app(self, app):
        self._app = app
        self._chosen = False

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
        context = self._context
        nodes, infos = _node_items()
        # A launch with an input kind creates a node and fills it in once
        # created, so a command has nothing to hand that node.
        if context and context.get('inputKind'):
            commands = _NO_ITEMS
        else:
            commands = _command_items(self.main_window())
        key = _context_key(context)
        cached = self._items_cache
        if (cached is not None and cached[0] is nodes and cached[1] is commands
                and cached[2] == key):
            return cached[3]
        if context:
            listed = [item for item, info in zip(nodes, infos) if _accepts_info(info, context)]
        else:
            listed = nodes
        items = _make_menupaths_unique(listed + commands)
        self._items_cache = (nodes, commands, key, items)
        return items

    def get_weights_file(self):
        if self._storage_dir is not None:
            return os.path.join(self._storage_dir, _WEIGHTS_FILENAME)
        return os.path.join(os.path.expanduser(_DEFAULT_STORAGE_DIR), _WEIGHTS_FILENAME)

    def invoke(self, thing):
        self._chosen = True
        _trigger_action(self._app, thing['menuobj'])

    def invoke_opens_window(self, thing):
        """False for a node the nodes menu creates without a dialog; menu
        commands and script actions may open anything."""
        plugin_id = _string_property(thing.get('menuobj'), _PLUGIN_ID_PROPERTY)
        return not plugin_id or plugin_id in _FILE_DIALOG_PLUGIN_IDS

    def closed(self):
        """The popup closed: a filtered launch closed with no choice tells
        the app, which would otherwise hold its context for a later choice."""
        if self._chosen or not self._context or self._app is None:
            return
        self._chosen = True
        try:
            self._app.triggerTabMenuAction(None)
        except RuntimeError:
            pass

    def get_color(self, menuobj):
        color = _string_property(menuobj, _COLOR_PROPERTY)
        if color:
            return (QtGui.QColor(color), QtGui.QColor(color))
        return (None, None)


_plugin = NatronPlugin()


def _live_popup():
    popup = tabtabtab_nuke_core._tabtabtab_instance
    return popup if _is_alive(popup) else None


def set_storage_dir(path):
    """Keep the weights file in `path` instead of ~/.Natron."""
    _plugin.set_storage_dir(path)
    popup = _live_popup()
    if popup is not None and hasattr(popup, "set_weights_file"):
        popup.set_weights_file(_plugin.get_weights_file())


def flush_weights():
    """Write the weights the popup has not saved yet."""
    popup = _live_popup()
    if popup is not None and hasattr(popup, "flush_weights"):
        popup.flush_weights()


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


_preferences_cache = None


def _preferences():
    global _preferences_cache
    if _preferences_cache is None:
        _preferences_cache = _read_preferences()
    order, scroll_enabled = _preferences_cache
    return list(order), scroll_enabled


def invalidate_preferences():
    """Read the Preferences again on the next open."""
    global _preferences_cache
    _preferences_cache = None


_window_cache = (None, None)


def _window_for(app):
    # A project's main window is the same on every open, and finding it walks
    # the widget tree.
    global _window_cache
    cached_app, cached_window = _window_cache
    if app is not None and app is cached_app and _is_alive(cached_window):
        return cached_window
    window = _find_natron_main_window()
    _window_cache = (app, window) if window is not None else (None, None)
    return window


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


_close_watcher = None
_watched_popup = None


def _watch_popup_close(popup):
    # The core hides its popup on every close path (a choice, Escape, a
    # click outside), and on a choice invokes it just before or just after.
    global _close_watcher, _watched_popup
    if not _is_alive(popup) or popup is _watched_popup:
        return
    if _close_watcher is None:
        class _CloseWatcher(QtCore.QObject):
            def eventFilter(self, watched, event):
                if event.type() == QtCore.QEvent.Hide:
                    # After a choice the core invokes once hidden, if any.
                    QtCore.QTimer.singleShot(0, _plugin.closed)
                return False

        _close_watcher = _CloseWatcher()
    popup.installEventFilter(_close_watcher)
    _watched_popup = popup


def launch(app, context=None):
    """Open the tab menu for `app`, the GuiApp whose node graph has focus.

    `context`, when given, lists only the node types that fit it, see
    _accepts(): a dict with any of 'inputKind' (a kind name such as
    "scene"), 'selectedKind' (the kind the selected nodes output) and
    'needsPrimPathKnob'. Menu commands, and actions scripts added to the
    nodes menu, stay listed unless 'inputKind' is set.
    """
    window = _window_for(app)
    _plugin.set_app(app)
    _plugin.set_context(dict(context) if context else None)
    _plugin.set_window(window)
    _attach_popup_to(window)
    space_mode_order, scroll_enabled = _preferences()
    tabtabtab_nuke_core.launch(
        _plugin,
        space_mode_order=space_mode_order,
        scroll_enabled=scroll_enabled,
    )
    _watch_popup_close(tabtabtab_nuke_core._tabtabtab_instance)


_WARM_UP_INTERVAL_MS = 500
_WARM_UP_ATTEMPTS = 240


def schedule_warm_up():
    """Build the popup and its items once the main window is up, so the
    first open costs what any other does. Weights not yet saved are written
    when the application quits."""
    qapp = QtWidgets.QApplication.instance()
    if qapp is not None:
        try:
            qapp.aboutToQuit.connect(flush_weights)
        except (AttributeError, RuntimeError):
            pass
    QtCore.QTimer.singleShot(_WARM_UP_INTERVAL_MS, lambda: _warm_up(_WARM_UP_ATTEMPTS))


def _warm_up(attempts):
    if _live_popup() is not None:
        return
    window = _find_natron_main_window()
    try:
        ready = window is not None and window.isVisible()
    except RuntimeError:
        ready = False
    if not ready:
        if attempts > 1:
            QtCore.QTimer.singleShot(_WARM_UP_INTERVAL_MS, lambda: _warm_up(attempts - 1))
        return
    _plugin.set_context(None)
    _plugin.set_window(window)
    space_mode_order, scroll_enabled = _preferences()
    tabtabtab_nuke_core.preload(
        _plugin,
        space_mode_order=space_mode_order,
        scroll_enabled=scroll_enabled,
    )
    popup = _live_popup()
    if popup is None:
        return
    _watch_popup_close(popup)
    try:
        popup.ensurePolished()
        layout = popup.layout()
        if layout is not None:
            layout.activate()
        # The native window and its backing store, which the first show
        # would otherwise create.
        popup.winId()
        # Painting the hidden popup into a pixmap runs the list's deferred
        # item layout, the delegate, the row colours and the glyph caches
        # once, off screen and without taking focus.
        popup.grab()
    except RuntimeError:
        pass
