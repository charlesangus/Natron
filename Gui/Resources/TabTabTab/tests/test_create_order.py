"""TabTabTabWidget.create() and the show that follows it: when the choice
is invoked relative to closing the popup, and where the pre-filled query
is filtered. Runs against stubbed PySide6, without real Qt.
"""

import importlib.util
import os
import sys
import types
from unittest.mock import MagicMock


def _load_core():
    pyside6_stub = types.ModuleType("PySide6")
    qtcore_stub = types.ModuleType("PySide6.QtCore")
    qtgui_stub = types.ModuleType("PySide6.QtGui")
    qtwidgets_stub = types.ModuleType("PySide6.QtWidgets")

    qtcore_stub.Qt = MagicMock()
    qtcore_stub.QEvent = MagicMock()
    qtcore_stub.QAbstractListModel = type("QAbstractListModel", (), {
        "__init__": lambda self, *args, **kwargs: None,
    })
    qtcore_stub.QModelIndex = MagicMock
    qtcore_stub.QSize = MagicMock
    qtcore_stub.QRect = MagicMock
    qtcore_stub.Signal = MagicMock(return_value=MagicMock())
    qtcore_stub.QTimer = MagicMock()

    qtgui_stub.QCursor = MagicMock()
    qtgui_stub.QIcon = MagicMock
    qtgui_stub.QColor = MagicMock
    qtgui_stub.QBrush = MagicMock
    qtgui_stub.QPen = MagicMock

    qtwidgets_stub.QApplication = MagicMock()
    qtwidgets_stub.QDialog = type("QDialog", (), {
        "__init__": lambda self, *args, **kwargs: None,
        "close": lambda self: None,
    })
    qtwidgets_stub.QLineEdit = type("QLineEdit", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets_stub.QListView = type("QListView", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets_stub.QVBoxLayout = type("QVBoxLayout", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets_stub.QStyledItemDelegate = type("QStyledItemDelegate", (), {"__init__": lambda self, *args, **kwargs: None})
    qtwidgets_stub.QStyle = MagicMock()

    pyside6_stub.QtCore = qtcore_stub
    pyside6_stub.QtGui = qtgui_stub
    pyside6_stub.QtWidgets = qtwidgets_stub

    stubs = {
        "PySide6": pyside6_stub,
        "PySide6.QtCore": qtcore_stub,
        "PySide6.QtGui": qtgui_stub,
        "PySide6.QtWidgets": qtwidgets_stub,
    }
    previous = {name: sys.modules.get(name) for name in stubs}
    sys.modules.update(stubs)
    sys.modules.setdefault("PySide2", types.ModuleType("PySide2"))

    path = os.path.join(os.path.dirname(__file__), "..", "tabtabtab_nuke_core.py")
    spec = importlib.util.spec_from_file_location("tabtabtab_core_under_test_create", path)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    finally:
        for name in stubs:
            if previous[name] is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous[name]
    return module


class _Input(object):
    def __init__(self, events, text=""):
        self._events = events
        self._text = text
        self._blocked = False

    def text(self):
        return self._text

    def blockSignals(self, blocked):
        self._blocked = blocked

    def setText(self, text):
        self._events.append(("setText", text, self._blocked))
        self._text = text


class _Plugin(object):
    def __init__(self, events, opens_window, items):
        self._events = events
        self._opens_window = opens_window
        self.items = items

    def invoke_opens_window(self, thing):
        return self._opens_window

    def invoke(self, thing):
        self._events.append(("invoke", thing["menupath"]))

    def get_items(self):
        return self.items

    def invalidate_color_cache(self):
        pass


class _Model(object):
    def __init__(self, events, thing):
        self._events = events
        self._thing = thing

    def getorig(self, selected):
        return self._thing

    def refresh_items(self, items, filtertext=None):
        self._events.append(("refresh_items", filtertext))

    def set_filter(self, text):
        self._events.append(("set_filter", text))

    def invalidate_color_cache(self):
        pass

    def index(self, row):
        return row


class _Weights(object):
    def __init__(self, events):
        self._events = events
        self.version = 0

    def increment(self, key):
        self._events.append(("increment", key))
        self.version += 1

    def reload_if_changed(self):
        pass


def _widget(module, opens_window):
    events = []
    thing = {"text": "Blur [Filter]", "menupath": "Filter/Blur", "menuobj": object()}
    items = [thing]
    widget = object.__new__(module.TabTabTabWidget)
    widget.things = MagicMock()
    widget.things.selectedIndexes.return_value = [0]
    widget.things_model = _Model(events, thing)
    widget.weights = _Weights(events)
    widget.plugin = _Plugin(events, opens_window, items)
    widget.input = _Input(events, text="blu")
    widget.close = lambda: events.append(("close",))
    widget._pending_text = None
    widget._shown_items = items
    widget._shown_weights_version = widget.weights.version
    return widget, events


def test_windowless_choice_is_invoked_before_the_popup_closes():
    module = _load_core()
    widget, events = _widget(module, opens_window=False)

    widget.create()

    assert events == [("increment", "Filter/Blur"), ("invoke", "Filter/Blur"), ("close",)]


def test_choice_that_opens_a_window_closes_the_popup_first():
    module = _load_core()
    widget, events = _widget(module, opens_window=True)

    widget.create()

    assert events == [("increment", "Filter/Blur"), ("close",), ("invoke", "Filter/Blur")]


def test_default_plugin_closes_first():
    module = _load_core()

    assert module.TabTabTabPlugin().invoke_opens_window({"menupath": "x"}) is True


def test_create_leaves_the_input_and_model_alone():
    module = _load_core()
    widget, events = _widget(module, opens_window=False)

    widget.create()

    assert not any(e[0] in ("setText", "set_filter", "refresh_items") for e in events)
    assert widget.input.text() == "blu"
    assert widget._pending_text == " Blur [Filter]"


def test_next_show_filters_the_pending_query_once_with_the_new_weight():
    module = _load_core()
    widget, events = _widget(module, opens_window=False)
    widget.create()
    del events[:]

    widget._refresh_before_show()

    assert events == [("setText", " Blur [Filter]", True), ("refresh_items", " Blur [Filter]")]
    assert widget._pending_text is None
    assert widget._shown_weights_version == widget.weights.version


def test_pending_query_filters_alone_when_rows_are_current():
    module = _load_core()
    widget, events = _widget(module, opens_window=False)
    widget._pending_text = " Blur [Filter]"

    widget._refresh_before_show()

    assert events == [("setText", " Blur [Filter]", True), ("set_filter", " Blur [Filter]")]


def test_show_without_pending_query_does_not_filter():
    module = _load_core()
    widget, events = _widget(module, opens_window=False)

    widget._refresh_before_show()

    assert events == []


def test_pending_query_already_in_the_input_is_not_refiltered():
    module = _load_core()
    widget, events = _widget(module, opens_window=False)
    widget.input = _Input(events, text=" Blur [Filter]")
    widget._pending_text = " Blur [Filter]"

    widget._refresh_before_show()

    assert events == []
