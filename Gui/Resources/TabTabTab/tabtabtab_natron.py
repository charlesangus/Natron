"""tabtabtab adapter for Natron.

Node items come from the process-wide nodes menu (natron.nodesMenu()), or
inside a network builder's graph from the network nodes menu
(natron.networkNodesMenu()), command items from the main window's menu bar,
and the preferences from Natron's Settings. Every item is a QAction; GuiApp triggers it so that the
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
_NETWORK_TARGETS_PROPERTY = "natronNetworkTargets"

# PLUGINID_NATRON_BACKDROP, listed in a network builder's graph as well.
_BACKDROP_PLUGIN_ID = "Backdrop"

# PLUGINID_NATRON_READ and PLUGINID_NATRON_WRITE, whose interactive creation
# asks for a file first.
_FILE_DIALOG_PLUGIN_IDS = frozenset(("Read", "Write"))

# An input of this kind takes any kind.
_ANY_KIND = "polymorphic"

# Network nodes (shaders, kernel and channel nodes) join only other network
# nodes, so they are listed only where the menu is filtered for that kind.
_NETWORK_KIND = "network"

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
    """What _accepts() and _accepts_network() read from a node action, read
    once per walk."""
    kinds = tuple(k for k in _string_property(action, _INPUT_KINDS_PROPERTY).split(",") if k)
    plugin_id = _string_property(action, _PLUGIN_ID_PROPERTY)
    targets = tuple(t for t in _string_property(action, _NETWORK_TARGETS_PROPERTY).split(",") if t)
    return (
        _string_property(action, _INPUT_KIND_PROPERTY),
        bool(plugin_id),
        kinds,
        bool(_string_property(action, _PRIM_PATH_KNOB_PROPERTY)),
        plugin_id,
        targets,
    )


def _accepts_info(info, context):
    input_kind, has_plugin, input_kinds, has_prim_knob = info[:4]
    if input_kind == _NETWORK_KIND or _NETWORK_KIND in input_kinds:
        if _NETWORK_KIND not in (context.get('inputKind'), context.get('selectedKind')):
            return False
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


# (version, menu id, items, infos) per natron menu getter.
_menu_item_caches = {}

# Returned for "no items", never modified: get_items() tells a list unchanged
# by its identity.
_NO_ITEMS = []


def _menu_items(getter):
    """(items, filter infos) of the menu natron.<getter>() returns; the same
    lists until a watched menu changes."""
    try:
        menu = getattr(NatronGui.natron, getter)()
    except Exception:
        return _NO_ITEMS, _NO_ITEMS
    if not _is_alive(menu):
        return _NO_ITEMS, _NO_ITEMS
    version = _versions['nodes']
    menu_id = _menu_id(menu)
    cached = _menu_item_caches.get(getter)
    if (cached is not None and cached[0] == version and menu_id is not None
            and cached[1] == menu_id):
        return cached[2], cached[3]
    items = []
    infos = []
    if _collect_nodes(menu, (), items, infos) and version == _versions['nodes']:
        _menu_item_caches[getter] = (version, menu_id, items, infos)
    return items, infos


def _node_items():
    """(items, filter infos) of the nodes menu."""
    return _menu_items('nodesMenu')


def _network_node_items():
    """(items, filter infos) of the network nodes menu."""
    return _menu_items('networkNodesMenu')


# The target last made current in the menu, per network context's
# 'networkKey'.
_network_current_targets = {}


def _is_network_context(context):
    return bool(context) and 'networkTargets' in context


def _network_target(context):
    """The current target of a network context, None when it has none."""
    targets = tuple(context.get('networkTargets') or ())
    if not targets:
        return None
    current = _network_current_targets.get(context.get('networkKey'))
    return current if current in targets else targets[0]


def _cycle_network_target(context, step=1):
    """Make the next of the context's targets current; False when there is
    no other."""
    targets = tuple(context.get('networkTargets') or ())
    if len(targets) < 2:
        return False
    current = _network_target(context)
    index = targets.index(current) if current in targets else 0
    _network_current_targets[context.get('networkKey')] = targets[(index + step) % len(targets)]
    return True


def _network_target_label(context, target):
    targets = tuple(context.get('networkTargets') or ())
    labels = tuple(context.get('networkTargetLabels') or ())
    if target in targets and targets.index(target) < len(labels):
        return labels[targets.index(target)]
    return target


def _accepts_network(info, context, target):
    """Whether the network node type of `info` is listed for a network
    `context` whose current target is `target`."""
    plugin_id, targets = info[4], info[5]
    plugin_ids = context.get('networkPluginIds')
    if plugin_ids is not None and plugin_id not in plugin_ids:
        return False
    return target is None or target in targets


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
        if _is_network_context(context):
            return self._network_items(context)
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
        listed = [item for item, info in zip(nodes, infos) if _accepts_info(info, context or {})]
        items = _make_menupaths_unique(listed + commands)
        self._items_cache = (nodes, commands, key, items)
        return items

    def _network_items(self, context):
        """The network node types supporting the current target, and
        Backdrop when the context says so; no commands."""
        target = _network_target(context)
        nodes, infos = _network_node_items()
        others, other_infos = _node_items() if context.get('networkBackdrop') else (_NO_ITEMS, _NO_ITEMS)
        key = (_context_key(context), target)
        cached = self._items_cache
        if (cached is not None and cached[0] is nodes and cached[1] is others
                and cached[2] == key):
            return cached[3]
        listed = [item for item, info in zip(nodes, infos) if _accepts_network(info, context, target)]
        listed += [item for item, info in zip(others, other_infos) if info[4] == _BACKDROP_PLUGIN_ID]
        items = _make_menupaths_unique(listed)
        self._items_cache = (nodes, others, key, items)
        return items

    def context(self):
        return self._context

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


_target_label = None
_target_key_watcher = None
_target_watched_input = None


def _refresh_target_label(popup):
    """Show the network context's current target above the input, or hide
    the label outside a network context."""
    global _target_label
    context = _plugin.context()
    target = _network_target(context) if _is_network_context(context) else None
    if target is None:
        if _is_alive(_target_label) and _target_label.isVisible():
            _target_label.hide()
            popup.adjustSize()
        return
    if not _is_alive(_target_label):
        _target_label = QtWidgets.QLabel(popup)
        _target_label.setObjectName("tabMenuNetworkTarget")
        popup.layout().insertWidget(0, _target_label)
    text = "Target: %s" % _network_target_label(context, target)
    if len(tuple(context.get('networkTargets') or ())) > 1:
        text += "    (Shift+Tab: next target)"
    _target_label.setText(text)
    if not _target_label.isVisible():
        _target_label.show()
        popup.adjustSize()


def _switch_network_target(popup):
    context = _plugin.context()
    if not _is_network_context(context) or not _cycle_network_target(context):
        return
    items = _plugin.get_items()
    popup._shown_items = items
    _plugin.invalidate_color_cache()
    popup.things_model.refresh_items(items)
    popup.things_model.invalidate_color_cache()
    popup.move_selection(where="first")
    _refresh_target_label(popup)


def _watch_target_key(popup):
    # The core's line edit leaves Shift+Tab to Qt's focus chain, which would
    # move the focus out of the input.
    global _target_key_watcher, _target_watched_input
    line_edit = getattr(popup, 'input', None)
    if not _is_alive(line_edit) or line_edit is _target_watched_input:
        return
    if _target_key_watcher is None:
        class _TargetKeyWatcher(QtCore.QObject):
            def eventFilter(self, watched, event):
                if event.type() not in (QtCore.QEvent.KeyPress, QtCore.QEvent.ShortcutOverride):
                    return False
                key = event.key()
                backtab = key == QtCore.Qt.Key_Backtab or (
                    key == QtCore.Qt.Key_Tab and event.modifiers() & QtCore.Qt.ShiftModifier)
                if not backtab or not _is_network_context(_plugin.context()):
                    return False
                if event.type() == QtCore.QEvent.ShortcutOverride:
                    event.accept()
                    return True
                popup = _live_popup()
                if popup is not None:
                    _switch_network_target(popup)
                return True

        _target_key_watcher = _TargetKeyWatcher()
    line_edit.installEventFilter(_target_key_watcher)
    _target_watched_input = line_edit


def launch(app, context=None):
    """Open the tab menu for `app`, the GuiApp whose node graph has focus.

    `context`, when given, lists only the node types that fit it, see
    _accepts(): a dict with any of 'inputKind' (a kind name such as
    "scene"), 'selectedKind' (the kind the selected nodes output) and
    'needsPrimPathKnob'. Menu commands, and actions scripts added to the
    nodes menu, stay listed unless 'inputKind' is set.

    A network context lists instead the network nodes menu's node types
    supporting its current target, see _accepts_network(), and no command:
    'networkTargets' (the targets Shift+Tab switches among, the first
    current until another is picked), 'networkTargetLabels' (their names),
    'networkKey' (what the current target is remembered for), and
    optionally 'networkPluginIds' (the only node types to list) and
    'networkBackdrop' (whether Backdrop is listed too).
    """
    window = _window_for(app)
    _plugin.set_app(app)
    _plugin.set_context(dict(context) if context else None)
    _plugin.set_window(window)
    _attach_popup_to(window)
    space_mode_order, scroll_enabled = _preferences()
    popup = _live_popup()
    if popup is not None:
        _refresh_target_label(popup)
    tabtabtab_nuke_core.launch(
        _plugin,
        space_mode_order=space_mode_order,
        scroll_enabled=scroll_enabled,
    )
    # A popup the core has just made shows no label yet.
    popup = _live_popup()
    if popup is not None:
        _refresh_target_label(popup)
    _watch_popup_close(popup)
    _watch_target_key(popup)


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
        # would otherwise create. Not winId(): it sets WA_NativeWindow,
        # which makes Qt turn every child of the main window native, and
        # then each repaint() in the main window paints and composites at
        # once instead of being coalesced into the next frame. The
        # unbound call skips the popup's own create(), which picks an item.
        QtWidgets.QWidget.create(popup)
        # Painting the hidden popup into a pixmap runs the list's deferred
        # item layout, the delegate, the row colours and the glyph caches
        # once, off screen and without taking focus.
        popup.grab()
    except RuntimeError:
        pass
