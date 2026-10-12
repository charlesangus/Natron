"""Unit tests for the Natron tabtabtab adapter (Gui/Resources/TabTabTab/tabtabtab_natron.py).

PySide6, shiboken6 and NatronGui are stubbed in sys.modules, following the
stub pattern used by the vendored core's own tests
(Gui/Resources/TabTabTab/tests/test_main_window_discovery.py). The adapter
calls tabtabtab_nuke_core.launch(...) through the module object, so that
module is stubbed too and its launch attribute can be swapped in after the
adapter is loaded.
"""

import importlib.util
import os
import sys
import tempfile
import types
import unittest
from unittest.mock import MagicMock


_ADAPTER_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..",
    "Gui", "Resources", "TabTabTab", "tabtabtab_natron.py",
)

_DEFAULT_SPACE_MODE_ORDER = ["anchored_fuzzy", "non_anchored_fuzzy", "consecutive"]


class _FakeQIcon(object):
    def __init__(self, path):
        self.path = path

    def isNull(self):
        return False


class _FakeQColor(object):
    def __init__(self, value=None):
        self.value = value


class _FakeQKeySequence(object):
    NativeText = "NativeText"


class _FakeKeySequenceValue(object):
    def __init__(self, text):
        self._text = text

    def toString(self, fmt):
        return self._text


class _FakeAction(object):
    def __init__(self, shortcut_text="", icon_obj=None):
        self._shortcut_text = shortcut_text
        self._icon_obj = icon_obj
        self.trigger_calls = 0

    def shortcut(self):
        return _FakeKeySequenceValue(self._shortcut_text)

    def icon(self):
        return self._icon_obj

    def trigger(self):
        self.trigger_calls += 1


class _FakeWidget(object):
    def __init__(self, classname="", parent=None, menubar=None):
        self._classname = classname
        self._parent = parent
        self._menubar = menubar

    def metaObject(self):
        return types.SimpleNamespace(className=lambda: self._classname)

    def parentWidget(self):
        return self._parent

    def menuBar(self):
        return self._menubar


class _FakeApp(object):
    def __init__(self, node_items_json):
        self._node_items_json = node_items_json
        self.created = []

    def getTabMenuNodeItems(self):
        return self._node_items_json

    def createNodeFromTabMenu(self, plugin_id, major):
        self.created.append((plugin_id, major))


class _FakeChoiceParam(object):
    def __init__(self, option_id):
        self._option_id = option_id

    def getValue(self):
        return 0

    def getOption(self, value):
        return self._option_id


class _FakeBoolParam(object):
    def __init__(self, value):
        self._value = value

    def getValue(self):
        return self._value


class _FakeSettings(object):
    def __init__(self, params):
        self._params = params

    def getParam(self, name):
        return self._params.get(name)


def _make_main_window_instance(main_window_cls, classname="", parent=None, menubar=None):
    cls = type("FakeMainWindow", (main_window_cls,), {})
    instance = cls()
    instance._classname = classname
    instance._parent = parent
    instance._menubar = menubar
    instance.metaObject = lambda: types.SimpleNamespace(className=lambda: instance._classname)
    instance.parentWidget = lambda: instance._parent
    instance.menuBar = lambda: instance._menubar
    return instance


def _make_qapp(active=None, top_level=None):
    return types.SimpleNamespace(
        activeWindow=lambda: active,
        topLevelWidgets=lambda: list(top_level or []),
    )


class _AdapterHarness(object):
    def __init__(self, module, qtcore, qtgui, qtwidgets, shiboken6_module,
                 natron_gui_module, natron_ns, core, scheduled, app_holder):
        self.module = module
        self.qtcore = qtcore
        self.qtgui = qtgui
        self.qtwidgets = qtwidgets
        self.shiboken6 = shiboken6_module
        self.natron_gui = natron_gui_module
        self.natron_ns = natron_ns
        self.core = core
        self.scheduled = scheduled
        self.app_holder = app_holder


