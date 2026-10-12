"""Tests for the pluggable main-window finder (set_main_window_finder /
_find_main_window).

Covers:
  - _find_main_window() defaulting to _find_nuke_main_window() when no
    finder has been registered
  - _create_tabtabtab_widget() passing the registered finder's result as
    the widget's parent
"""

import importlib.util
import os
import sys
import types
from unittest.mock import MagicMock


_NUKE_DOCK_CLASSNAME = "Foundry::UI::DockMainWindow"


def _build_pyside_stubs(top_level_widgets_holder):
    """Construct minimal PySide6 stub modules, with QApplication.instance()
    reading from the mutable `top_level_widgets_holder` list.

    Mirrors test_main_window_discovery.py's stub builder; only the bits
    this file's tests touch are included.
    """
    pyside6 = types.ModuleType("PySide6")
    qtcore = types.ModuleType("PySide6.QtCore")
    qtgui = types.ModuleType("PySide6.QtGui")
    qtwidgets = types.ModuleType("PySide6.QtWidgets")

    qtcore.Qt = MagicMock()
    qtcore.QEvent = MagicMock()
    qtcore.QAbstractListModel = type("QAbstractListModel", (), {
        "__init__": lambda self, *args, **kwargs: None,
    })
    qtcore.QModelIndex = MagicMock
    qtcore.QSize = MagicMock
    qtcore.QRect = MagicMock
    qtcore.Signal = MagicMock(return_value=MagicMock())
    qtcore.QTimer = MagicMock()

    qtgui.QCursor = MagicMock()
    qtgui.QIcon = MagicMock
    qtgui.QColor = MagicMock
    qtgui.QBrush = MagicMock
    qtgui.QPen = MagicMock

    qtwidgets.QDialog = type("QDialog", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets.QLineEdit = type("QLineEdit", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets.QListView = type("QListView", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets.QVBoxLayout = type("QVBoxLayout", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets.QStyledItemDelegate = type("QStyledItemDelegate", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets.QStyle = MagicMock()
    qtwidgets.QMainWindow = type("QMainWindow", (), {
        "__init__": lambda self, *args, **kwargs: None,
    })

    class _StubQApplication:
        @staticmethod
        def instance():
            app_mock = MagicMock()
            app_mock.topLevelWidgets.return_value = list(top_level_widgets_holder)
            return app_mock

    qtwidgets.QApplication = _StubQApplication

    pyside6.QtCore = qtcore
    pyside6.QtGui = qtgui
    pyside6.QtWidgets = qtwidgets

    return pyside6, qtcore, qtgui, qtwidgets


def _load_module(top_level_widgets_holder):
    """Load tabtabtab_nuke_core with stubs wired to the given holder.

    Returns (module, QMainWindowStub).
    """
    pyside6, qtcore, qtgui, qtwidgets = _build_pyside_stubs(top_level_widgets_holder)

    stub_names = ["PySide6", "PySide6.QtCore", "PySide6.QtGui", "PySide6.QtWidgets"]
    previous = {name: sys.modules.get(name) for name in stub_names}

    sys.modules["PySide6"] = pyside6
    sys.modules["PySide6.QtCore"] = qtcore
    sys.modules["PySide6.QtGui"] = qtgui
    sys.modules["PySide6.QtWidgets"] = qtwidgets

    pyside2_stub = types.ModuleType("PySide2")
    sys.modules.setdefault("PySide2", pyside2_stub)

    core_path = os.path.join(os.path.dirname(__file__), "..", "tabtabtab_nuke_core.py")
    spec = importlib.util.spec_from_file_location(
        "tabtabtab_core_host_hooks_test", core_path
    )
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    finally:
        for name in stub_names:
            if previous[name] is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous[name]

    return module, qtwidgets.QMainWindow


def _make_mock_main_window(qmainwindow_class, classname="", title=""):
    instance = qmainwindow_class.__new__(qmainwindow_class)
    instance._classname = classname
    instance._title = title

    def metaObject():
        meta = MagicMock()
        meta.className.return_value = instance._classname
        return meta

    instance.metaObject = metaObject
    instance.parent = lambda: None
    instance.windowTitle = lambda: instance._title
    return instance


def test_find_main_window_defaults_to_nuke_finder_when_no_finder_set():
    top_level = []
    module, QMainWindow = _load_module(top_level)

    dock_main = _make_mock_main_window(QMainWindow, classname=_NUKE_DOCK_CLASSNAME)
    top_level.append(dock_main)

    assert module._find_main_window() is module._find_nuke_main_window()
    assert module._find_main_window() is dock_main


def test_create_widget_uses_registered_finder_as_parent():
    top_level = []
    module, _QMainWindow = _load_module(top_level)

    sentinel = object()
    module.set_main_window_finder(lambda: sentinel)

    captured = {}

    def fake_widget_ctor(plugin, parent=None, winflags=None, space_mode_order=None, scroll_enabled=False):
        captured["parent"] = parent
        return MagicMock()

    module.TabTabTabWidget = fake_widget_ctor

    module._create_tabtabtab_widget(plugin=MagicMock(), space_mode_order=None)

    assert captured["parent"] is sentinel
