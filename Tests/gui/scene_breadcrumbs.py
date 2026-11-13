"""The breadcrumb bar above the 3D view.

Graph: USDRead(a generated hierarchy) -> USDPrune -> Viewer1. The hierarchy has
/geo/group_1/a_thing/mesh, /geo/group_1/b_thing, /geo/group_2, a path of long names, /big with
10000 children and /pay, a prim whose payload is not loaded. b_thing and group_2's cube sit apart
from the mesh, so the scene, group_1 and a_thing each frame differently. The prune starts out removing
nothing; the script then makes it remove the mesh.

Checks:
  - the bar sits above the scene view and shows only "/" with nothing selected;
  - selecting /geo/group_1/a_thing/mesh reads geo, group_1, a_thing, mesh, all present, and the
    HUD text no longer has the path;
  - a click on group_1 selects /geo/group_1, a Shift+click on group_1 adds it, the "/" button clears;
  - a "+1" badge shows when two prims are selected;
  - the chevron on group_1 lists its children with the scene graph's icons; a right-click on group_1
    opens a menu of Siblings, Copy Path, Copy Selected Paths and Frame, and Siblings lists group_1 and
    group_2 with group_1 marked, and a click on the group_2 row selects /geo/group_2;
  - Copy Path puts /geo/group_1 on the clipboard, Copy Selected Paths with two prims selected puts both
    lines there, and a double-click on group_1 selects it and moves the free camera (framing);
  - a hover on group_1 shows a tooltip naming its type, kind and loaded state;
  - the filter narrows /big's 10000 children to one row, with the main thread within the stall
    budget while the list is read and the filter is typed;
  - the chevron on /pay shows a "Not loaded" row whose Load button fills the list;
  - pruning the mesh greys it while its ancestors stay clickable, a click on a greyed segment
    changes nothing, its tooltip says "Not in this scene" and its menu still copies its path, with
    Siblings and Frame disabled;
  - split to half the screen's width, a path of long names folds its middle segments into a "..."
    button whose menu lists them, first and last stay, and an entry of that menu selects its prim.

Run through Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 120.0
SETTLE_MS = 400
MESH = "/geo/group_1/a_thing/mesh"
GROUP = "/geo/group_1"
SIBLING = "/geo/group_2"
BIG = "/big"
BIG_COUNT = 10000
PAYLOAD = "/pay"
PAYLOAD_CHILDREN = ["Floor", "Roof"]
HEARTBEAT_MS = 10
HEARTBEAT_BUDGET_MS = 100.0
HEARTBEAT_BLOCKED_BUDGET_MS = 400.0
LONG_NAMES = ["a_long_segment_name_%d_for_the_bar" % i for i in range(1, 7)] + ["leaf"]
LONG_PATH = "/" + "/".join(LONG_NAMES)


def translate(x, y, z, indent):
    return ('%sdouble3 xformOp:translate = (%g, %g, %g)\n%suniform token[] xformOpOrder = ["xformOp:translate"]\n'
            % (indent, x, y, z, indent))


def write_scene(directory):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "pay_payload.usda"), "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Root"\n)\ndef Xform "Root"\n{\n')
        for name in PAYLOAD_CHILDREN:
            f.write('    def Xform "%s"\n    {\n    }\n' % name)
        f.write('}\n')
    path = os.path.join(directory, "breadcrumbs.usda")
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "geo"\n    upAxis = "Y"\n)\n')
        f.write('def Xform "geo"\n{\n')
        f.write('    def Xform "group_1"\n    {\n')
        f.write('        def Xform "a_thing"\n        {\n            def Cube "mesh"\n            {\n                double size = 1\n            }\n        }\n')
        f.write('        def Cube "b_thing"\n        {\n            double size = 1\n%s        }\n' % translate(4, 0, 0, "            "))
        f.write('    }\n')
        f.write('    def Xform "group_2"\n    {\n        def Cube "c_thing"\n        {\n            double size = 1\n%s        }\n    }\n' % translate(0, 0, -8, "            "))
        f.write('}\n')
        f.write('def Scope "big"\n{\n')
        for i in range(BIG_COUNT):
            f.write('    def Xform "child_%05d"\n    {\n    }\n' % i)
        f.write('}\n')
        f.write('def Xform "pay" (\n    prepend payload = @./pay_payload.usda@\n)\n{\n}\n')
        f.write('def Xform "%s"\n{\n' % LONG_NAMES[0])
        indent = "    "
        for name in LONG_NAMES[1:-1]:
            f.write('%sdef Xform "%s"\n%s{\n' % (indent, name, indent))
            indent += "    "
        f.write('%sdef Cube "leaf"\n%s{\n%s    double size = 1\n%s}\n' % (indent, indent, indent, indent))
        for _ in LONG_NAMES[1:-1]:
            indent = indent[:-4]
            f.write('%s}\n' % indent)
        f.write('}\n')
    return path


try:
    import NatronEngine
    from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, Qt, QTimer
    from PySide6.QtGui import QHelpEvent, QIcon
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLineEdit, QListView, QMenu, QPushButton, QToolButton, QToolTip

    from guitest import OUT, check, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    scene_path = write_scene(os.path.realpath(os.path.join(OUT, "scene_breadcrumbs")))

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    prune = app.createNode("USDPrune")
    require(prune is not None, "create USDPrune", plugin_problem("USDPrune"))
    prune.getParam("primPattern").setValue("/nothing")
    require(prune.connectInput(0, usd), "connect the USDPrune to the USDRead")
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.disconnectInput(0)
    require(viewer.connectInput(0, prune), "connect the Viewer to the USDPrune")
    usd.setPosition(0, 0)
    prune.setPosition(0, 100)
    viewer.setPosition(0, 200)

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    def bar():
        bars = [w for w in widgets_of("SceneBreadcrumbBar") if w.isVisible()]
        return hold(bars[0]) if bars else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def selection():
        v = view()
        return [str(p) for p in (v.property("natronSceneSelection") or [])] if v is not None else []

    def select(paths):
        QMetaObject.invokeMethod(view(), "selectPaths", Qt.DirectConnection, QStringList_arg(paths))

    def segments():
        b = bar()
        return [dict(s) for s in (b.property("natronBreadcrumbSegments") or [])] if b is not None else []

    def texts(visible_only=False):
        return [str(s["text"]) for s in segments() if not (visible_only and s["elided"])]

    def states():
        return [str(s["state"]) for s in segments()]

    def buttons(name):
        b = bar()
        return [hold(w) for w in b.findChildren(QToolButton, name) if w.isVisibleTo(b)] if b is not None else []

    def segment_button(text):
        for w in buttons("breadcrumbSegment"):
            if str(w.text()) == text:
                return w
        return None

    def click(button, modifier=Qt.NoModifier):
        QTest.mouseClick(button, Qt.LeftButton, modifier)

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def QStringList_arg(paths):
        return Q_ARG("QStringList", list(paths))

    def popup():
        ps = [w for w in widgets_of("ScenePrimListPopup") if w.isVisible()]
        return hold(ps[0]) if ps else None

    def popup_rows(p):
        return [str(r) for r in (p.property("natronPopupRows") or [])]

    def segment_menu():
        b = bar()
        ms = [w for w in b.findChildren(QMenu, "breadcrumbSegmentMenu") if w.isVisible()] if b is not None else []
        return hold(ms[0]) if ms else None

    def menu_action(name):
        m = segment_menu()
        for a in (m.actions() if m is not None else []):
            if a.objectName() == name:
                return a
        return None

    def choose(name):
        """Clicks the menu's entry, as a user does: QAction.trigger() would leave the menu open."""
        m = segment_menu()
        QTest.mouseClick(m, Qt.LeftButton, Qt.NoModifier, m.actionGeometry(menu_action(name)).center())

    def camera():
        v = view()
        value = v.property("natronSceneFreeCamera") if v is not None else None
        return [float(x) for x in value] if value else None

    def tooltip_of(button):
        QToolTip.hideText()
        QApplication.sendEvent(button, QHelpEvent(QEvent.ToolTip, button.rect().center(), button.mapToGlobal(button.rect().center())))
        return str(QToolTip.text())

    def chevron(path):
        b = bar()
        for w in b.findChildren(QToolButton, "breadcrumbChevron"):
            if str(w.property("natronBreadcrumbPath")) == path and w.isVisibleTo(b):
                return hold(w)
        return None

    def main_thread_times_ms():
        with open("/proc/thread-self/schedstat") as f:
            fields = f.read().split()
        return int(fields[0]) / 1000000.0, int(fields[1]) / 1000000.0

    class Heartbeat:
        """The longest the main thread's event loop has gone without running a short timer: the most
        CPU one gap used, and the gap's length with the time spent ready to run without a CPU left out."""

        def __init__(self):
            self.timer = QTimer()
            self.timer.setInterval(HEARTBEAT_MS)
            self.timer.timeout.connect(self.beat)
            self.reset()

        def reset(self):
            self.last = time.monotonic()
            self.cpu, self.waited = main_thread_times_ms()
            self.worst_cpu_ms = 0.0
            self.worst_ms = 0.0

        def beat(self):
            now = time.monotonic()
            cpu, waited = main_thread_times_ms()
            self.worst_ms = max(self.worst_ms, (now - self.last) * 1000.0 - (waited - self.waited))
            self.worst_cpu_ms = max(self.worst_cpu_ms, cpu - self.cpu)
            self.last, self.cpu, self.waited = now, cpu, waited

        def start(self):
            self.reset()
            self.timer.start()

        def stop(self):
            self.timer.stop()

    def check_heartbeat(beat, what):
        report("NOTE worst main-thread gap %s: %.1f ms on a CPU, %.1f ms with blocking" % (what, beat.worst_cpu_ms, beat.worst_ms))
        check(beat.worst_cpu_ms <= HEARTBEAT_BUDGET_MS, "%s leaves the main thread idle (%.1f ms)" % (what, beat.worst_cpu_ms))
        check(beat.worst_ms <= HEARTBEAT_BLOCKED_BUDGET_MS, "%s does not block the main thread (%.1f ms)" % (what, beat.worst_ms))

    def steps():
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        require(ok, "the scene view shows the scene")
        ok = yield ("until", lambda: bar() is not None, POLL_TIMEOUT_S)
        require(ok, "the breadcrumb bar is visible with the scene view")
        v, b = view(), bar()
        check(b.mapTo(b.window(), b.rect().topLeft()).y() < v.mapTo(v.window(), v.rect().topLeft()).y(),
              "the bar is above the scene view")

        check(texts() == [] and len(buttons("breadcrumbRoot")) == 1, "nothing selected: only the \"/\" button")
        height = b.height()

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"], POLL_TIMEOUT_S)
        check(ok, "the mesh's path reads geo, group_1, a_thing, mesh (got %r)" % (texts(),))
        ok = yield ("until", lambda: states() == ["present"] * 4, POLL_TIMEOUT_S)
        check(ok, "every segment is present (got %r)" % (states(),))
        check(b.height() == height, "the bar keeps its height with a selection")
        hud = str(v.property("natronSceneHud") or "")
        check(bool(hud) and "group_1" not in hud and MESH not in hud, "the HUD has no path (got %r)" % (hud,))
        shot("scene_breadcrumbs_mesh.png")

        button = segment_button("group_1")
        require(button is not None and button.isEnabled(), "the group_1 button is clickable")
        click(button)
        ok = yield ("until", lambda: selection() == [GROUP], POLL_TIMEOUT_S)
        check(ok, "a click on group_1 selects /geo/group_1 (got %r)" % (selection(),))
        ok = yield ("until", lambda: texts() == ["geo", "group_1"], POLL_TIMEOUT_S)
        check(ok, "the bar follows the selection (got %r)" % (texts(),))

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"], POLL_TIMEOUT_S)
        click(segment_button("group_1"), Qt.ShiftModifier)
        ok = yield ("until", lambda: selection() == [MESH, GROUP], POLL_TIMEOUT_S)
        check(ok, "a Shift+click on group_1 adds it to the selection (got %r)" % (selection(),))
        ok = yield ("until", lambda: int(bar().property("natronBreadcrumbMore") or 0) == 1 and len(buttons("breadcrumbMore")) == 1,
                    POLL_TIMEOUT_S)
        check(ok, "a +1 badge shows for two selected prims")
        check(texts() == ["geo", "group_1"], "the bar shows the current (last) prim's path (got %r)" % (texts(),))

        click(buttons("breadcrumbRoot")[0])
        ok = yield ("until", lambda: selection() == [], POLL_TIMEOUT_S)
        check(ok, "the \"/\" button clears the selection (got %r)" % (selection(),))
        check(texts() == [] and not buttons("breadcrumbMore"), "an empty selection leaves only \"/\"")
        check(b.height() == height, "the bar keeps its height with no selection")

        select([GROUP])
        ok = yield ("until", lambda: texts() == ["geo", "group_1"] and chevron(GROUP) is not None, POLL_TIMEOUT_S)
        require(ok, "the bar shows geo, group_1 with a chevron after group_1")
        click(chevron(GROUP))
        ok = yield ("until", lambda: popup() is not None and bool(popup().property("natronPopupComplete")), POLL_TIMEOUT_S)
        require(ok, "the chevron on group_1 opens a popup that finishes reading")
        p = popup()
        check(str(p.property("natronPopupParent")) == GROUP, "the popup lists group_1's children (got %r)" % (p.property("natronPopupParent"),))
        check(popup_rows(p) == ["a_thing", "b_thing"], "the children are a_thing and b_thing (got %r)" % (popup_rows(p),))
        view_list = hold(p.findChild(QListView, "scenePrimListView"))
        icon = view_list.model().index(0, 0).data(Qt.DecorationRole)
        check(isinstance(icon, QIcon) and not icon.isNull(), "the Xform row has the scene graph's icon for its type")
        check(not p.findChild(QPushButton, "scenePrimListLoad").isVisibleTo(p), "a loaded prim has no Load row")
        yield ("sleep", SETTLE_MS)
        shot("scene_breadcrumbs_children.png")
        p.close()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "the popup closes")

        QTest.mouseClick(segment_button("group_1"), Qt.RightButton)
        ok = yield ("until", lambda: segment_menu() is not None, POLL_TIMEOUT_S)
        require(ok, "a right-click on group_1 opens a menu")
        check(popup() is None, "the right-click opens the menu, not the siblings popup")
        labels = [str(a.text()) for a in segment_menu().actions() if not a.isSeparator()]
        check(labels == ["Siblings", "Copy Path", "Copy Selected Paths", "Frame", "Show In Scene Graph"], "the menu lists its five entries (got %r)" % (labels,))
        check(all(menu_action(n).isEnabled() for n in ("breadcrumbSiblings", "breadcrumbCopyPath", "breadcrumbCopySelectedPaths", "breadcrumbFrame")),
              "every entry of a present segment's menu is enabled")
        shot("scene_breadcrumbs_menu.png")
        choose("breadcrumbSiblings")
        ok = yield ("until", lambda: popup() is not None and bool(popup().property("natronPopupComplete")), POLL_TIMEOUT_S)
        require(ok, "Siblings opens a popup")
        p = popup()
        check(str(p.property("natronPopupParent")) == "/geo", "the popup lists geo's children (got %r)" % (p.property("natronPopupParent"),))
        check(popup_rows(p) == ["group_1", "group_2"], "the siblings are group_1 and group_2 (got %r)" % (popup_rows(p),))
        check(str(p.property("natronPopupCurrent")) == "group_1", "group_1 is marked (got %r)" % (p.property("natronPopupCurrent"),))
        check(selection() == [GROUP], "opening the popup leaves the selection (got %r)" % (selection(),))
        yield ("sleep", SETTLE_MS)
        shot("scene_breadcrumbs_siblings.png")
        siblings_view = hold(p.findChild(QListView, "scenePrimListView"))
        sibling_rect = siblings_view.visualRect(siblings_view.model().index(1, 0))
        QTest.mouseClick(siblings_view.viewport(), Qt.LeftButton, Qt.NoModifier, sibling_rect.center())
        ok = yield ("until", lambda: selection() == [SIBLING], POLL_TIMEOUT_S)
        check(ok, "a click on the group_2 row selects /geo/group_2 (got %r)" % (selection(),))
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)
        check(ok, "a pick closes the popup")

        clipboard = QApplication.clipboard()
        select([GROUP])
        ok = yield ("until", lambda: texts() == ["geo", "group_1"] and states() == ["present"] * 2, POLL_TIMEOUT_S)
        require(ok, "the bar shows geo, group_1")
        clipboard.setText("")
        QTest.mouseClick(segment_button("group_1"), Qt.RightButton)
        ok = yield ("until", lambda: menu_action("breadcrumbCopyPath") is not None, POLL_TIMEOUT_S)
        require(ok, "the menu opens on group_1")
        choose("breadcrumbCopyPath")
        check(str(clipboard.text()) == GROUP, "Copy Path puts /geo/group_1 on the clipboard (got %r)" % (clipboard.text(),))
        ok = yield ("until", lambda: segment_menu() is None, POLL_TIMEOUT_S)
        check(ok, "choosing an entry closes the menu")

        select([MESH, GROUP])
        ok = yield ("until", lambda: selection() == [MESH, GROUP] and texts() == ["geo", "group_1"], POLL_TIMEOUT_S)
        require(ok, "two prims are selected and the bar shows the last (got %r)" % (selection(),))
        clipboard.setText("")
        QTest.mouseClick(segment_button("group_1"), Qt.RightButton)
        ok = yield ("until", lambda: menu_action("breadcrumbCopySelectedPaths") is not None, POLL_TIMEOUT_S)
        require(ok, "the menu opens on group_1 with two selected")
        choose("breadcrumbCopySelectedPaths")
        check(str(clipboard.text()).split("\n") == [MESH, GROUP], "Copy Selected Paths puts both paths on the clipboard, one per line (got %r)" % (clipboard.text(),))
        ok = yield ("until", lambda: segment_menu() is None, POLL_TIMEOUT_S)

        tip = tooltip_of(segment_button("group_1"))
        check("Xform" in tip and GROUP in tip, "the tooltip of group_1 names its type (got %r)" % (tip,))
        check("Kind:" in tip and "Loaded:" in tip, "the tooltip tells the kind and the loaded state (got %r)" % (tip,))
        QToolTip.hideText()

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"] and states() == ["present"] * 4, POLL_TIMEOUT_S)
        require(ok, "the bar shows the mesh's path again")
        v.update()
        yield ("sleep", SETTLE_MS)
        before = camera()
        require(before is not None, "the free camera is published")
        QTest.mouseDClick(segment_button("group_1"), Qt.LeftButton)
        ok = yield ("until", lambda: selection() == [GROUP], POLL_TIMEOUT_S)
        check(ok, "a double-click on group_1 selects /geo/group_1 (got %r)" % (selection(),))

        def moved():
            v.update()
            now = camera()
            return now is not None and any(abs(a - b) > 1e-6 * max(1.0, abs(a)) for a, b in zip(now, before))

        ok = yield ("until", moved, POLL_TIMEOUT_S)
        check(ok, "a double-click on group_1 frames it: the camera moved from %r to %r" % (before, camera()))

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"] and segment_button("group_1") is not None, POLL_TIMEOUT_S)
        v.update()
        yield ("sleep", SETTLE_MS)
        before = camera()
        QTest.mouseClick(segment_button("a_thing"), Qt.RightButton)
        ok = yield ("until", lambda: menu_action("breadcrumbFrame") is not None, POLL_TIMEOUT_S)
        require(ok, "the menu opens on a_thing")
        choose("breadcrumbFrame")
        ok = yield ("until", lambda: selection() == ["/geo/group_1/a_thing"], POLL_TIMEOUT_S)
        check(ok, "Frame selects the prim (got %r)" % (selection(),))
        ok = yield ("until", moved, POLL_TIMEOUT_S)
        check(ok, "Frame moves the camera")

        select([BIG])
        ok = yield ("until", lambda: texts() == ["big"] and chevron(BIG) is not None, POLL_TIMEOUT_S)
        require(ok, "the bar shows big with a chevron")
        click(chevron(BIG))
        ok = yield ("until", lambda: popup() is not None and int(popup().property("natronPopupLoadedCount") or 0) > 0, POLL_TIMEOUT_S)
        p = popup()
        require(ok, "the chevron on big opens a popup with rows")
        # Looking a widget up wraps every widget in the application, which would be what the heartbeat
        # measures, so the popup's widgets are found before it starts and polled from here on.
        beat = Heartbeat()
        beat.start()
        field = hold(p.findChild(QLineEdit, "scenePrimListFilter"))
        check(int(p.property("natronPopupLoadedCount")) < BIG_COUNT, "the list reads a batch, not all %d children, to open" % (BIG_COUNT,))
        target = "child_%05d" % (BIG_COUNT - 1)
        QTest.keyClicks(field, target)
        ok = yield ("until", lambda: popup_rows(p) == [target] and bool(p.property("natronPopupComplete")), LOAD_TIMEOUT_S)
        beat.stop()
        check(ok, "the filter narrows %d children to %s (got %r of %r)" % (BIG_COUNT, target, popup_rows(p)[:3], p.property("natronPopupRowCount")))
        check(int(p.property("natronPopupLoadedCount")) == BIG_COUNT, "every child is listed, with no cap (got %r)" % (p.property("natronPopupLoadedCount"),))
        check_heartbeat(beat, "opening and filtering a %d-child list" % (BIG_COUNT,))
        shot("scene_breadcrumbs_filtered.png")
        p.close()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)

        select([PAYLOAD])
        ok = yield ("until", lambda: texts() == ["pay"] and chevron(PAYLOAD) is not None, POLL_TIMEOUT_S)
        require(ok, "the bar shows pay with a chevron")
        click(chevron(PAYLOAD))
        ok = yield ("until", lambda: popup() is not None and bool(popup().property("natronPopupNotLoaded")), POLL_TIMEOUT_S)
        require(ok, "the popup over an unloaded payload offers a Not loaded row")
        p = popup()
        load_button = hold(p.findChild(QPushButton, "scenePrimListLoad"))
        check(load_button.isVisibleTo(p) and load_button.isEnabled() and popup_rows(p) == [], "the Load button is shown over an empty list (got %r)" % (popup_rows(p),))
        yield ("sleep", SETTLE_MS)
        shot("scene_breadcrumbs_not_loaded.png")
        click(load_button)
        ok = yield ("until", lambda: popup_rows(p) == PAYLOAD_CHILDREN and not bool(p.property("natronPopupNotLoaded")), LOAD_TIMEOUT_S)
        check(ok, "Load fills the list with the payload's prims (got %r)" % (popup_rows(p),))
        check(not load_button.isVisibleTo(p), "the Not loaded row goes once the payload is loaded")
        shot("scene_breadcrumbs_loaded.png")
        p.close()
        ok = yield ("until", lambda: popup() is None, POLL_TIMEOUT_S)

        select([MESH])
        ok = yield ("until", lambda: texts() == ["geo", "group_1", "a_thing", "mesh"], POLL_TIMEOUT_S)
        require(ok, "the bar shows the mesh's path before the prune (got %r, selection %r)" % (texts(), selection()))
        prune.getParam("primPattern").setValue(MESH)
        ok = yield ("until", lambda: states() == ["present", "present", "present", "missing"], POLL_TIMEOUT_S)
        check(ok, "pruning the mesh greys the mesh alone (got %r for %r, selection %r)" % (states(), texts(), selection()))
        yield ("sleep", SETTLE_MS)
        mesh_button = segment_button("mesh")
        require(mesh_button is not None and str(mesh_button.property("natronBreadcrumbState")) == "missing", "the greyed segment is still shown")
        ancestor = segment_button("a_thing")
        check(ancestor is not None and ancestor.isEnabled(), "the ancestors stay clickable")
        shot("scene_breadcrumbs_pruned.png")
        QTest.mouseClick(mesh_button, Qt.LeftButton)
        yield ("sleep", SETTLE_MS)
        check(selection() == [MESH], "a click on the greyed segment changes nothing (got %r)" % (selection(),))
        tip = tooltip_of(mesh_button)
        check(tip == "Not in this scene", "the missing segment's tooltip says it is not in this scene (got %r)" % (tip,))
        QToolTip.hideText()
        clipboard.setText("")
        QTest.mouseClick(mesh_button, Qt.RightButton)
        ok = yield ("until", lambda: menu_action("breadcrumbCopyPath") is not None, POLL_TIMEOUT_S)
        require(ok, "a right-click on the missing segment opens its menu")
        check(not menu_action("breadcrumbSiblings").isEnabled() and not menu_action("breadcrumbFrame").isEnabled(),
              "Siblings and Frame are disabled for a missing segment")
        choose("breadcrumbCopyPath")
        check(str(clipboard.text()) == MESH, "Copy Path works on the missing segment (got %r)" % (clipboard.text(),))
        ok = yield ("until", lambda: segment_menu() is None, POLL_TIMEOUT_S)
        check(selection() == [MESH], "the missing segment's menu leaves the selection (got %r)" % (selection(),))

        prune.getParam("primPattern").setValue("/nothing")
        select([])
        check(not buttons("breadcrumbElision"), "a short path in a wide bar has no \"...\" button")
        home = ancestor_of_class(v, "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        split = None
        yield ("sleep", SETTLE_MS)
        select([LONG_PATH])
        ok = yield ("until", lambda: texts() == LONG_NAMES, POLL_TIMEOUT_S)
        check(ok, "the long path reads in full in the model (got %r)" % (texts(),))
        ok = yield ("until", lambda: bool(bar().property("natronBreadcrumbElided")) and len(buttons("breadcrumbElision")) == 1,
                    POLL_TIMEOUT_S)
        check(ok, "the narrow bar folds the middle into a \"...\" button")
        shown = texts(True)
        check(shown and shown[0] == LONG_NAMES[0] and shown[-1] == LONG_NAMES[-1] and len(shown) < len(LONG_NAMES),
              "the first and last segments stay (got %r)" % (shown,))
        check(segment_button(LONG_NAMES[0]) is not None and segment_button(LONG_NAMES[-1]) is not None,
              "the first and last buttons are shown")
        elision = buttons("breadcrumbElision")[0]
        menu = elision.menu()
        folded = [str(s["text"]) for s in segments() if s["elided"]]
        check(menu is not None and [str(a.text()) for a in menu.actions()] == folded and len(folded) > 0,
              "the \"...\" menu lists the folded segments (got %r)" % (folded,))
        shot("scene_breadcrumbs_elided.png")
        folded_paths = [str(s["path"]) for s in segments() if s["elided"]]
        if menu is not None and folded_paths:
            menu.actions()[0].trigger()
            ok = yield ("until", lambda: selection() == [folded_paths[0]], POLL_TIMEOUT_S)
            check(ok, "the \"...\" menu's first entry selects %s (got %r)" % (folded_paths[0], selection()))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
