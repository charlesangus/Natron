"""The plain-Qt floor of a node properties panel.

Run as Natron's startup script (``Natron panel_floor.py``) on a real display. For each plug-in in
PANEL_FLOOR_PLUGINS it opens the node's real panel PANEL_FLOOR_REPS times, timing build and first
paint, then copies the panel's widget tree (kinds, texts, sizes, size policies, layouts, dynamic
properties, object names) into plain Qt widgets and times building, showing and painting that copy
in the same place under the app stylesheet, then again with no stylesheet.

The copy is built from Python, so each widget costs a PySide call or several: its times are an
upper bound on what Qt needs, and the real panel's times minus them is what Natron adds.

Results go to stdout and to PANEL_FLOOR_OUT (default ~/natron-panel-floor.txt). Natron quits when
done unless PANEL_FLOOR_EXIT=0.
"""
import os
import statistics
import time
import traceback

from PySide6.QtCore import QEvent, QMetaObject, QObject, QTimer
from PySide6.QtWidgets import (QAbstractButton, QAbstractSlider, QAbstractSpinBox, QApplication, QBoxLayout,
                               QCheckBox, QComboBox, QDoubleSpinBox, QFormLayout, QFrame, QGridLayout,
                               QHBoxLayout, QLabel, QLineEdit, QMainWindow, QPushButton, QRadioButton,
                               QScrollArea, QSizePolicy, QSlider, QSpacerItem, QStackedLayout,
                               QStackedWidget, QTabWidget, QTextEdit, QToolButton, QVBoxLayout, QWidget)

try:
    import shiboken6
except ImportError:
    shiboken6 = None


def env_list(name, default):
    return [v.strip() for v in os.environ.get(name, default).split(",") if v.strip()]


PLUGINS = env_list("PANEL_FLOOR_PLUGINS", "Blur,Grade,Merge,Transform")
REPS = int(os.environ.get("PANEL_FLOOR_REPS", "10"))
OUT = os.path.expanduser(os.environ.get("PANEL_FLOOR_OUT", "~/natron-panel-floor.txt"))
EXIT = os.environ.get("PANEL_FLOOR_EXIT", "1") != "0"
STARTUP_TIMEOUT_S = 120.0
PAINT_TIMEOUT_S = 5.0
SETTLE_MS = 150

_lines = []


def report(line):
    _lines.append(line)
    print(line, flush=True)


def finish():
    try:
        with open(OUT, "w") as f:
            f.write("\n".join(_lines) + "\n")
        print("panel_floor: results written to " + OUT, flush=True)
    except OSError as e:
        print("panel_floor: could not write %s: %s" % (OUT, e), flush=True)
    if EXIT:
        os._exit(0)


def now_ms():
    return time.perf_counter() * 1000.0


def class_name(w):
    return w.metaObject().className().split("::")[-1]


def key(obj):
    return shiboken6.getCppPointer(obj)[0] if shiboken6 else id(obj)


def alive(obj):
    return obj is not None and (shiboken6 is None or shiboken6.isValid(obj))


def natron_app():
    found = globals().get("app")
    if found is not None:
        return found
    import NatronEngine
    return NatronEngine.natron.getInstance(0)


def main_window():
    for w in QApplication.topLevelWidgets():
        if isinstance(w, QMainWindow) and w.isVisible():
            return w
    return None


def root_graph():
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == "NodeGraph" and not w.objectName() and w.isVisible():
                return w
        except RuntimeError:
            continue
    return None


def settings_panels():
    found = []
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == "NodeSettingsPanel":
                found.append(w)
        except RuntimeError:
            continue
    return found


def close_all_panels():
    for w in QApplication.allWidgets():
        try:
            if w.metaObject().indexOfMethod("closePanel()") >= 0 and w.isVisible():
                QMetaObject.invokeMethod(w, "closePanel")
        except RuntimeError:
            continue
    QApplication.processEvents()


# ---- snapshot --------------------------------------------------------------------------------

# Most specific first: a Natron widget is copied as the first standard class it derives from.
KINDS = [
    ("QTabWidget", QTabWidget), ("QScrollArea", QScrollArea), ("QStackedWidget", QStackedWidget),
    ("QComboBox", QComboBox), ("QCheckBox", QCheckBox), ("QRadioButton", QRadioButton),
    ("QToolButton", QToolButton), ("QPushButton", QPushButton), ("QAbstractButton", QPushButton),
    ("QLineEdit", QLineEdit), ("QTextEdit", QTextEdit), ("QAbstractSpinBox", QAbstractSpinBox),
    ("QAbstractSlider", QAbstractSlider), ("QLabel", QLabel), ("QFrame", QFrame), ("QWidget", QWidget),
]
# Their children are Qt's own (viewport, scroll bars, line edit, tab bar), which the copy makes itself.
LEAF_KINDS = {"QComboBox", "QLineEdit", "QTextEdit", "QAbstractSpinBox", "QAbstractSlider"}


