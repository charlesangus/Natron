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


_GRADE_RESOURCE_ICON = ":/Resources/Images/NativeNodes/net.sf.openfx.GradePlugin.png"
_FAKE_QT_RESOURCES = {_GRADE_RESOURCE_ICON}


_FAKE_APP_FONT = object()
_FAKE_FONT_HEIGHT = 11


class _FakeFontMetrics(object):
    def __init__(self, font):
        self.font = font

    def height(self):
        return _FAKE_FONT_HEIGHT


class _FakeKeySequenceValue(object):
    def __init__(self, text):
        self._text = text

    def toString(self, fmt):
        return self._text


class _FakeAction(object):
    def __init__(self, shortcut_text="", icon_obj=None, text="", properties=None,
                 menu=None, visible=True, enabled=True):
        self._shortcut_text = shortcut_text
        self._icon_obj = icon_obj
        self._text = text
        self._properties = dict(properties or {})
        self._menu = menu
        self._visible = visible
        self._enabled = enabled
        self.trigger_calls = 0

    def shortcut(self):
        return _FakeKeySequenceValue(self._shortcut_text)

    def icon(self):
        return self._icon_obj

    def text(self):
        return self._text

    def property(self, name):
        return self._properties.get(name)

    def setProperty(self, name, value):
        self._properties[name] = value

    def menu(self):
        return self._menu

    def isVisible(self):
        return self._visible

    def isEnabled(self):
        return self._enabled

    def trigger(self):
        self.trigger_calls += 1


class _FakeMenu(object):
    def __init__(self, actions=None, enabled=True):
        self._actions = list(actions or [])
        self._enabled = enabled

    def actions(self):
        return list(self._actions)

    def isEnabled(self):
        return self._enabled


def _submenu(title, actions, owned=True):
    properties = {"natronPluginOwned": True, "natronGrouping": title} if owned else {}
    return _FakeAction(text=title, menu=_FakeMenu(actions), properties=properties)


def _plugin_action(label, plugin_id="", major=1, color="", shortcut=""):
    return _FakeAction(text=label, properties={
        "natronPluginOwned": True,
        "natronPluginID": plugin_id,
        "natronPluginMajor": major,
        "natronCategoryColor": color,
        "natronShortcut": shortcut,
    })


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
    def __init__(self):
        self.triggered = []

    def triggerTabMenuAction(self, action):
        self.triggered.append(action)
        action.trigger()


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


