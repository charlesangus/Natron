"""A project carries node layout (positions, backdrop size, colours) but no workspace: saving a
.ntpz with two Viewers, a moved and sized backdrop, a coloured node, a node inside a Group, an open
settings panel and a selection, then reopening it after splitting a pane, must keep the node
layout and leave the pane layout, panels and selection as they were before opening. Moving a node
from Python and by a mouse drag shows in getPosition(), and a copy/paste of two nodes keeps their
relative offset.

The overlay colour is not checked: Effect has no Python accessor for it. The anchor pane is not
exposed to Python either, so it is taken to be the pane the first Viewer's tab landed in.

Usage (natron-dev container):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        Tests/gui/run-gui-test.sh Tests/gui/m16_project_layout.py'

Drags follow Tests/gui/m55_node_graph.py: synthetic QMouseEvents sent to the NodeGraph viewport at
positions mapped through mapFromScene(), with the main window active first.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 60.0
WATCHDOG_S = 240
SETTLE_MS = 500
DRAG_STEPS = 4
POS_TOL = 0.01
COLOR_TOL = 1e-3
DRAG_TOL = 6.0

VIEWER = "fr.inria.built-in.Viewer"
BACKDROP = "fr.inria.built-in.BackDrop"
GROUP = "fr.inria.built-in.Group"
BLUR = "net.sf.cimg.CImgBlur"
GRADE = "net.sf.openfx.GradePlugin"

BACKDROP_POS = (-420.0, -310.0)
BACKDROP_SIZE = (610.0, 390.0)
BACKDROP_COLOR = (0.2, 0.5, 0.3)
BLUR_POS = (110.0, 40.0)
BLUR_COLOR = (0.9, 0.1, 0.15)
GRADE_POS = (330.0, 170.0)
GROUP_POS = (700.0, 20.0)
CHILD_POS = (123.0, 456.0)

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtWidgets import QApplication, QMainWindow, QSplitter
    from PySide6.QtGui import QMouseEvent

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    PROJECT_PATH = os.path.join(gt.OUT, "m16_project_layout.ntpz")

    def create(plugin_id, x, y, group=None, open_panel=False):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(open_panel)}
        node = app.createNode(plugin_id, -1, group, props)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def close(a, b, tol):
        return all(abs(p - q) <= tol for p, q in zip(a, b))

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def pane_names():
        names = []
        for w in gt.widgets_of("TabWidget"):
            try:
                if w.isVisible() and w.objectName():
                    names.append(w.objectName())
            except RuntimeError:
                continue
        return sorted(names)

    def tab_labels(name):
        tw = app.getTabWidget(name)
        if tw is None:
            return []
        return [tw.getTabLabel(i) for i in range(tw.count())]

    def pane_holding(label):
        for name in pane_names():
            if any(label in t for t in tab_labels(name)):
                return name
        return None

    def layout_snapshot(viewer_labels):
        """Each visible pane with the orientation of the splitter holding it and its tabs other
        than viewers, which a project is allowed to add to the anchor pane."""
        snap = []
        for w in gt.widgets_of("TabWidget"):
            try:
                if not w.isVisible() or not w.objectName():
                    continue
                parent = w.parentWidget()
                orient = int(parent.orientation().value) if isinstance(parent, QSplitter) else -1
            except RuntimeError:
                continue
            tabs = tuple(t for t in tab_labels(w.objectName())
                         if not any(v in t for v in viewer_labels))
            snap.append((w.objectName(), orient, tabs))
        return sorted(snap)

    def box(effect):
        x, y = effect.getPosition()
        w, h = effect.getSize()
        return x, y, w, h

    def frame_on(graph, effect, zoom=1.0):
        x, y, w, h = box(effect)
        graph.resetTransform()
        graph.scale(zoom, zoom)
        graph.centerOn(x + w / 2.0, y + h / 2.0)
        QApplication.processEvents()

    def send_mouse(widget, etype, pos, button, buttons):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, Qt.NoModifier))
        QApplication.processEvents()

    def drag_by(graph, effect, dx, dy):
        x, y, w, h = box(effect)
        start = (x + w / 2.0, y + h / 2.0)
        vp = graph.viewport()
        send_mouse(vp, QEvent.MouseButtonPress, graph.mapFromScene(QPointF(*start)), Qt.LeftButton, Qt.LeftButton)
        pos = start
        for i in range(1, DRAG_STEPS + 1):
            f = i / float(DRAG_STEPS)
            pos = (start[0] + dx * f, start[1] + dy * f)
            send_mouse(vp, QEvent.MouseMove, graph.mapFromScene(QPointF(*pos)), Qt.NoButton, Qt.LeftButton)
        send_mouse(vp, QEvent.MouseButtonRelease, graph.mapFromScene(QPointF(*pos)), Qt.LeftButton, Qt.NoButton)

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("build")
        viewer1 = create(VIEWER, -600, 300)
        viewer2 = create(VIEWER, -400, 300)
        yield ("sleep", SETTLE_MS)
        viewer_labels = [viewer1.getLabel(), viewer2.getLabel()]
        anchor = pane_holding(viewer_labels[0])
        require(anchor is not None, "the first Viewer has a tab in some pane",
                "panes %r" % [(n, tab_labels(n)) for n in pane_names()])
        report("NOTE anchor pane taken to be %s, tabs %r" % (anchor, tab_labels(anchor)))
        check(pane_holding(viewer_labels[1]) == anchor, "both Viewers are tabs of the anchor pane before saving")

        backdrop = create(BACKDROP, *BACKDROP_POS)
        backdrop.setSize(*BACKDROP_SIZE)
        backdrop.setColor(*BACKDROP_COLOR)
        blur = create(BLUR, *BLUR_POS)
        blur.setColor(*BLUR_COLOR)
        group = create(GROUP, *GROUP_POS)
        child = create(BLUR, CHILD_POS[0], CHILD_POS[1], group=group)
        grade = create(GRADE, GRADE_POS[0], GRADE_POS[1], open_panel=True)
        ok = yield ("until", gt.any_panel_open, 20.0)
        check(ok, "a settings panel is open when the project is saved")
        app.selectNode(blur, True)
        app.selectNode(grade, False)
        check(len(app.getSelectedNodes()) == 2, "two nodes are selected when the project is saved")
        yield ("sleep", SETTLE_MS)

        names = {
            "backdrop": backdrop.getScriptName(),
            "blur": blur.getScriptName(),
            "grade": grade.getScriptName(),
            "group": group.getScriptName(),
            "child": group.getScriptName() + "." + child.getScriptName(),
        }
        backdrop_saved_color = backdrop.getColor()
        blur_saved_color = blur.getColor()
        check(close(backdrop_saved_color, BACKDROP_COLOR, COLOR_TOL),
              "the backdrop reports the colour it was given (got %r)" % (backdrop_saved_color,))
        check(close(blur_saved_color, BLUR_COLOR, COLOR_TOL),
              "the Blur reports the colour it was given (got %r)" % (blur_saved_color,))
        require(app.saveProjectAs(PROJECT_PATH), "save the project to " + PROJECT_PATH)

        gt.section("reopen")
        require(app.resetProject(), "reset the project (close it, keep the window)")
        yield ("sleep", SETTLE_MS)
        split_from = next((n for n in pane_names() if n != anchor), None)
        require(split_from is not None, "a pane other than the anchor exists to split")
        split = app.getTabWidget(split_from).splitVertically()
        require(split is not None, "split pane " + split_from)
        yield ("sleep", SETTLE_MS)
        layout_before = layout_snapshot(viewer_labels)
        report("NOTE pane layout before opening: %r" % (layout_before,))
        gt.shot("m16-layout-before-open.png")

        reloaded = app.loadProject(PROJECT_PATH)
        require(reloaded is not None, "reopen " + PROJECT_PATH)
        ok = yield ("until", lambda: all(app.getNode(n) is not None for n in names.values()), LOAD_TIMEOUT_S)
        require(ok, "every saved node is back after reopening")
        yield ("sleep", SETTLE_MS)
        gt.shot("m16-layout-after-open.png")

        layout_after = layout_snapshot(viewer_labels)
        report("NOTE pane layout after opening: %r" % (layout_after,))
        check(layout_after == layout_before, "opening the project leaves the pane layout as it was")
        check(split.getScriptName() in pane_names(), "the pane split before opening is still there")
        anchor_tabs = tab_labels(anchor)
        for label in viewer_labels:
            check(any(label in t for t in anchor_tabs),
                  "%s is a tab of the anchor pane %s (tabs %r)" % (label, anchor, anchor_tabs))
        check(not gt.any_panel_open(), "no settings panel is open after opening")
        check(len(app.getSelectedNodes()) == 0, "nothing is selected in the root graph after opening")
        r_group = app.getNode(names["group"])
        check(len(app.getSelectedNodes(r_group)) == 0, "nothing is selected inside the Group after opening")
        check(not any(app.getNode(n).isNodeSelected() for n in names.values()),
              "no reopened node reports itself selected")

        r_backdrop = app.getNode(names["backdrop"])
        r_blur = app.getNode(names["blur"])
        r_grade = app.getNode(names["grade"])
        r_child = app.getNode(names["child"])
        for label, node, want in (("backdrop", r_backdrop, BACKDROP_POS), ("Blur", r_blur, BLUR_POS),
                                  ("Grade", r_grade, GRADE_POS), ("Group", r_group, GROUP_POS),
                                  ("Group child", r_child, CHILD_POS)):
            got = node.getPosition()
            check(close(got, want, POS_TOL), "%s position survives reopening (got %r, want %r)" % (label, got, want))
        got = r_backdrop.getSize()
        check(close(got, BACKDROP_SIZE, POS_TOL),
              "backdrop size survives reopening (got %r, want %r)" % (got, BACKDROP_SIZE))
        got = r_backdrop.getColor()
        check(close(got, backdrop_saved_color, COLOR_TOL),
              "backdrop colour survives reopening (got %r, want %r)" % (got, backdrop_saved_color))
        got = r_blur.getColor()
        check(close(got, blur_saved_color, COLOR_TOL),
              "Blur user colour survives reopening (got %r, want %r)" % (got, blur_saved_color))

        gt.section("move")
        target = (BLUR_POS[0] + 250.0, BLUR_POS[1] + 40.0)
        r_blur.setPosition(*target)
        got = r_blur.getPosition()
        check(close(got, target, POS_TOL), "setPosition shows in getPosition (got %r, want %r)" % (got, target))

        win = main_window()
        require(win is not None, "found the main window")
        win.activateWindow()
        win.raise_()
        ok = yield ("until", lambda: win.isActiveWindow(), 10.0)
        require(ok, "the main window is active",
                "QGraphicsItem::isActive() is false in an inactive window, so presses miss every node")
        app.clearSelection()
        graph = root_graph()
        require(graph is not None, "the root node graph is visible")
        frame_on(graph, r_blur)
        yield ("sleep", SETTLE_MS)
        before = r_blur.getPosition()
        drag_by(graph, r_blur, 170.0, 130.0)
        yield ("sleep", SETTLE_MS)
        after = r_blur.getPosition()
        moved = (after[0] - before[0], after[1] - before[1])
        check(close(moved, (170.0, 130.0), DRAG_TOL),
              "a mouse drag shows in getPosition (moved by %r, dragged by (170, 130))" % (moved,))

        gt.section("copy-paste")
        bx, by = r_blur.getPosition()
        gx, gy = r_grade.getPosition()
        existing = set(n.getScriptName() for n in app.getChildren())
        app.clearSelection()
        app.selectNode(r_blur, True)
        app.selectNode(r_grade, False)
        app.copySelectedNodes()
        app.pasteNodes()
        yield ("sleep", SETTLE_MS)
        pasted = [n for n in app.getChildren() if n.getScriptName() not in existing]
        if check(len(pasted) == 2, "pasting two copied nodes adds two nodes (got %d)" % len(pasted)):
            p_blur = next((n for n in pasted if n.getPluginID() == r_blur.getPluginID()), None)
            p_grade = next((n for n in pasted if n.getPluginID() == r_grade.getPluginID()), None)
            if check(p_blur is not None and p_grade is not None, "the pasted nodes are a Blur and a Grade"):
                pbx, pby = p_blur.getPosition()
                pgx, pgy = p_grade.getPosition()
                want = (gx - bx, gy - by)
                got = (pgx - pbx, pgy - pby)
                check(close(got, want, POS_TOL),
                      "the pasted nodes keep their relative offset (got %r, want %r)" % (got, want))
        gt.shot("m16-layout-end.png")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
