"""Checks that a node-graph pipe carrying geometry takes the Native 3D node colour.

Usage, from the repository root: Tests/gui/run-gui-test.sh Tests/gui/geometry_edge_colour.py

Graph: GeoPrimitive -> GeoTransform, stacked, nothing selected and nothing rendering.

Asserts:
  - after the Native 3D colour preference is changed and saved, the pipe follows the new colour;
  - the pipe between the two nodes is drawn in that colour: across a strip at the pipe's middle,
    the drawn pixel closest to the preference is within a few levels of it.

Shots: geometry-edge-colour.png and geometry-edge-colour-changed.png (the node graph around the pipe).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 500
WATCHDOG_S = 120
NODE_GAP = 300
STRIP_HALF_WIDTH = 8
# Antialiasing blends the pipe's edges with the background; its middle keeps the pen colour.
MAX_CHANNEL_ERROR = 12

try:
    import NatronEngine
    import NatronGui
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QColor
    from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton

    import guitest as gt
    from guitest import check, menu_of, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        if not views:
            return None
        _held.append(views[0])
        return views[0]

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def colour_of(param):
        value = param.get()
        return QColor.fromRgbF(min(max(value.r, 0.0), 1.0), min(max(value.g, 0.0), 1.0),
                               min(max(value.b, 0.0), 1.0))

    def channel_error(a, b):
        return max(abs(a.red() - b.red()), abs(a.green() - b.green()), abs(a.blue() - b.blue()))

    def open_preferences():
        for win in QApplication.topLevelWidgets():
            if not isinstance(win, QMainWindow) or win.menuBar() is None:
                continue
            for act in win.menuBar().actions():
                menu = menu_of(act)
                if menu is None or "edit" not in act.text().lower():
                    continue
                for a in menu.actions():
                    if "preferences" in a.text().lower():
                        a.trigger()
                        QApplication.processEvents()
                        panels = [w for w in gt.widgets_of("PreferencesPanel") if w.isVisible()]
                        return panels[0] if len(panels) == 1 else None
        return None

    def pipe_pixel(view, middle, expected):
        best = pipe_pixel(view, middle, expected)
        require(best is not None, "the pipe's middle is inside the node graph view")
        check(channel_error(best, expected) <= MAX_CHANNEL_ERROR,
              "the GeoPrimitive -> GeoTransform pipe is drawn in the Native 3D colour %s (closest pixel %s)"
              % (expected.name(), best.name()))

        gt.section("geometry edge follows the preference")
        changed = QColor(40, 220, 60)
        panel = open_preferences()
        require(panel is not None, "the Preferences panel opens")
        native.set(changed.redF(), changed.greenF(), changed.blueF())
        save = [w for w in panel.findChildren(QPushButton) if w.text() == "Save" and w.isVisible()]
        require(len(save) == 1, "found the Preferences Save button")
        save[0].click()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        view = graph_view()
        view.centerOn(middle)
        yield ("sleep", SETTLE_MS)
        gt.shot("geometry-edge-colour-changed.png")
        best = pipe_pixel(view, middle, changed)
        require(best is not None, "the pipe's middle is inside the node graph view")
        check(channel_error(best, changed) <= MAX_CHANNEL_ERROR,
              "after changing the Native 3D colour the pipe is drawn in %s (closest pixel %s)"
              % (changed.name(), best.name()))
        native.set(*original)
        yield ("sleep", SETTLE_MS)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.report("FAIL script error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