def kind_of(w):
    for name, cls in KINDS:
        if isinstance(w, cls):
            return name
    return "QWidget"


def snap_policy(p):
    return (p.horizontalPolicy(), p.verticalPolicy(), p.horizontalStretch(), p.verticalStretch())


def snap_widget(w, counts):
    kind = kind_of(w)
    counts[kind] = counts.get(kind, 0) + 1
    node = {
        "kind": kind, "source": class_name(w), "name": w.objectName(), "hidden": w.isHidden(),
        "min": (w.minimumWidth(), w.minimumHeight()), "max": (w.maximumWidth(), w.maximumHeight()),
        "policy": snap_policy(w.sizePolicy()), "style": w.styleSheet(), "tooltip": w.toolTip(),
        "props": [(n, w.property(n)) for n in (bytes(b).decode() for b in w.dynamicPropertyNames())
                  if not n.startswith("_q_")],
        "geometry": (w.x(), w.y(), w.width(), w.height()), "children": [], "layout": None,
    }
    if isinstance(w, (QLabel, QAbstractButton, QLineEdit)):
        node["text"] = w.text()
    if isinstance(w, QAbstractButton):
        node["checkable"] = w.isCheckable()
        node["checked"] = w.isChecked()
    if isinstance(w, QComboBox):
        node["items"] = [w.itemText(i) for i in range(w.count())]
        node["current"] = w.currentIndex()
    if isinstance(w, QAbstractSpinBox):
        node["text"] = w.text()
    if isinstance(w, QAbstractSlider):
        node["range"] = (w.minimum(), w.maximum(), w.value(), int(w.orientation().value))
    if kind == "QTabWidget":
        node["tabs"] = [(w.tabText(i), snap_widget(w.widget(i), counts)) for i in range(w.count())]
        node["tab"] = w.currentIndex()
        return node
    if kind == "QScrollArea":
        inner = w.widget()
        node["inner"] = snap_widget(inner, counts) if inner is not None else None
        node["resizable"] = w.widgetResizable()
        return node
    if kind == "QStackedWidget":
        node["pages"] = [snap_widget(w.widget(i), counts) for i in range(w.count())]
        node["page"] = w.currentIndex()
        return node
    if kind in LEAF_KINDS:
        return node
    managed = set()
    if w.layout() is not None:
        node["layout"] = snap_layout(w.layout(), counts, managed)
    for child in w.children():
        if isinstance(child, QWidget) and not child.isWindow() and key(child) not in managed:
            node["children"].append(snap_widget(child, counts))
    return node


def snap_layout(layout, counts, managed):
    if isinstance(layout, QGridLayout):
        kind = "grid"
    elif isinstance(layout, QFormLayout):
        kind = "form"
    elif isinstance(layout, QStackedLayout):
        kind = "stacked"
    elif isinstance(layout, QBoxLayout):
        kind = "h" if layout.direction() in (QBoxLayout.LeftToRight, QBoxLayout.RightToLeft) else "v"
    else:
        kind = "v"
    m = layout.contentsMargins()
    node = {"kind": kind, "margins": (m.left(), m.top(), m.right(), m.bottom()), "spacing": layout.spacing(),
            "items": []}
    if kind == "grid":
        node["hv"] = (layout.horizontalSpacing(), layout.verticalSpacing())
        node["stretch"] = ([layout.columnStretch(c) for c in range(layout.columnCount())],
                           [layout.rowStretch(r) for r in range(layout.rowCount())])
    for i in range(layout.count()):
        item = layout.itemAt(i)
        entry = {}
        if kind == "grid":
            entry["pos"] = tuple(layout.getItemPosition(i))
        elif kind in ("h", "v"):
            entry["stretch"] = layout.stretch(i)
        if item.widget() is not None:
            managed.add(key(item.widget()))
            entry["widget"] = snap_widget(item.widget(), counts)
        elif item.layout() is not None:
            entry["layout"] = snap_layout(item.layout(), counts, managed)
        elif item.spacerItem() is not None:
            s = item.spacerItem()
            hint = s.sizeHint()
            policy = s.sizePolicy()
            entry["spacer"] = (hint.width(), hint.height(), policy.horizontalPolicy(), policy.verticalPolicy())
        else:
            continue
        node["items"].append(entry)
    return node


# ---- plain-Qt copy ---------------------------------------------------------------------------

