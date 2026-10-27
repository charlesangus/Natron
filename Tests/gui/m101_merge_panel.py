"""The Merge properties panel: its knobs sit in the rows B Layers; B Alpha Source and B Alpha;
A Layers; A Alpha Source and A Alpha; Operation; Set BBox, Metadata From and Range From, top to
bottom, with a divider after each group. There is no Layer, Output, B Layer or Also Merge knob. B
Layers is set to the three layers of a
multi-layer Read connected to B, with rgba's row merging R, G and B alone: each row shows a button
per channel of its layer, and rgba's A button is off. The panel is saved to
$NATRON_GUI_TEST_OUT/shots/merge_panel.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m101_merge_panel.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
MERGE_ID = "Merge"

# Labels of one panel row lie within this many pixels of each other vertically.
ROW_TOLERANCE_PX = 8

ROWS = [
    ["B Layers"],
    ["B Alpha Source", "B Alpha"],
    ["A Layers"],
    ["A Alpha Source", "A Alpha"],
    ["Operation"],
    ["Set BBox", "Metadata From", "Range From"],
]

try:
    from PySide6.QtCore import QMetaObject, QRect
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QPushButton, QScrollArea

    import guitest as gt
    from guitest import check, report, require

    SHOT_DIR = os.path.join(gt.OUT, "shots")
    os.makedirs(SHOT_DIR, exist_ok=True)

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def label_rows(panel):
        """The vertical centre of every visible knob label in panel, keyed by its text without a
        trailing colon; the first of equal texts wins, the panel listing knobs top to bottom."""
        centres = {}
        for w in panel.findChildren(QLabel):
            try:
                if not w.isVisible():
                    continue
                text = w.text().strip().rstrip(":").strip()
                if text and text not in centres:
                    centres[text] = w.mapTo(panel, w.rect().center()).y()
            except RuntimeError:
                continue
        return centres

    def enlarge(panel):
        """Grows the panel's scroll area to its content so the whole knob list is on screen."""
        for area in panel.findChildren(QScrollArea):
            content = area.widget()
            if content is not None:
                area.setMinimumHeight(content.sizeHint().height() + 4)
        QApplication.processEvents()

    def screen_crop(name, panel):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(panel.mapToGlobal(panel.rect().topLeft()), panel.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(SHOT_DIR, name + ".png")
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def check_rows(panel):
        centres = label_rows(panel)
        report("NOTE label rows: %r" % sorted(centres.items(), key=lambda item: item[1]))
        row_ys = []
        for row in ROWS:
            ys = []
            for label in row:
                if check(label in centres, "the panel shows a %s knob" % label):
                    ys.append(centres[label])
            if not ys:
                row_ys.append(None)
                continue
            check(max(ys) - min(ys) <= ROW_TOLERANCE_PX, "%s share one row (y %r)" % (", ".join(row), ys))
            row_ys.append(sum(ys) / len(ys))
        known = [y for y in row_ys if y is not None]
        check(known == sorted(known) and len(set(known)) == len(known),
              "the rows run top to bottom in order (y %r)" % row_ys)
        for gone in ("Layer", "Output", "B Layer", "Also Merge"):
            check(gone not in centres, "the panel has no %s knob" % gone)

    # Set by LayerChannelRow on each channel button to the channel's name.
    CHANNEL_NAME_PROPERTY = "channelName"

    def channel_buttons(panel):
        """The visible channel buttons of panel's layer rows, as (tooltip, name, checked)."""
        found = []
        for b in panel.findChildren(QPushButton):
            try:
                name = b.property(CHANNEL_NAME_PROPERTY)
                if name and b.isVisible():
                    found.append((b.toolTip(), str(name), b.isChecked()))
            except RuntimeError:
                continue
        return found

    def steps():
        gt.section("merge panel")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        view = node_graph_view()
        gt.close_all_panels()

        path = os.path.join(gt.FIXTURES, "flat-three-layers.exr")
        require(os.path.isfile(path), "fixture exists: " + os.path.basename(path))
        read = app.createReader(path)
        require(read is not None, "create a Read for " + os.path.basename(path))
        read.setPosition(0, 0)

        merge = app.createNode(MERGE_ID)
        require(merge is not None, "create Merge", gt.plugin_problem(MERGE_ID))
        merge.setPosition(0, 200)
        require(merge.connectInput(0, read), "connect the Read to Merge's B")

        for name in ("layer", "output", "bLayer", "alsoMerge", "bbox"):
            check(merge.getParam(name) is None, "Merge has no %s param" % name)
        check(merge.getParam("setBBox") is not None, "Merge has a setBBox param")
        layers = merge.getParam("bLayers")
        require(layers is not None, "Merge has a layers param")

        app.selectNode(merge, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(merge) is not None, POLL_TIMEOUT_S)
        require(ok, "open Merge's settings panel")
        yield ("sleep", 800)
        panel = panel_of(merge)
        enlarge(panel)
        yield ("sleep", 500)
        check_rows(panel)

        layers.setLayer("rgba")
        layers.addLayer("diffuse")
        layers.addLayer("specular")
        layers.setChannels(["R", "G", "B"], 0)
        yield ("sleep", 800)
        rows = layers.getRows()
        report("NOTE Layers rows: %r" % (rows,))
        check(bool(rows) and list(rows[0][2]) == ["R", "G", "B"], "rgba's row merges R, G and B alone")
        summary = layers.getSummary()
        report("NOTE Layers summary: %r" % summary)
        for layer in ("diffuse", "specular"):
            check(layer in summary, "Layers lists %s" % layer)
        panel = panel_of(merge)
        require(panel is not None, "Merge's settings panel is still open")
        enlarge(panel)
        yield ("sleep", 500)
        check_rows(panel)
        buttons = channel_buttons(panel)
        report("NOTE channel buttons: %r" % buttons)
        rgba = [(name, checked) for tip, name, checked in buttons if tip.lower().startswith("rgba.")]
        check(sorted(name for name, _ in rgba) == ["A", "B", "G", "R"],
              "rgba's row shows R, G, B and A buttons (got %r)" % rgba)
        check(("A", False) in rgba and all(checked for name, checked in rgba if name != "A"),
              "rgba's A button is off and its R, G and B buttons on")
        check(len(buttons) > len(rgba), "the diffuse and specular rows show their own channel buttons")
        screen_crop("merge_panel", panel)

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
