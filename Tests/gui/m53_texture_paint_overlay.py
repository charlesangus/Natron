"""A TexturePaint painted in the Viewer's 3D view shows its unbaked strokes, a brush ring and the paint
toolbars, and the buffer goes once the bake lands.

Graph: USDRead(scene-reference.usda) into TexturePaint and into Viewer1, so the 3D view shows the scene,
with TexturePaint's settings panel open, which makes it the node the view paints into.
A plain left drag in the scene view paints a stroke through the view's camera. While the session is
open (unbaked) the view's natronScenePaintOverlayDrawn property is true and a screenshot shows the
buffer over the view, the brush ring at the cursor (natronScenePaintBrushRing: x, y, radius) and the
brush toolbars. The Bake Paint key (B) closes the session and queues its bake; once it lands the
overlay is no longer drawn.

Mouse events are sent to the SceneViewport (synthesized window-system events do not reach Natron's
widgets under Xvfb). Run through Tests/gui/run-gui-test.sh at 1920x1080. Screenshots and results.txt
go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
BAKE_TIMEOUT_S = 120.0
SETTLE_MS = 500
STARTUP_TIMEOUT_S = 60.0

try:
    import NatronEngine
    from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit

    from guitest import (FIXTURES, check, finish, plugin_problem, report, require, run, section, shot, viewer_up,
                         widgets_of)

    scene_path = os.path.join(FIXTURES, "scene-reference.usda")
    require(os.path.isfile(scene_path), "fixture exists: " + scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("filename").setValue(scene_path)
    paint = app.createNode("TexturePaint")
    require(paint is not None, "create TexturePaint", plugin_problem("TexturePaint"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(0, 0)
    paint.setPosition(0, 100)
    viewer.setPosition(0, 200)

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def node_graph_view():
        views = [w for w in widgets_of("NodeGraph") if w.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                    if w.text() == label:
                        return panel
            except RuntimeError:
                continue
        return None

    def mouse(v, kind, pos, button, buttons):
        QApplication.sendEvent(v, QMouseEvent(kind, QPointF(pos), QPointF(v.mapToGlobal(pos)), button, buttons,
                                              Qt.NoModifier))

    def stroke(v, start, delta):
        mouse(v, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, 11):
            mouse(v, QEvent.MouseMove, start + delta * (i / 10.), Qt.NoButton, Qt.LeftButton)
        mouse(v, QEvent.MouseButtonRelease, start + delta, Qt.LeftButton, Qt.NoButton)
        QApplication.processEvents()

    def overlay_drawn():
        v = view()
        return v is not None and bool(v.property("natronScenePaintOverlayDrawn"))

    def steps():
        require(paint.connectInput(0, usd), "connect TexturePaint to the USDRead")
        require(viewer.connectInput(0, usd), "connect the Viewer to the USDRead")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        graph = node_graph_view()
        require(graph is not None, "the node graph is on screen")
        app.selectNode(paint, True)
        QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(paint) is not None, POLL_TIMEOUT_S)
        require(ok, "open TexturePaint's settings panel")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the scene (%r)" % (v.property("natronSceneError") if v else None,))

        section("Unbaked buffer")
        v.activateWindow()
        v.setFocus()
        check(not overlay_drawn(), "no overlay before anything is painted")
        centre = v.rect().center()
        stroke(v, centre - QPoint(120, 40), QPoint(240, 80))
        ok = yield ("until", overlay_drawn, POLL_TIMEOUT_S)
        check(ok, "the unbaked buffer is drawn over the view while the session is open")
        mouse(v, QEvent.MouseMove, centre, Qt.NoButton, Qt.NoButton)
        v.update()
        yield ("sleep", SETTLE_MS)
        ring = v.property("natronScenePaintBrushRing")
        check(bool(ring) and len(ring) == 3 and ring[2] > 0, "a brush ring is drawn at the cursor (%r)" % (ring,))
        shot("m53-paint-overlay.png")

        section("Bake")
        QTest.keyClick(v, Qt.Key_B)
        QApplication.processEvents()
        ok = yield ("until", lambda: not overlay_drawn(), BAKE_TIMEOUT_S)
        check(ok, "the buffer is gone once the bake has landed")
        yield ("sleep", SETTLE_MS)
        shot("m53-paint-baked.png")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