def make_widget(node, parent):
    kind = node["kind"]
    if kind == "QAbstractButton":
        w = QPushButton(parent)
    elif kind == "QAbstractSpinBox":
        w = QDoubleSpinBox(parent)
        w.setRange(-1e9, 1e9)
        w.setDecimals(3)
        try:
            w.setValue(float(node.get("text") or 0))
        except ValueError:
            pass
    elif kind == "QAbstractSlider":
        w = QSlider(parent)
    else:
        w = dict(KINDS)[kind](parent)
    if node["name"]:
        w.setObjectName(node["name"])
    for name, value in node["props"]:
        w.setProperty(name, value)
    if node["style"]:
        w.setStyleSheet(node["style"])
    if node["tooltip"]:
        w.setToolTip(node["tooltip"])
    w.setMinimumSize(*node["min"])
    w.setMaximumSize(*node["max"])
    hp, vp, hs, vs = node["policy"]
    policy = QSizePolicy(hp, vp)
    policy.setHorizontalStretch(hs)
    policy.setVerticalStretch(vs)
    w.setSizePolicy(policy)
    if "text" in node and hasattr(w, "setText") and kind != "QAbstractSpinBox":
        w.setText(node["text"])
    if node.get("checkable"):
        w.setCheckable(True)
        w.setChecked(node["checked"])
    if "items" in node:
        w.addItems(node["items"])
        w.setCurrentIndex(node["current"])
    if "range" in node:
        lo, hi, value, orientation = node["range"]
        w.setRange(lo, hi)
        w.setValue(value)
        w.setOrientation(type(w.orientation())(orientation))
    if kind == "QTabWidget":
        for text, page in node["tabs"]:
            w.addTab(make_widget(page, None), text)
        w.setCurrentIndex(node["tab"])
    elif kind == "QScrollArea":
        w.setWidgetResizable(node["resizable"])
        if node["inner"] is not None:
            w.setWidget(make_widget(node["inner"], None))
    elif kind == "QStackedWidget":
        for page in node["pages"]:
            w.addWidget(make_widget(page, None))
        w.setCurrentIndex(node["page"])
    if node["layout"] is not None:
        w.setLayout(make_layout(node["layout"]))
    for child in node["children"]:
        c = make_widget(child, w)
        c.setGeometry(*child["geometry"])
    if node["hidden"]:
        w.setHidden(True)
    return w


def make_layout(node):
    kind = node["kind"]
    if kind == "grid":
        layout = QGridLayout()
        layout.setHorizontalSpacing(node["hv"][0])
        layout.setVerticalSpacing(node["hv"][1])
    elif kind == "h":
        layout = QHBoxLayout()
    elif kind == "stacked":
        layout = QStackedLayout()
    else:
        layout = QVBoxLayout()
    layout.setContentsMargins(*node["margins"])
    if kind != "grid" and node["spacing"] >= 0:
        layout.setSpacing(node["spacing"])
    for entry in node["items"]:
        if "widget" in entry:
            item = make_widget(entry["widget"], None)
        elif "layout" in entry:
            item = make_layout(entry["layout"])
        else:
            w, h, hp, vp = entry["spacer"]
            item = QSpacerItem(w, h, hp, vp)
        if kind == "grid":
            r, c, rs, cs = entry["pos"]
            if isinstance(item, QWidget):
                layout.addWidget(item, r, c, rs, cs)
            elif isinstance(item, QSpacerItem):
                layout.addItem(item, r, c, rs, cs)
            else:
                layout.addLayout(item, r, c, rs, cs)
        elif kind == "stacked":
            if isinstance(item, QWidget):
                layout.addWidget(item)
        else:
            if isinstance(item, QWidget):
                layout.addWidget(item, entry.get("stretch", 0))
            elif isinstance(item, QSpacerItem):
                layout.addItem(item)
            else:
                layout.addLayout(item, entry.get("stretch", 0))
    if kind == "grid":
        for c, s in enumerate(node["stretch"][0]):
            layout.setColumnStretch(c, s)
        for r, s in enumerate(node["stretch"][1]):
            layout.setRowStretch(r, s)
    return layout


# ---- timing ----------------------------------------------------------------------------------

class PaintWatch(QObject):
    """First Paint on the watched widget, and the end of the event-loop pass that painted it."""

    def __init__(self):
        QObject.__init__(self)
        self.paint_ms = None
        self.frame_ms = None

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Paint and self.paint_ms is None:
            self.paint_ms = now_ms()
            QTimer.singleShot(0, self._frame_done)
        return False

    def _frame_done(self):
        self.frame_ms = now_ms()


def summary(values):
    if not values:
        return "n/a"
    ordered = sorted(values)
    p90 = ordered[min(len(ordered) - 1, int(round(0.9 * (len(ordered) - 1))))]
    return "median %7.2f  p90 %7.2f  max %7.2f" % (statistics.median(ordered), p90, ordered[-1])


