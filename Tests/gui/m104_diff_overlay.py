"""The node-graph comparison overlay and the Node Diff panel, in the real application.

A base project holds Blur1 -> Blur2 -> Blur3 and a lone Blur4, with Blur1's size at 3. After it is
saved, Blur1's size goes to 5, Blur2 is deleted without reconnecting, Blur3's size gets keys at
frames 1 and 10, and Blur5 is created. Comparing with the saved base tints Blur5 as added and
Blur1 and Blur3 as changed, leaves Blur4 untinted and draws one ghost, for Blur2. Selecting Blur1
lists its size change, selecting Blur3 its keys and its input, which lost Blur2. Clear Comparison
in the panel removes every tint and the ghost; so does clearing it from Python after comparing again.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m104_diff_overlay.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400
BLUR = "Blur"
SIZE = "size"
ARROW = " → "

try:
    from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton

    import guitest as gt
    from guitest import check, report, require

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and gt.class_name(w) == "Gui":
                return w
        return None

    def widget(cls, name):
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, cls) and w.objectName() == name:
                    return w
            except RuntimeError:
                continue
        return None

    def panel_shown():
        for w in gt.widgets_of("NodeDiffPanel"):
            return w.isVisible()
        return False

    def blur(name, x, y, source=None):
        node = app.createNode(BLUR)
        require(node is not None, "create " + name, gt.plugin_problem(BLUR))
        node.setScriptName(name)
        node.setPosition(x, y)
        if source is not None:
            require(node.connectInput(0, source), "connect %s to %s" % (source.getScriptName(), name))
        return node

    def tints(names):
        return dict((name, app.getDiffTint(app.getNode(name))) for name in names)

    def rows_for(node):
        """Selects node alone and returns the panel's rows once they list it."""
        app.selectNode(node, True)
        name = node.getScriptName()
        listed = lambda: name in [str(r) for r in app.getNodeDiffRows()]
        yield ("until", listed, POLL_TIMEOUT_S)

    def check_cleared(label, names):
        found = tints(names)
        check(all(t == "" for t in found.values()), "%s: no node is tinted (%r)" % (label, found))
        check(app.getDiffGhostCount() == 0, "%s: no ghost is drawn (got %d)" % (label, app.getDiffGhostCount()))

    def steps():
        gt.section("base")
        ok = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()

        base = os.path.join(gt.OUT, "m104_diff_base.ntpz")
        blur1 = blur("Blur1", 0, 0)
        blur2 = blur("Blur2", 0, 150, blur1)
        blur3 = blur("Blur3", 0, 300, blur2)
        blur("Blur4", 300, 0)
        blur1.getParam(SIZE).set(3.0, 3.0)
        input_label = str(blur3.getInputLabel(0))
        require(app.saveProjectAs(base), "save the base")

        gt.section("edits")
        blur1.getParam(SIZE).set(5.0, 5.0)
        blur2.destroy(False)
        yield ("sleep", SETTLE_MS)
        size3 = blur3.getParam(SIZE)
        for dim in range(2):
            size3.setValueAtTime(1.0, 1, dim)
            size3.setValueAtTime(6.0, 10, dim)
        blur("Blur5", 300, 300)
        yield ("sleep", SETTLE_MS)

        gt.section("compare")
        names = ["Blur1", "Blur3", "Blur4", "Blur5"]
        require(app.compareWithFile(base), "compare with the base")
        ok = yield ("until", lambda: app.getDiffGhostCount() > 0, POLL_TIMEOUT_S)
        check(ok, "the comparison draws its ghosts")
        found = tints(names)
        report("NOTE tints %r" % found)
        added = [n for n, t in found.items() if t == "added"]
        changed = [n for n, t in found.items() if t == "changed"]
        check(added == ["Blur5"], "one node is tinted as added, Blur5 (%r)" % added)
        check(sorted(changed) == ["Blur1", "Blur3"], "two nodes are tinted as changed, Blur1 and Blur3 (%r)" % changed)
        check(found["Blur4"] == "", "the untouched Blur4 is not tinted (%r)" % found["Blur4"])
        check(app.getDiffGhostCount() == 1, "one ghost is drawn, for Blur2 (got %d)" % app.getDiffGhostCount())
        ok = yield ("until", panel_shown, POLL_TIMEOUT_S)
        check(ok, "the Node Diff panel is shown by the comparison")
        gt.shot("m104_diff_overlay.png")

        gt.section("panel")
        yield from rows_for(app.getNode("Blur1"))
        rows = [str(r) for r in app.getNodeDiffRows()]
        report("NOTE Blur1 rows %r" % rows)
        check(rows == ["Blur1", "size: 3" + ARROW + "5"], "Blur1 lists its size change (%r)" % rows)

        yield from rows_for(app.getNode("Blur3"))
        rows = [str(r) for r in app.getNodeDiffRows()]
        report("NOTE Blur3 rows %r" % rows)
        check(rows[:1] == ["Blur3"], "Blur3's heading comes first (%r)" % rows)
        keys = [r for r in rows if r.startswith(SIZE) and "2 keys" in r and "added at 1, 10" in r]
        check(len(keys) > 0, "Blur3 lists its two new keys and their times (%r)" % rows)
        lost = [r for r in rows if r.startswith("Input") and r.endswith(": Blur2" + ARROW + "none")]
        check(len(lost) == 1, "Blur3 lists its input losing Blur2 (%r)" % rows)
        report("NOTE Blur3 input %r is labelled %r" % (lost, input_label))
        gt.shot("m104_diff_panel.png")

        gt.section("clear")
        clear = widget(QPushButton, "nodeDiffClear")
        require(clear is not None and clear.isEnabled(), "the Clear Comparison button is enabled")
        clear.click()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check_cleared("after Clear Comparison", names)
        check(len(app.getNodeDiffRows()) == 0, "the panel lists nothing after Clear Comparison")

        require(app.compareWithFile(base), "compare with the base again")
        ok = yield ("until", lambda: app.getDiffGhostCount() == 1, POLL_TIMEOUT_S)
        check(ok, "comparing again draws the ghost again")
        app.clearGraphComparison()
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        check_cleared("after clearing from Python", names)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