class _FakePopup(object):
    def __init__(self, parent=None, flags="DialogFlags"):
        self._parent = parent
        self._flags = flags
        self.set_parent_calls = []

    def parent(self):
        return self._parent

    def windowFlags(self):
        return self._flags

    def setParent(self, parent, flags):
        self.set_parent_calls.append((parent, flags))
        self._parent = parent


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

        class _StubQFile(object):
            @staticmethod
            def exists(path):
                return path in _FAKE_QT_RESOURCES or os.path.isfile(path)

        qtcore.QFile = _StubQFile

        qtgui = types.ModuleType("PySide6.QtGui")
        qtgui.QIcon = _FakeQIcon
        qtgui.QColor = _FakeQColor
        qtgui.QKeySequence = _FakeQKeySequence
        qtgui.QFontMetrics = _FakeFontMetrics

        app_holder = {"instance": _make_qapp()}

        qtwidgets = types.ModuleType("PySide6.QtWidgets")
        qtwidgets.QMainWindow = type("QMainWindow", (), {})

        class _StubQApplication(object):
            @staticmethod
            def instance():
                return app_holder["instance"]

            @staticmethod
            def font():
                return _FAKE_APP_FONT

        qtwidgets.QApplication = _StubQApplication

        pyside6 = types.ModuleType("PySide6")
        pyside6.QtCore = qtcore
        pyside6.QtGui = qtgui
        pyside6.QtWidgets = qtwidgets

        shiboken6_module = types.ModuleType("shiboken6")
        shiboken6_module.isValid = lambda obj: True

        natron_ns = types.SimpleNamespace(
            getSettings=lambda: _FakeSettings({}),
            nodesMenu=lambda: None,
        )
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
        core.set_row_metrics = MagicMock()
        core.launch = MagicMock()
        core._tabtabtab_instance = None

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

    def _set_nodes_menu(self, h, actions):
        menu = _FakeMenu(actions)
        h.natron_ns.nodesMenu = lambda: menu
        return menu

    def test_node_items_build_menupath_below_the_nodes_menu(self):
        h = self._load_adapter()
        blur = _plugin_action("Blur", "BlurOFX")
        self._set_nodes_menu(h, [_submenu("Filter", [blur])])
        plugin = h.module.NatronPlugin()

        items = plugin.get_items()

        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["menupath"], "Filter/Blur")
        self.assertIs(items[0]["menuobj"], blur)
        self.assertNotIn("shortcut", items[0])

    def test_nested_groupings_and_top_level_leaves(self):
        h = self._load_adapter()
        self._set_nodes_menu(h, [
            _submenu("Extra", [_submenu("Filter", [_plugin_action("Bloom")])]),
            _FakeAction(text="Loose"),
        ])
        plugin = h.module.NatronPlugin()

        paths = [i["menupath"] for i in plugin.get_items()]

        self.assertEqual(paths, ["Extra/Filter/Bloom", "Loose"])

    def test_slash_in_label_is_substituted_and_shortcut_property_used(self):
        h = self._load_adapter()
        self._set_nodes_menu(h, [_submenu("Merge", [_plugin_action("A/B", shortcut="M")])])
        plugin = h.module.NatronPlugin()

        items = plugin.get_items()

        self.assertEqual(items[0]["menupath"], u"Merge/A\u2215B")
        self.assertEqual(items[0]["shortcut"], "M")

    def test_menu_text_ampersands_are_unescaped(self):
        h = self._load_adapter()
        self._set_nodes_menu(h, [_submenu("R&&D", [_plugin_action("Shuffle && Copy"),
                                                   _FakeAction(text="&Mine")])])
        plugin = h.module.NatronPlugin()

        paths = [i["menupath"] for i in plugin.get_items()]

        self.assertEqual(paths, ["R&D/Shuffle & Copy", "R&D/Mine"])

    def test_older_majors_keep_their_version_encoded_labels(self):
        h = self._load_adapter()
        self._set_nodes_menu(h, [_submenu("Filter", [
            _plugin_action("Blurry", "test.Blurry", 2),
            _plugin_action("Blurry 1", "test.Blurry", 1),
        ])])
        plugin = h.module.NatronPlugin()

        paths = [i["menupath"] for i in plugin.get_items()]

        self.assertEqual(paths, ["Filter/Blurry", "Filter/Blurry 1"])

    def test_python_action_with_a_plugin_path_is_kept_and_numbered(self):
        h = self._load_adapter()
        plus = _plugin_action("Plus", "PlusOFX")
        python_plus = _FakeAction(text="Plus", shortcut_text="Alt+P")
        self._set_nodes_menu(h, [_submenu("Merge", [plus, python_plus])])
        plugin = h.module.NatronPlugin()

        items = plugin.get_items()

        self.assertEqual([i["menupath"] for i in items], ["Merge/Plus", "Merge/Plus (2)"])
        self.assertIs(items[1]["menuobj"], python_plus)
        self.assertEqual(items[1]["shortcut"], "Alt+P")

    def test_python_action_under_merge_is_listed_and_triggers(self):
        h = self._load_adapter()
        mine = _FakeAction(text="My Merge Tool")
        self._set_nodes_menu(h, [_submenu("Merge", [_plugin_action("Plus"), mine])])
        app = _FakeApp()
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)

        items = [i for i in plugin.get_items() if i["menupath"] == "Merge/My Merge Tool"]
        self.assertEqual(len(items), 1)
        plugin.invoke(items[0])
        h.scheduled[0][1]()

        self.assertEqual(app.triggered, [mine])
        self.assertEqual(mine.trigger_calls, 1)

    def test_hidden_disabled_and_separator_actions_are_skipped(self):
        h = self._load_adapter()
        self._set_nodes_menu(h, [
            _submenu("Filter", [
                _plugin_action("Blur"),
                _FakeAction(text="Hidden", visible=False),
                _FakeAction(text="Disabled", enabled=False),
                _FakeAction(text=""),
            ]),
            _FakeAction(text="Off", menu=_FakeMenu([_FakeAction(text="X")], enabled=False)),
        ])
        plugin = h.module.NatronPlugin()

        paths = [i["menupath"] for i in plugin.get_items()]

        self.assertEqual(paths, ["Filter/Blur"])

    def test_no_nodes_menu_lists_no_node_items(self):
        h = self._load_adapter()
        h.natron_ns.nodesMenu = lambda: None
        plugin = h.module.NatronPlugin()

        self.assertEqual(plugin.get_items(), [])

    def test_duplicate_menupath_gets_suffix(self):
        h = self._load_adapter()
        self._set_nodes_menu(h, [_submenu("Filter", [_plugin_action("Blur", "BlurOFX1"),
                                                     _plugin_action("Blur", "BlurOFX2")])])
        plugin = h.module.NatronPlugin()

        items = plugin.get_items()

        self.assertEqual([i["menupath"] for i in items], ["Filter/Blur", "Filter/Blur (2)"])

    def test_command_items_appended_with_shortcut(self):
        h = self._load_adapter()
        app = _FakeApp()
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
        app = _FakeApp()
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

    def test_get_color_reads_the_category_colour_property(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        action = _plugin_action("Blur", color="#112233")

        left, text = plugin.get_color(action)

        self.assertIsInstance(left, h.qtgui.QColor)
        self.assertEqual(left.value, "#112233")
        self.assertIsInstance(text, h.qtgui.QColor)
        self.assertEqual(text.value, "#112233")

    def test_get_color_returns_none_for_empty_color(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()

        self.assertEqual(plugin.get_color(_plugin_action("Blur", color="")), (None, None))

    def test_get_color_returns_none_for_command_items(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        action = _FakeAction()

        self.assertEqual(plugin.get_color(action), (None, None))

    # --- deferred invoke ----------------------------------------------------

    def test_invoke_node_defers_the_trigger_through_the_app_one_tick(self):
        h = self._load_adapter()
        app = _FakeApp()
        plugin = h.module.NatronPlugin()
        plugin.set_app(app)
        blur = _plugin_action("Blur", "BlurOFX")

        plugin.invoke({"menuobj": blur})

        self.assertEqual(len(h.scheduled), 1)
        self.assertEqual(app.triggered, [])
        delay, callback = h.scheduled[0]
        self.assertEqual(delay, 0)

        callback()

        self.assertEqual(app.triggered, [blur])
        self.assertEqual(blur.trigger_calls, 1)

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

    def test_get_icon_comes_from_the_node_action(self):
        h = self._load_adapter()
        plugin = h.module.NatronPlugin()
        icon_obj = h.qtgui.QIcon(_GRADE_RESOURCE_ICON)
        grade = _FakeAction(text="Grade", icon_obj=icon_obj)

        self.assertIs(plugin.get_icon(grade), icon_obj)

    def test_get_icon_uses_the_base_for_commands(self):
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

    # --- filtered launch --------------------------------------------------------

    def _kind_menu(self, h):
        def node(label, kind, prim_knob=""):
            action = _plugin_action(label, label)
            action._properties["natronInputKind"] = kind
            action._properties["natronPrimPathKnob"] = prim_knob
            return action

        self._set_nodes_menu(h, [
            _submenu("3D", [
                _submenu("USD", [
                    node("USDEditTransform", "scene", "primPattern"),
                    node("USDPrune", "scene", "primPattern"),
                    node("USDMerge", "scene"),
                    node("USDMaterial", "", "primPath"),
                    node("USDXformPrim", "scene", "primPath"),
                ]),
            ]),
            _submenu("Filter", [node("Blur", "image")]),
            _submenu("Color", [node("Grade", "image")]),
            _submenu("Other", [node("Dot", "polymorphic")]),
        ])
        h.module.find_qt_menu_items = lambda menubar: [
            {"menuobj": _FakeAction(), "menupath": "Edit/Preferences"}
        ]
        window = _FakeWidget(classname="Natron::Gui", menubar=object())
        h.app_holder["instance"] = _make_qapp(active=window)

    def _labels(self, items):
        return sorted(i["menupath"].split("/")[-1] for i in items)

    def test_scene_context_lists_scene_nodes_with_a_prim_knob_only(self):
        h = self._load_adapter()
        self._kind_menu(h)

        h.module.launch(_FakeApp(), {"inputKind": "scene", "needsPrimPathKnob": True})
        items = h.module._plugin.get_items()

        # USDMaterial declares a prim path knob but takes no input, so it is not listed.
        self.assertEqual(self._labels(items), ["USDEditTransform", "USDPrune", "USDXformPrim"])

    def test_kind_context_without_knob_requirement_keeps_polymorphic_inputs(self):
        h = self._load_adapter()
        self._kind_menu(h)

        h.module.launch(_FakeApp(), {"inputKind": "image", "needsPrimPathKnob": False})
        items = h.module._plugin.get_items()

        self.assertEqual(self._labels(items), ["Blur", "Dot", "Grade"])

    def test_plain_launch_after_a_filtered_one_lists_everything(self):
        h = self._load_adapter()
        self._kind_menu(h)

        h.module.launch(_FakeApp(), {"inputKind": "scene", "needsPrimPathKnob": True})
        h.module.launch(_FakeApp())
        items = h.module._plugin.get_items()

        labels = self._labels(items)
        self.assertIn("Blur", labels)
        self.assertIn("USDMaterial", labels)
        self.assertIn("Preferences", labels)

    def test_filtered_choice_triggers_through_the_app(self):
        h = self._load_adapter()
        self._kind_menu(h)
        app = _FakeApp()

        h.module.launch(app, {"inputKind": "scene", "needsPrimPathKnob": True})
        items = h.module._plugin.get_items()
        prune = [i for i in items if i["menupath"].endswith("USDPrune")][0]
        h.module._plugin.invoke(prune)
        for _delay, callback in list(h.scheduled):
            callback()

        self.assertEqual(app.triggered, [prune["menuobj"]])

    # --- launch ---------------------------------------------------------------

    def test_launch_passes_preferences_to_core_launch(self):
        h = self._load_adapter()
        h.natron_ns.getSettings = lambda: _FakeSettings({
            "tabMenuSearchModes": _FakeChoiceParam("consecutive,anchored_fuzzy,non_anchored_fuzzy"),
            "tabMenuScroll": _FakeBoolParam(False),
        })
        app = _FakeApp()

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
        app = _FakeApp()

        h.module.launch(app)

        _args, kwargs = h.core.launch.call_args
        self.assertEqual(kwargs["space_mode_order"], _DEFAULT_SPACE_MODE_ORDER)
        self.assertTrue(kwargs["scroll_enabled"])

    def test_launch_reparents_cached_popup_to_launching_window(self):
        h = self._load_adapter()
        first_window = _FakeWidget(classname="Natron::Gui", menubar=object())
        second_window = _FakeWidget(classname="Natron::Gui", menubar=object())
        popup = _FakePopup(parent=first_window)
        h.core._tabtabtab_instance = popup
        h.app_holder["instance"] = _make_qapp(active=second_window)

        h.module.launch(_FakeApp())

        self.assertEqual(popup.set_parent_calls, [(second_window, "DialogFlags")])
        self.assertIs(popup.parent(), second_window)

    def test_launch_keeps_popup_parent_when_window_unchanged(self):
        h = self._load_adapter()
        window = _FakeWidget(classname="Natron::Gui", menubar=object())
        popup = _FakePopup(parent=window)
        h.core._tabtabtab_instance = popup
        h.app_holder["instance"] = _make_qapp(active=window)

        h.module.launch(_FakeApp())

        self.assertEqual(popup.set_parent_calls, [])

    def test_command_items_come_from_launching_window_after_focus_moves(self):
        h = self._load_adapter()
        first_menubar = object()
        second_menubar = object()
        first_window = _FakeWidget(classname="Natron::Gui", menubar=first_menubar)
        second_window = _FakeWidget(classname="Natron::Gui", menubar=second_menubar)
        first_save = _FakeAction()
        second_save = _FakeAction()
        actions = {first_menubar: first_save, second_menubar: second_save}
        h.module.find_qt_menu_items = lambda menubar: [
            {"menuobj": actions[menubar], "menupath": "File/Save"}
        ]

        h.app_holder["instance"] = _make_qapp(active=second_window)
        h.module.launch(_FakeApp())
        # Once shown, the popup is the active window, and a stale popup
        # would lead the focus walk back to the first project.
        stale_popup = _FakeWidget(classname="TabTabTabWidget", parent=first_window)
        h.app_holder["instance"] = _make_qapp(active=stale_popup)

        items = h.module._plugin.get_items()

        self.assertEqual(len(items), 1)
        self.assertIs(items[0]["menuobj"], second_save)
        self.assertIs(h.module._launch_window(), second_window)

    def test_main_window_falls_back_to_focus_when_launch_window_destroyed(self):
        h = self._load_adapter()
        gone = _FakeWidget(classname="Natron::Gui")
        current = _FakeWidget(classname="Natron::Gui")
        h.app_holder["instance"] = _make_qapp(active=gone)
        h.module.launch(_FakeApp())

        h.shiboken6.isValid = lambda obj: obj is not gone
        h.app_holder["instance"] = _make_qapp(active=current)

        self.assertIs(h.module._plugin.main_window(), current)

    def test_get_color_follows_the_current_property(self):
        h = self._load_adapter()
        blur = _plugin_action("Blur", "BlurOFX", color="#111111")
        self._set_nodes_menu(h, [_submenu("Filter", [blur])])
        plugin = h.module.NatronPlugin()
        first = plugin.get_items()[0]["menuobj"]
        self.assertEqual(plugin.get_color(first)[0].value, "#111111")

        blur.setProperty("natronCategoryColor", "#222222")

        self.assertEqual(plugin.get_color(plugin.get_items()[0]["menuobj"])[0].value, "#222222")

    # --- row metrics ---------------------------------------------------------

    def test_sets_row_metrics_from_app_font_height(self):
        h = self._load_adapter()

        h.core.set_row_metrics.assert_called_once_with(
            row_height=_FAKE_FONT_HEIGHT + h.module._ROW_HEIGHT_PADDING,
            visible_rows=h.module._VISIBLE_ROWS,
        )

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