def steps():
    natron = natron_app()
    ok = yield ("until", lambda: main_window() is not None and root_graph() is not None, STARTUP_TIMEOUT_S)
    if not ok:
        report("panel_floor: the node graph was not visible within %.0f s" % STARTUP_TIMEOUT_S)
        return
    yield ("sleep", 1000)
    qapp = QApplication.instance()
    report("panel_floor: %d reps per plug-in; times in ms from the start of the build" % REPS)
    for pid in PLUGINS:
        real = {"build": [], "paint": [], "frame": []}
        snapshot = None
        counts = {}
        container = None
        for rep in range(REPS):
            close_all_panels()
            natron.clearSelection()
            node = natron.createNode(pid)
            if node is None:
                report("%s: createNode returned None; skipped" % pid)
                break
            natron.setSelection([node])
            yield ("sleep", SETTLE_MS)
            before = set(key(p) for p in settings_panels())
            t0 = now_ms()
            QMetaObject.invokeMethod(root_graph(), "showSelectedNodeSettingsPanel")
            built = now_ms()
            fresh = [p for p in settings_panels() if key(p) not in before]
            if len(fresh) != 1:
                report("%s: opening the panel built %d panels; skipped" % (pid, len(fresh)))
                node.destroy(False)
                break
            panel = fresh[0]
            watch = PaintWatch()
            panel.installEventFilter(watch)
            yield ("until", lambda: watch.frame_ms is not None, PAINT_TIMEOUT_S)
            if alive(panel):
                panel.removeEventFilter(watch)
            real["build"].append(built - t0)
            if watch.frame_ms is not None:
                real["paint"].append(watch.paint_ms - t0)
                real["frame"].append(watch.frame_ms - t0)
            if snapshot is None and alive(panel):
                yield ("sleep", SETTLE_MS)
                snapshot = snap_widget(panel, counts)
                container = panel.parentWidget()
            close_all_panels()
            node.destroy(False)
            yield ("sleep", SETTLE_MS)
        if snapshot is None or not alive(container) or container.layout() is None:
            report("%s: no panel to copy" % pid)
            continue
        report("%s: %d widgets copied (%s)" % (pid, sum(counts.values()),
                                               ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
        report("  real panel   build  %s" % summary(real["build"]))
        report("               paint  %s" % summary(real["paint"]))
        report("               frame  %s" % summary(real["frame"]))
        stylesheet = qapp.styleSheet()
        passes = [("app stylesheet", None), ("no stylesheet", "")]
        for label, sheet in passes:
            if sheet is not None:
                qapp.setStyleSheet(sheet)
                yield ("sleep", 500)
            floor = {"build": [], "show": [], "paint": [], "frame": []}
            for rep in range(REPS):
                if not alive(container):
                    break
                t0 = now_ms()
                clone = make_widget(snapshot, None)
                built = now_ms()
                watch = PaintWatch()
                clone.installEventFilter(watch)
                clone.setParent(container)
                if isinstance(container.layout(), QBoxLayout):
                    container.layout().insertWidget(0, clone)
                else:
                    container.layout().addWidget(clone)
                clone.show()
                shown = now_ms()
                yield ("until", lambda: watch.frame_ms is not None, PAINT_TIMEOUT_S)
                floor["build"].append(built - t0)
                floor["show"].append(shown - t0)
                if watch.frame_ms is not None:
                    floor["paint"].append(watch.paint_ms - t0)
                    floor["frame"].append(watch.frame_ms - t0)
                clone.removeEventFilter(watch)
                clone.hide()
                container.layout().removeWidget(clone)
                clone.setParent(None)
                clone.deleteLater()
                clone = None
                yield ("sleep", SETTLE_MS)
            report("  plain Qt, %s" % label)
            report("               build  %s" % summary(floor["build"]))
            report("               show   %s" % summary(floor["show"]))
            report("               paint  %s" % summary(floor["paint"]))
            report("               frame  %s" % summary(floor["frame"]))
        if qapp.styleSheet() != stylesheet:
            qapp.setStyleSheet(stylesheet)
            yield ("sleep", 500)
    report("panel_floor: build = widgets made; show = parented, laid out and shown; paint = first paint "
           "event; frame = the event-loop pass that painted it has returned")


def run(gen):
    def advance(value):
        try:
            cmd = gen.send(value)
        except StopIteration:
            finish()
            return
        except Exception:
            report("panel_floor: error: " + traceback.format_exc())
            finish()
            return
        if cmd[0] == "sleep":
            QTimer.singleShot(cmd[1], lambda: advance(None))
            return
        predicate, timeout = cmd[1], cmd[2]
        deadline = time.time() + timeout

        def poll():
            try:
                ok = bool(predicate())
            except Exception:
                ok = False
            if ok or time.time() > deadline:
                advance(ok)
            else:
                QTimer.singleShot(5, poll)

        poll()

    QTimer.singleShot(0, lambda: advance(None))


run(steps())
