"""Tests for the pluggable host hooks: the main-window finder
(set_main_window_finder / _find_main_window), the row metrics override
(set_row_metrics / _row_height / _visible_rows), and the popup placement
helper (_popup_position).

Covers:
  - _find_main_window() defaulting to _find_nuke_main_window() when no
    finder has been registered
  - _create_tabtabtab_widget() passing the registered finder's result as
    the widget's parent
  - _row_height()/_visible_rows() defaulting to the built-in Nuke sizing
    until set_row_metrics() overrides them
  - _popup_position() keeping the search box at the cursor when the
    popup fits below it, and flipping above it when it doesn't
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


# --- row metrics (set_row_metrics / _row_height / _visible_rows) -----------


def test_row_metrics_default_to_nuke_sizing_when_unset():
    module, _QMainWindow = _load_module([])

    assert module._row_height(10) == 20  # twice the font height
    assert module._visible_rows() == 18


def test_set_row_metrics_overrides_both_values():
    module, _QMainWindow = _load_module([])

    module.set_row_metrics(row_height=24, visible_rows=15)

    assert module._row_height(10) == 24
    assert module._visible_rows() == 15


def test_set_row_metrics_omitted_argument_falls_back_to_default():
    module, _QMainWindow = _load_module([])

    module.set_row_metrics(row_height=24)

    assert module._row_height(10) == 24
    assert module._visible_rows() == 18  # visible_rows wasn't passed


# --- popup placement (_popup_position) --------------------------------------


class _Rect(object):
    """Minimal stand-in for a QRect, exposing only what _popup_position uses."""

    def __init__(self, left, top, right, bottom):
        self._left, self._top, self._right, self._bottom = left, top, right, bottom

    def left(self):
        return self._left

    def top(self):
        return self._top

    def right(self):
        return self._right

    def bottom(self):
        return self._bottom


def test_popup_position_keeps_edit_at_cursor_when_it_fits_below():
    module, _QMainWindow = _load_module([])
    screen = _Rect(0, 0, 1920, 1199)

    xpos, ypos, flipped = module._popup_position(500, 200, 400, 300, screen)

    assert flipped is False
    assert ypos == 200 - 13  # search box lands right at the cursor


def test_popup_position_flips_above_when_it_does_not_fit_below():
    module, _QMainWindow = _load_module([])
    # A popup taller than the room below the cursor on a short screen,
    # but which fits above it (cursor is near the bottom of the screen).
    screen = _Rect(0, 0, 1920, 999)

    xpos, ypos, flipped = module._popup_position(960, 900, 400, 757, screen)

    assert flipped is True
    assert ypos >= screen.top()
    assert ypos + 757 <= screen.bottom() + 1


def test_popup_position_clamps_on_screen_when_it_fits_neither_way():
    module, _QMainWindow = _load_module([])
    # A popup taller than the entire screen: can't fit above or below
    # the cursor in either orientation.
    screen = _Rect(0, 0, 1920, 999)

    xpos, ypos, flipped = module._popup_position(960, 500, 400, 2000, screen)

    assert flipped is False
    assert ypos >= screen.top()