class NatronAdapterTests(unittest.TestCase):
    def _load_adapter(self):
        scheduled = []

        qtcore = types.ModuleType("PySide6.QtCore")

        class _StubQTimer(object):
            @staticmethod
            def singleShot(delay_ms, callback):
                scheduled.append((delay_ms, callback))

        qtcore.QTimer = _StubQTimer

        qtgui = types.ModuleType("PySide6.QtGui")
        qtgui.QIcon = _FakeQIcon
        qtgui.QColor = _FakeQColor
        qtgui.QKeySequence = _FakeQKeySequence

        app_holder = {"instance": _make_qapp()}

        qtwidgets = types.ModuleType("PySide6.QtWidgets")
        qtwidgets.QMainWindow = type("QMainWindow", (), {})

        class _StubQApplication(object):
            @staticmethod
            def instance():
                return app_holder["instance"]

        qtwidgets.QApplication = _StubQApplication

        pyside6 = types.ModuleType("PySide6")
        pyside6.QtCore = qtcore
        pyside6.QtGui = qtgui
        pyside6.QtWidgets = qtwidgets

        shiboken6_module = types.ModuleType("shiboken6")
        shiboken6_module.isValid = lambda obj: True

        natron_ns = types.SimpleNamespace(getSettings=lambda: _FakeSettings({}))
        natron_gui_module = types.ModuleType("NatronGui")
        natron_gui_module.natron = natron_ns

        # Mirrors TabTabTabPlugin's default get_icon/get_color (the real
        # contract the adapter is written against), so NatronPlugin's
        # super() calls behave like they would against the real core.
        class _FakeTabTabTabPlugin(object):
            def get_icon(self, menuobj):
                try:
                    icon = menuobj.icon()
                    if isinstance(icon, qtgui.QIcon) and not icon.isNull():
                        return icon
                except Exception:
                    pass
                return None

            def get_color(self, menuobj):
                return (None, None)

        core = types.ModuleType("tabtabtab_nuke_core")
        core.DEFAULT_SPACE_MODE_ORDER = list(_DEFAULT_SPACE_MODE_ORDER)
        core.TabTabTabPlugin = _FakeTabTabTabPlugin
        core.find_qt_menu_items = lambda menubar: []
        core.set_main_window_finder = MagicMock()
        core.launch = MagicMock()

        stub_modules = {
            "PySide6": pyside6,
            "PySide6.QtCore": qtcore,
            "PySide6.QtGui": qtgui,
            "PySide6.QtWidgets": qtwidgets,
            "shiboken6": shiboken6_module,
            "NatronGui": natron_gui_module,
            "tabtabtab_nuke_core": core,
        }
        previous = {name: sys.modules.get(name) for name in stub_modules}
        sys.modules.update(stub_modules)

        spec = importlib.util.spec_from_file_location(
            "tabtabtab_natron_under_test", _ADAPTER_PATH
        )
        module = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(module)
        finally:
            for name in stub_modules:
                if previous[name] is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = previous[name]

        return _AdapterHarness(
            module, qtcore, qtgui, qtwidgets, shiboken6_module,
            natron_gui_module, natron_ns, core, scheduled, app_holder,
        )

    # --- node and command items ------------------------------------------

    def test_node_items_build_menupath_from_grouping_and_label(self):
        h = self._load_adapter()
        app = _FakeApp(
            '[{"id": "BlurOFX", "major": 1, "label": "Blur", '
            '"grouping": ["Filter"], "icon": "", "color": "", "shortcut": ""}]'
        )
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)

        items = plugin.get_items()

        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["menupath"], "Filter/Blur")
        self.assertIsInstance(items[0]["menuobj"], h.module._NodeEntry)
        self.assertNotIn("shortcut", items[0])

    def test_slash_in_label_is_substituted(self):
        h = self._load_adapter()
        app = _FakeApp(
            '[{"id": "MergeOFX", "major": 2, "label": "A/B", '
            '"grouping": ["Merge"], "icon": "", "color": "", "shortcut": "M"}]'
        )
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)

        items = plugin.get_items()

        self.assertEqual(items[0]["menupath"], u"Merge/A∕B")
        self.assertEqual(items[0]["shortcut"], "M")

    def test_duplicate_menupath_gets_suffix(self):
        h = self._load_adapter()
        app = _FakeApp(
            '[{"id": "BlurOFX1", "major": 1, "label": "Blur", "grouping": ["Filter"]},'
            ' {"id": "BlurOFX2", "major": 2, "label": "Blur", "grouping": ["Filter"]}]'
        )
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)

        items = plugin.get_items()

        self.assertEqual([i["menupath"] for i in items], ["Filter/Blur", "Filter/Blur (2)"])

    def test_command_items_appended_with_shortcut(self):
        h = self._load_adapter()
        app = _FakeApp('[]')
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)

        action = _FakeAction(shortcut_text="Ctrl+P")
        h.module.find_qt_menu_items = lambda menubar: [
            {"menuobj": action, "menupath": "Edit/Preferences"}
        ]
        window = _FakeWidget(classname="Natron::Gui", menubar=object())
        h.app_holder["instance"] = _make_qapp(active=window)

        items = plugin.get_items()

        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["menupath"], "Edit/Preferences")
        self.assertIs(items[0]["menuobj"], action)
        self.assertEqual(items[0]["shortcut"], "Ctrl+P")

    def test_command_items_without_shortcut_text_omit_key(self):
        h = self._load_adapter()
        app = _FakeApp('[]')
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)

        action = _FakeAction(shortcut_text="")
        h.module.find_qt_menu_items = lambda menubar: [
            {"menuobj": action, "menupath": "File/Open"}
        ]
        window = _FakeWidget(classname="Natron::Gui", menubar=object())
        h.app_holder["instance"] = _make_qapp(active=window)

        items = plugin.get_items()

        self.assertNotIn("shortcut", items[0])

    # --- colour hook -------------------------------------------------------

    def test_get_color_parses_hex_for_node_entries(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        entry = h.module._NodeEntry("id", 1, "", "#112233")

        left, text = plugin.get_color(entry)

        self.assertIsInstance(left, h.qtgui.QColor)
        self.assertEqual(left.value, "#112233")
        self.assertIsInstance(text, h.qtgui.QColor)
        self.assertEqual(text.value, "#112233")

    def test_get_color_returns_none_for_empty_color(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        entry = h.module._NodeEntry("id", 1, "", "")

        self.assertEqual(plugin.get_color(entry), (None, None))

    def test_get_color_returns_none_for_command_items(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        action = _FakeAction()

        self.assertEqual(plugin.get_color(action), (None, None))

    # --- deferred invoke ----------------------------------------------------

    def test_invoke_node_defers_create_node_one_tick(self):
        h = self._load_adapter()
        app = _FakeApp('[]')
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)
        entry = h.module._NodeEntry("BlurOFX", 1, "", "")

        plugin.invoke({"menuobj": entry})

        self.assertEqual(len(h.scheduled), 1)
        self.assertEqual(app.created, [])
        delay, callback = h.scheduled[0]
        self.assertEqual(delay, 0)

        callback()

        self.assertEqual(app.created, [("BlurOFX", 1)])

    def test_invoke_action_defers_trigger_one_tick(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        action = _FakeAction()

        plugin.invoke({"menuobj": action})

        self.assertEqual(len(h.scheduled), 1)
        self.assertEqual(action.trigger_calls, 0)

        h.scheduled[0][1]()

        self.assertEqual(action.trigger_calls, 1)

    def test_invoke_action_skipped_when_shiboken_invalid(self):
        h = self._load_adapter()
        h.shiboken6.isValid = lambda obj: False
        plugin = h.module.NatronPlugin()
        action = _FakeAction()

        plugin.invoke({"menuobj": action})
        h.scheduled[0][1]()

        self.assertEqual(action.trigger_calls, 0)

    def test_invoke_action_swallows_runtime_error_from_shiboken(self):
        h = self._load_adapter()

        def _raise(obj):
            raise RuntimeError("wrapped C++ object destroyed")

        h.shiboken6.isValid = _raise
        plugin = h.module.NatronPlugin()
        action = _FakeAction()

        plugin.invoke({"menuobj": action})
        h.scheduled[0][1]()

        self.assertEqual(action.trigger_calls, 0)

    # --- icon hook -----------------------------------------------------------

    def test_get_icon_caches_per_path(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        tmp = tempfile.NamedTemporaryFile(delete=False)
        tmp.close()
        try:
            entry = h.module._NodeEntry("id", 1, tmp.name, "")

            icon_first = plugin.get_icon(entry)
            icon_second = plugin.get_icon(entry)

            self.assertIsInstance(icon_first, h.qtgui.QIcon)
            self.assertEqual(icon_first.path, tmp.name)
            self.assertIs(icon_first, icon_second)
        finally:
            os.unlink(tmp.name)

    def test_get_icon_returns_none_for_empty_or_missing_path(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()

        self.assertIsNone(plugin.get_icon(h.module._NodeEntry("id", 1, "", "")))
        self.assertIsNone(
            plugin.get_icon(h.module._NodeEntry("id", 1, "/no/such/file.png", ""))
        )

    def test_get_icon_falls_back_to_base_for_commands(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        icon_obj = h.qtgui.QIcon("/some/path")

        action_with_icon = _FakeAction(icon_obj=icon_obj)
        action_without_icon = _FakeAction(icon_obj=None)

        self.assertIs(plugin.get_icon(action_with_icon), icon_obj)
        self.assertIsNone(plugin.get_icon(action_without_icon))

    # --- weights path --------------------------------------------------------

    def test_get_weights_file_default_path(self):
        h = self._load_adapter()

        expected = os.path.join(os.path.expanduser("~/.Natron"), "tabtabtab_weights.json")
        self.assertEqual(h.module._plugin.get_weights_file(), expected)

    def test_set_storage_dir_repoints_weights_file_and_creates_nothing(self):
        h = self._load_adapter()
        tmp_dir = tempfile.mkdtemp()
        try:
            h.module.set_storage_dir(tmp_dir)

            path = h.module._plugin.get_weights_file()

            self.assertEqual(path, os.path.join(tmp_dir, "tabtabtab_weights.json"))
            self.assertFalse(os.path.exists(path))
        finally:
            os.rmdir(tmp_dir)

    # --- preferences -----------------------------------------------------------

    def test_read_preferences_valid(self):
        h = self._load_adapter()
        h.natron_ns.getSettings = lambda: _FakeSettings({
            "tabMenuSearchModes": _FakeChoiceParam("consecutive,anchored_fuzzy,non_anchored_fuzzy"),
            "tabMenuScroll": _FakeBoolParam(False),
        })

        order, scroll = h.module._read_preferences()

        self.assertEqual(order, ["consecutive", "anchored_fuzzy", "non_anchored_fuzzy"])
        self.assertFalse(scroll)

    def test_read_preferences_missing_param_falls_back(self):
        h = self._load_adapter()
        h.natron_ns.getSettings = lambda: _FakeSettings({})

        order, scroll = h.module._read_preferences()

        self.assertEqual(order, _DEFAULT_SPACE_MODE_ORDER)
        self.assertTrue(scroll)

    def test_read_preferences_non_permutation_falls_back(self):
        h = self._load_adapter()
        h.natron_ns.getSettings = lambda: _FakeSettings({
            "tabMenuSearchModes": _FakeChoiceParam("consecutive,consecutive,anchored_fuzzy"),
            "tabMenuScroll": _FakeBoolParam(True),
        })

        order, scroll = h.module._read_preferences()

        self.assertEqual(order, _DEFAULT_SPACE_MODE_ORDER)
        self.assertTrue(scroll)

    def test_read_preferences_exception_falls_back(self):
        h = self._load_adapter()

        def _raise():
            raise RuntimeError("settings unavailable")

        h.natron_ns.getSettings = _raise

        order, scroll = h.module._read_preferences()

        self.assertEqual(order, _DEFAULT_SPACE_MODE_ORDER)
        self.assertTrue(scroll)

    # --- launch ---------------------------------------------------------------

    def test_launch_passes_preferences_to_core_launch(self):
        h = self._load_adapter()
        h.natron_ns.getSettings = lambda: _FakeSettings({
            "tabMenuSearchModes": _FakeChoiceParam("consecutive,anchored_fuzzy,non_anchored_fuzzy"),
            "tabMenuScroll": _FakeBoolParam(False),
        })
        app = _FakeApp("[]")

        h.module.launch(app)

        h.core.launch.assert_called_once()
        args, kwargs = h.core.launch.call_args
        self.assertIs(args[0], h.module._plugin)
        self.assertEqual(kwargs["space_mode_order"],
                          ["consecutive", "anchored_fuzzy", "non_anchored_fuzzy"])
        self.assertFalse(kwargs["scroll_enabled"])
        self.assertIs(h.module._plugin._app, app)

    def test_launch_falls_back_to_defaults_when_params_missing(self):
        h = self._load_adapter()
        h.natron_ns.getSettings = lambda: _FakeSettings({})
        app = _FakeApp("[]")

        h.module.launch(app)

        _args, kwargs = h.core.launch.call_args
        self.assertEqual(kwargs["space_mode_order"], _DEFAULT_SPACE_MODE_ORDER)
        self.assertTrue(kwargs["scroll_enabled"])

    # --- main window finder -----------------------------------------------------

    def test_find_natron_main_window_prefers_ancestor_of_active_window(self):
        h = self._load_adapter()
        main_gui = _FakeWidget(classname="Natron::Gui")
        mid = _FakeWidget(classname="", parent=main_gui)
        leaf = _FakeWidget(classname="SomeDialog", parent=mid)
        h.app_holder["instance"] = _make_qapp(active=leaf, top_level=[])

        self.assertIs(h.module._find_natron_main_window(), main_gui)

    def test_find_natron_main_window_falls_back_to_top_level_scan(self):
        h = self._load_adapter()
        other_panel = _make_main_window_instance(h.qtwidgets.QMainWindow, classname="SomePanel")
        not_a_main_window = _FakeWidget(classname="Natron::Gui")
        target = _make_main_window_instance(h.qtwidgets.QMainWindow, classname="Natron::Gui")
        h.app_holder["instance"] = _make_qapp(
            active=None, top_level=[other_panel, not_a_main_window, target]
        )

        self.assertIs(h.module._find_natron_main_window(), target)

    def test_find_natron_main_window_returns_none_when_nothing_matches(self):
        h = self._load_adapter()
        leaf = _FakeWidget(classname="SomeDialog")
        other_panel = _make_main_window_instance(h.qtwidgets.QMainWindow, classname="SomePanel")
        h.app_holder["instance"] = _make_qapp(active=leaf, top_level=[other_panel])

        self.assertIsNone(h.module._find_natron_main_window())


if __name__ == "__main__":
    unittest.main()
