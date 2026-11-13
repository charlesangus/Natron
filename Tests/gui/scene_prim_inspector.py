"""The Scene Graph panel's prim inspector: its tabs, who authored each row, and that reading a prim
leaves the main thread free.

Graph: USDCamera into USDEditTransform into Viewer1, and a USDRead of a scene the script writes to
$NATRON_GUI_TEST_OUT: a small prim with authored values, relationships and metadata, a flat Scope
with FLAT_PRIMS children, a prim with HEAVY_ATTRIBUTES large arrays, and an instance.

Checks:
  - on the camera, focalLength names the USDCamera, and the transform op and op order name the
    USDEditTransform;
  - on the file, the Source column tells a default value, time samples and a schema fallback apart,
    and file opinions name the file;
  - a value at another frame reads at that frame;
  - the Relationships tab lists targets, and the Metadata tab lists fields and authors;
  - a long value is cut in the table with all of it in the tooltip;
  - an instance shows its prototype;
  - the Composition tab lists the camera's root arc;
  - the Opinions tab lists the specs behind the camera prim and the selected focal length, strongest
    first, and double-clicking a row naming the USDEditTransform opens that node's settings panel;
  - the Layer tab shows the viewed USDEditTransform's layer as USDA with its authored over, also with
    no prim selected, and switching it to the composed mode shows the selected prim's definition,
    which follows the selection;
  - a new prim leaves no attribute row selected;
  - frame changes run no new composition query or definition read, and a new prim runs one each;
  - a frame change leaves a read under way running, and a tab that varies with the frame then reads
    the latest frame;
  - a large array's tooltip stays short and a double-click shows all of it in a window;
  - selecting a prim, and showing a large definition as text, keep the main thread's heartbeat within
    HEARTBEAT_BUDGET_MS of CPU and HEARTBEAT_BLOCKED_BUDGET_MS with blocking, while a slow
    read shows the last result greyed, and a new selection cancels the read under way.

Screenshots go to $NATRON_GUI_TEST_OUT (default build/gui-test-out/) for sign-off. Run through
Tests/gui/run-gui-test.sh. Exit status 0 only if every check passed.
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
HEARTBEAT_MS = 10
HEARTBEAT_BUDGET_MS = 100.0
# Software GL holds the main thread 75-170 ms on any repaint, with no read at all, waiting for the
# rasteriser to flush. Waiting on a lock a read holds lasts as long as the read, which is seconds.
HEARTBEAT_BLOCKED_BUDGET_MS = 400.0
FLAT_PRIMS = 50000
HEAVY_ATTRIBUTES = 200
HEAVY_ELEMENTS = 10000
LONG_VALUE_LENGTH = 600
MAX_CELL_LENGTH = 200
MAX_TIP_LENGTH = 2000

ROOT = "/Inspect"
SMALL = ROOT + "/Small"
OTHER = ROOT + "/Other"
PROTO = ROOT + "/Proto"
FLAT = ROOT + "/Flat"
HEAVY = ROOT + "/Heavy"
INSTANCE = ROOT + "/Inst"

ATTRIBUTES = "sceneGraphAttributes"
RELATIONSHIPS = "sceneGraphRelationships"
METADATA = "sceneGraphMetadata"
COMPOSITION = "sceneGraphComposition"
OPINIONS = "sceneGraphOpinions"
LAYER_MODE = "sceneGraphLayerMode"
LAYER_TEXT = "sceneGraphLayerText"
COMPOSITION_TAB = 3
OPINIONS_TAB = 4
LAYER_TAB = 5

LONG_VALUE = "".join(chr(ord("a") + n % 26) for n in range(LONG_VALUE_LENGTH))


def write_scene(path):
    heavy_value = "[" + ", ".join(["1.25"] * HEAVY_ELEMENTS) + "]"
    with open(path, "w") as f:
        f.write('#usda 1.0\n(\n    defaultPrim = "Inspect"\n    timeCodesPerSecond = 24\n)\n\n')
        f.write('def Xform "Inspect"\n{\n')
        f.write('    def Scope "Small" (\n        kind = "component"\n        doc = "A prim to inspect"\n'
                '        customData = {\n            string note = "hello"\n        }\n    )\n    {\n')
        f.write('        custom double size = 3\n')
        f.write('        custom double animated.timeSamples = {\n            1: 10,\n            20: 200,\n        }\n')
        f.write('        custom string longValue = "%s"\n' % LONG_VALUE)
        f.write('        custom rel links = [</Inspect/Other>, </Inspect/Flat>]\n')
        f.write('    }\n\n')
        f.write('    def Scope "Other"\n    {\n        custom double otherValue = 1\n    }\n\n')
        f.write('    def Scope "Proto"\n    {\n        def Scope "Child"\n        {\n        }\n    }\n\n')
        f.write('    def Scope "Inst" (\n        instanceable = true\n        references = </Inspect/Proto>\n    )\n    {\n    }\n\n')
        f.write('    def Scope "Heavy"\n    {\n')
        for n in range(HEAVY_ATTRIBUTES):
            f.write('        custom float[] heavy_%d = %s\n' % (n, heavy_value))
        f.write('    }\n\n')
        f.write('    def Scope "Flat"\n    {\n')
        for n in range(FLAT_PRIMS):
            f.write('        def Scope "P%d"\n        {\n        }\n' % n)
        f.write('    }\n}\n')


try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, QModelIndex, Qt, QTimer
    from PySide6.QtWidgets import QComboBox, QDialog, QLineEdit, QPlainTextEdit, QTabWidget, QToolButton, QTreeView, QWidget

    from guitest import OUT, check, close_all_panels, finish, plugin_problem, report, require, run, shot, viewer_up, widgets_of

    PATH_ROLE = Qt.UserRole + 1

    os.makedirs(OUT, exist_ok=True)
    scene_name = "scene-prim-inspector.usda"
    scene_path = os.path.join(OUT, scene_name)
    write_scene(scene_path)

    usd = app.createNode("USDRead")
    require(usd is not None, "create USDRead", plugin_problem("USDRead"))
    usd.getParam("initialDraw").setValue(3)
    usd.getParam("filename").setValue(scene_path)
    camera = app.createNode("USDCamera")
    require(camera is not None, "create USDCamera", plugin_problem("USDCamera"))
    edit = app.createNode("USDEditTransform")
    require(edit is not None, "create USDEditTransform", plugin_problem("USDEditTransform"))
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    usd.setPosition(400, 0)
    camera.setPosition(0, 0)
    edit.setPosition(0, 100)
    viewer.setPosition(0, 200)
    require(edit.connectInput(0, camera), "connect the USDEditTransform to the USDCamera")
    camera_prim = str(camera.getParam("primPath").getValue())
    edit.getParam("primPattern").setValue(camera_prim)
    edit.getParam("translate").set(1.0, 2.0, 3.0)
    transform_attribute = "xformOp:transform:" + edit.getScriptName()

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return hold(views[0]) if views else None

    # Looking a widget up wraps every widget in the application, which takes the main thread far
    # longer than the heartbeat budget. The polls run there too, so they must not repeat the walk.
    _found = {}

    def panel():
        if "panel" not in _found:
            panels = widgets_of("SceneGraphPanel")
            if panels:
                _found["panel"] = hold(panels[0])
        return _found.get("panel")

    def child(cls, name):
        if name not in _found:
            p = panel()
            try:
                c = hold(p.findChild(cls, name)) if p is not None else None
            except RuntimeError:
                return None
            if c is None:
                return None
            _found[name] = c
        return _found[name]

    def tree():
        return child(QTreeView, "sceneGraphTree")

    def model():
        t = tree()
        try:
            return hold(t.model()) if t is not None else None
        except RuntimeError:
            return None

    def inspector():
        return child(QWidget, "sceneGraphInspector")

    def state(name):
        return inspector().property("natronSceneGraphInspector" + name)

    def hold_reads(hold):
        inspector().setProperty("natronSceneGraphInspectorHoldReads", hold)

    def rows(parent=QModelIndex()):
        m = model()
        if m is None:
            return []
        try:
            while m.canFetchMore(parent):
                m.fetchMore(parent)
            return [m.index(r, 0, parent) for r in range(m.rowCount(parent))]
        except RuntimeError:
            return []

    def path_of(index):
        return str(index.data(PATH_ROLE))

    def find(path):
        parent = QModelIndex()
        found = QModelIndex()
        for depth in range(1, path.count("/") + 1):
            prefix = "/" + "/".join(path.strip("/").split("/")[:depth])
            found = QModelIndex()
            for index in rows(parent):
                if path_of(index) == prefix:
                    found = index
                    break
            if not found.isValid():
                return found
            parent = found
        return found

    def top_paths():
        return [path_of(i) for i in rows()]

    def select(index):
        selection = tree().selectionModel()
        selection.select(index, QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        selection.setCurrentIndex(index, QItemSelectionModel.NoUpdate)

    def table(name):
        """The tab's rows as {first column: [every column's text]}."""
        v = child(QTreeView, name)
        m = v.model() if v is not None else None
        if m is None:
            return {}
        result = {}
        for r in range(m.rowCount()):
            cells = [m.index(r, c).data() for c in range(m.columnCount())]
            result[str(cells[0])] = ["" if c is None else str(c) for c in cells]
        return result

    def table_rows(name):
        """The tab's rows in order, each the list of every column's text."""
        v = child(QTreeView, name)
        m = v.model() if v is not None else None
        if m is None:
            return []
        return [["" if m.index(r, c).data() is None else str(m.index(r, c).data()) for c in range(m.columnCount())]
                for r in range(m.rowCount())]

    def open_panel_labels():
        labels = []
        for p in widgets_of("NodeSettingsPanel"):
            try:
                if p.isVisible():
                    labels.extend(le.text() for le in p.findChildren(QLineEdit) if le.metaObject().className().split("::")[-1] == "LineEdit")
            except RuntimeError:
                continue
        return labels

    def tooltip(name, key, column):
        v = child(QTreeView, name)
        m = v.model()
        for r in range(m.rowCount()):
            if str(m.index(r, 0).data()) == key:
                return str(m.index(r, column).data(Qt.ToolTipRole))
        return ""

    def foreground(name):
        v = child(QTreeView, name)
        m = v.model()
        return m.index(0, 0).data(Qt.ForegroundRole) if m.rowCount() > 0 else None

    def show_tab(index):
        child(QTabWidget, "sceneGraphInspectorTabs").setCurrentIndex(index)

    def settled_on(path):
        return str(state("Path")) == path and not state("Busy")

    def ancestor_of_class(widget, name):
        w = widget
        while w is not None:
            if w.metaObject().className().split("::")[-1] == name:
                return w
            w = w.parentWidget()
        return None

    def view_input(node):
        """Points the viewer's input 0 at node: connectInput refuses an input that is already connected."""
        viewer.disconnectInput(0)
        return viewer.connectInput(0, node)

    def main_thread_times_ms():
        """The calling thread's total time on a CPU, and total time ready to run without one."""
        with open("/proc/thread-self/schedstat") as f:
            fields = f.read().split()
        return int(fields[0]) / 1000000.0, int(fields[1]) / 1000000.0

    class Heartbeat:
        """The longest the main thread's event loop has gone without running a short timer.

        worst_cpu_ms is the most CPU one gap used, which is where a read on the main thread shows.
        worst_ms leaves out the time the thread was ready to run but had no CPU, of which a busy host
        adds hundreds of ms, so what it adds to worst_cpu_ms is time spent blocked.
        """

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
            self.worst_wall_ms = 0.0

        def beat(self):
            now = time.monotonic()
            cpu, waited = main_thread_times_ms()
            gap = (now - self.last) * 1000.0
            self.worst_wall_ms = max(self.worst_wall_ms, gap)
            self.worst_ms = max(self.worst_ms, gap - (waited - self.waited))
            self.worst_cpu_ms = max(self.worst_cpu_ms, cpu - self.cpu)
            self.last = now
            self.cpu = cpu
            self.waited = waited

        def start(self):
            self.reset()
            self.timer.start()

        def stop(self):
            self.timer.stop()
            return self.worst_ms

    def camera_section():
        index = find(camera_prim)
        require(index.isValid(), "the tree lists the camera %s (got %r)" % (camera_prim, top_paths()))
        select(index)
        ok = yield ("until", lambda: "focalLength" in table(ATTRIBUTES) and settled_on(camera_prim), POLL_TIMEOUT_S)
        attributes = table(ATTRIBUTES)
        check(ok, "the attributes tab lists the camera's focal length (got %r)" % (sorted(attributes.keys())[:12],))
        report("NOTE camera attributes: %r" % (sorted(attributes.keys())[:24],))

        def author(name):
            return attributes.get(name, [""] * 5)[4]

        check(author("focalLength") == camera.getScriptName(),
              "focalLength names the USDCamera (got %r)" % (author("focalLength"),))
        check(author(transform_attribute) == edit.getScriptName(),
              "the transform op names the USDEditTransform (got %r)" % (author(transform_attribute),))
        check(author("xformOpOrder") == edit.getScriptName(),
              "the op order names the USDEditTransform (got %r)" % (author("xformOpOrder"),))
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-inspector-camera-attributes.png")

        show_tab(2)
        ok = yield ("until", lambda: bool(table(METADATA)) and settled_on(camera_prim), POLL_TIMEOUT_S)
        metadata = table(METADATA)
        check(ok, "the metadata tab lists the camera's metadata (got %r)" % (sorted(metadata.keys()),))
        if "specifier" in metadata:
            check(metadata["specifier"][2] == camera.getScriptName(),
                  "the specifier names the node that defined the prim, not an edit's over (got %r)" % (metadata["specifier"][2],))
        show_tab(0)

    def composition_section():
        show_tab(COMPOSITION_TAB)
        ok = yield ("until", lambda: any(k.strip() == "Root" for k in table(COMPOSITION)) and settled_on(camera_prim), POLL_TIMEOUT_S)
        arcs = table_rows(COMPOSITION)
        check(ok, "the composition tab lists the camera's root arc (got %r)" % (arcs,))
        root = [row for row in arcs if row[0].strip() == "Root"]
        check(bool(root) and root[0][1] == camera_prim, "the root arc targets the camera prim (got %r)" % (root,))
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-inspector-composition.png")
        show_tab(0)

    def opinions_section():
        show_tab(0)
        attributes_view = child(QTreeView, ATTRIBUTES)
        attributes_model = attributes_view.model()
        focal = [r for r in range(attributes_model.rowCount()) if str(attributes_model.index(r, 0).data()) == "focalLength"]
        require(bool(focal), "the attributes tab lists focalLength")
        attributes_view.selectionModel().select(attributes_model.index(focal[0], 0), QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        show_tab(OPINIONS_TAB)
        ok = yield ("until", lambda: any(r[0] == "Property focalLength" for r in table_rows(OPINIONS)) and settled_on(camera_prim), POLL_TIMEOUT_S)
        opinions = table_rows(OPINIONS)
        check(ok, "the opinions tab lists the selected focal length (got %r)" % (opinions,))
        property_authors = [r[1] for r in opinions if r[0] == "Property focalLength"]
        check(property_authors == [camera.getScriptName()],
              "focalLength's only opinion is the USDCamera's (got %r)" % (property_authors,))
        prim_authors = [r[1] for r in opinions if r[0] == "Prim" and r[1]]
        check(prim_authors == [edit.getScriptName(), camera.getScriptName()],
              "the prim's opinions run strongest first, USDEditTransform then USDCamera (got %r)" % (prim_authors,))
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-inspector-opinions.png")

        close_all_panels()
        yield ("sleep", SETTLE_MS)
        edit_label = edit.getLabel()
        camera_label = camera.getLabel()
        check(edit_label not in open_panel_labels(), "no panel for the USDEditTransform is open before the double-click")
        opinions_view = child(QTreeView, OPINIONS)
        opinions_model = opinions_view.model()
        edit_row = [r for r in range(opinions_model.rowCount())
                    if str(opinions_model.index(r, 0).data()) == "Prim" and str(opinions_model.index(r, 1).data()) == edit.getScriptName()]
        require(bool(edit_row), "the opinions tab has a row naming the USDEditTransform")
        opinions_view.doubleClicked.emit(opinions_model.index(edit_row[0], 2))
        yield ("sleep", SETTLE_MS)
        check(edit_label not in open_panel_labels(), "double-clicking the row's layer cell opens no node")
        opinions_view.doubleClicked.emit(opinions_model.index(edit_row[0], 1))
        ok = yield ("until", lambda: edit_label in open_panel_labels(), POLL_TIMEOUT_S)
        check(ok, "double-clicking a row opens the USDEditTransform's settings panel (open: %r)" % (open_panel_labels(),))
        check(camera_label not in open_panel_labels(), "the USDCamera's panel stays closed")
        show_tab(0)

    def layer_section():
        show_tab(LAYER_TAB)
        mode = child(QComboBox, LAYER_MODE)
        text_view = child(QPlainTextEdit, LAYER_TEXT)
        require(mode is not None and text_view is not None, "the layer tab has its mode box and text view")
        mode.setCurrentIndex(0)
        ok = yield ("until", lambda: "over " in text_view.toPlainText() and settled_on(camera_prim), POLL_TIMEOUT_S)
        contribution = text_view.toPlainText()
        check(ok, "the node contribution holds the authored over (got %r)" % (contribution[:200],))
        check(contribution.lstrip().startswith("# Layer:") or "#usda" in contribution,
              "the node contribution is USDA (got %r)" % (contribution[:80],))
        check(transform_attribute in contribution,
              "the node contribution holds the transform op the USDEditTransform authored")
        check(text_view.isReadOnly(), "the text view is read-only")
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-inspector-layer.png")

        tree().selectionModel().clear()
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: not state("Busy"), POLL_TIMEOUT_S)
        check(ok and transform_attribute in text_view.toPlainText(), "the node contribution stays up with no prim selected")
        mode.setCurrentIndex(1)
        ok = yield ("until", lambda: not text_view.toPlainText(), POLL_TIMEOUT_S)
        check(ok, "the composed mode shows nothing with no prim selected (got %r)" % (text_view.toPlainText()[:80],))
        select(find(camera_prim))

        ok = yield ("until", lambda: "focalLength" in text_view.toPlainText(), POLL_TIMEOUT_S)
        composed = text_view.toPlainText()
        check(ok, "the composed mode holds the camera's focalLength (got %r)" % (composed[:200],))
        check(transform_attribute in composed, "the composed definition holds the edit's transform op too")
        check("#usda" in composed, "the composed definition is USDA")

        show_tab(0)

    def layer_prim_switch_section():
        show_tab(LAYER_TAB)
        text_view = child(QPlainTextEdit, LAYER_TEXT)
        select(find(SMALL))
        ok = yield ("until", lambda: "longValue" in text_view.toPlainText() and settled_on(SMALL), POLL_TIMEOUT_S)
        small = text_view.toPlainText()
        check(ok, "the composed mode shows the small prim (got %r)" % (small[:200],))
        check(LONG_VALUE in small, "a long value is shown whole")
        check("otherValue" not in small and "Heavy" not in small, "the composed mode leaves out other prims")
        select(find(OTHER))
        ok = yield ("until", lambda: "otherValue" in text_view.toPlainText() and settled_on(OTHER), POLL_TIMEOUT_S)
        check(ok, "switching the prim updates the composed mode (got %r)" % (text_view.toPlainText()[:200],))
        check("longValue" not in text_view.toPlainText(), "the composed mode no longer shows the previous prim")
        show_tab(0)

    def file_section(py_viewer):
        select(find(SMALL))
        ok = yield ("until", lambda: "size" in table(ATTRIBUTES) and settled_on(SMALL), POLL_TIMEOUT_S)
        attributes = table(ATTRIBUTES)
        check(ok, "the attributes tab lists the small prim (got %r)" % (sorted(attributes.keys())[:12],))
        report("NOTE small prim attributes: %r" % (sorted(attributes.keys()),))
        empty = [""] * 5
        check(attributes.get("size", empty)[3] == "Default Value",
              "a default value says so (got %r)" % (attributes.get("size", empty)[3],))
        check(attributes.get("animated", empty)[3] == "Time Samples",
              "samples say so (got %r)" % (attributes.get("animated", empty)[3],))
        check(attributes.get("size", empty)[4].endswith(scene_name),
              "an opinion from a file names the file (got %r)" % (attributes.get("size", empty)[4],))
        check(attributes.get("visibility", empty)[3] == "Fallback" and attributes.get("visibility", empty)[4] == "Schema Fallback",
              "an unauthored attribute is its schema's fallback (got %r)" % (attributes.get("visibility", empty),))

        cut = attributes.get("longValue", empty)[2]
        check(len(cut) == MAX_CELL_LENGTH + 3 and cut.endswith("...") and LONG_VALUE.startswith(cut[:-3]),
              "a long value is cut in the table (got %d characters)" % (len(cut),))
        tip = tooltip(ATTRIBUTES, "longValue", 2)
        check(tip.replace("\n", "") == LONG_VALUE, "the tooltip holds the whole value (got %d characters)" % (len(tip),))
        check(max(len(line) for line in tip.split("\n")) < LONG_VALUE_LENGTH, "the tooltip's lines are wrapped to fit the screen")

        def animated():
            return float(table(ATTRIBUTES).get("animated", ["", "", "nan"])[2])

        py_viewer.seek(1)
        ok = yield ("until", lambda: animated() == 10.0, POLL_TIMEOUT_S)
        check(ok, "at frame 1 the samples read 10 (got %r)" % (animated(),))
        py_viewer.seek(20)
        ok = yield ("until", lambda: animated() == 200.0, POLL_TIMEOUT_S)
        check(ok, "at frame 20 the samples read 200 (got %r)" % (animated(),))
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-inspector-attributes.png")

        show_tab(1)
        ok = yield ("until", lambda: "links" in table(RELATIONSHIPS) and settled_on(SMALL), POLL_TIMEOUT_S)
        relationships = table(RELATIONSHIPS)
        check(ok, "the relationships tab lists links (got %r)" % (sorted(relationships.keys()),))
        links = relationships.get("links", ["", "", ""])
        check(links[1] == OTHER + ", " + FLAT, "links lists its targets (got %r)" % (links[1],))
        check(links[2].endswith(scene_name), "links names the file (got %r)" % (links[2],))
        check(relationships.get("proxyPrim", ["", "", ""])[2] == "Schema Fallback",
              "an unauthored relationship is its schema's (got %r)" % (relationships.get("proxyPrim"),))
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-inspector-relationships.png")

        show_tab(2)
        ok = yield ("until", lambda: "kind" in table(METADATA) and settled_on(SMALL), POLL_TIMEOUT_S)
        metadata = table(METADATA)
        check(ok, "the metadata tab lists kind (got %r)" % (sorted(metadata.keys()),))
        check(metadata.get("kind", ["", "", ""])[1] == "component", "kind has its value (got %r)" % (metadata.get("kind"),))
        check(metadata.get("kind", ["", "", ""])[2].endswith(scene_name), "kind names the file (got %r)" % (metadata.get("kind"),))
        check("documentation" in metadata and "customData" in metadata, "documentation and custom data are listed (got %r)" % (sorted(metadata.keys()),))
        check("Prototype" not in metadata, "a prim that is no instance shows no prototype")
        yield ("sleep", SETTLE_MS)
        shot("scene-prim-inspector-metadata.png")

        select(find(INSTANCE))
        ok = yield ("until", lambda: "Prototype" in table(METADATA) and settled_on(INSTANCE), POLL_TIMEOUT_S)
        prototype = table(METADATA).get("Prototype", ["", "", ""])[1]
        check(ok and prototype.startswith("/__Prototype"), "an instance shows its prototype (got %r)" % (prototype,))
        show_tab(0)

    def selection_section():
        select(find(PROTO))
        ok = yield ("until", lambda: "visibility" in table(ATTRIBUTES) and settled_on(PROTO), POLL_TIMEOUT_S)
        require(ok, "the prototype's attributes are listed")
        attributes = child(QTreeView, ATTRIBUTES)
        rows_model = attributes.model()
        row = [r for r in range(rows_model.rowCount()) if str(rows_model.index(r, 0).data()) == "visibility"]
        attributes.selectionModel().select(rows_model.index(row[0], 0), QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)
        check(bool(attributes.selectionModel().selectedRows()), "an attribute is selected")
        select(find(PROTO + "/Child"))
        ok = yield ("until", lambda: settled_on(PROTO + "/Child"), POLL_TIMEOUT_S)
        check(ok, "the child prim is shown (got %r)" % (state("Path"),))
        check(not attributes.selectionModel().selectedRows(), "a new prim shows no attribute selected")

    def frames_leave_count_alone(py_viewer, counter, what):
        count = int(state(counter))
        for frame in range(2, 6):
            finished = int(state("ReadsFinished"))
            py_viewer.seek(frame)
            ok = yield ("until", lambda: int(state("ReadsFinished")) > finished and not state("Busy"), POLL_TIMEOUT_S)
            check(ok, "frame %d is read for %s" % (frame, what))
        check(int(state(counter)) == count, "frame changes do not read %s again (%d, was %d)" % (what, int(state(counter)), count))

    def frame_reads_section(py_viewer):
        select(find(SMALL))
        show_tab(COMPOSITION_TAB)
        ok = yield ("until", lambda: any(k.strip() == "Root" for k in table(COMPOSITION)) and settled_on(SMALL), POLL_TIMEOUT_S)
        require(ok, "the composition tab shows the small prim")
        yield from frames_leave_count_alone(py_viewer, "CompositionQueries", "the composition")
        queries = int(state("CompositionQueries"))
        select(find(OTHER))
        ok = yield ("until", lambda: settled_on(OTHER), POLL_TIMEOUT_S)
        check(ok and int(state("CompositionQueries")) == queries + 1, "a new prim queries its composition once (%d, was %d)" % (int(state("CompositionQueries")), queries))

        select(find(SMALL))
        show_tab(LAYER_TAB)
        text_view = child(QPlainTextEdit, LAYER_TEXT)
        child(QComboBox, LAYER_MODE).setCurrentIndex(1)
        ok = yield ("until", lambda: "longValue" in text_view.toPlainText() and settled_on(SMALL), POLL_TIMEOUT_S)
        require(ok, "the composed mode shows the small prim")
        yield from frames_leave_count_alone(py_viewer, "DefinitionReads", "the prim's definition")
        reads = int(state("DefinitionReads"))
        select(find(OTHER))
        ok = yield ("until", lambda: "otherValue" in text_view.toPlainText() and settled_on(OTHER), POLL_TIMEOUT_S)
        check(ok and int(state("DefinitionReads")) == reads + 1, "a new prim reads its definition once (%d, was %d)" % (int(state("DefinitionReads")), reads))
        show_tab(0)

    def read_counts():
        return int(state("ReadsStarted")), int(state("ReadsFinished")), int(state("ReadsCancelled"))

    def frames_under_way_section(py_viewer):
        """A held read stands in for a slow one: without this, each new frame would replace it."""
        select(find(SMALL))
        show_tab(COMPOSITION_TAB)
        ok = yield ("until", lambda: settled_on(SMALL), POLL_TIMEOUT_S)
        require(ok, "the composition tab shows the small prim")
        queries = int(state("CompositionQueries"))
        started, finished, cancelled = read_counts()
        hold_reads(True)
        select(find(OTHER))
        ok = yield ("until", lambda: int(state("ReadsStarted")) > started, POLL_TIMEOUT_S)
        require(ok, "the composition read to keep is under way")
        started = int(state("ReadsStarted"))
        for frame in (7, 8, 9):
            py_viewer.seek(frame)
            yield ("sleep", SETTLE_MS)
        check(read_counts() == (started, finished, cancelled),
              "frame changes leave a composition read under way (%r, was %r)" % (read_counts(), (started, finished, cancelled)))
        hold_reads(False)
        ok = yield ("until", lambda: settled_on(OTHER), POLL_TIMEOUT_S)
        check(ok, "the composition read under way finishes (got %r)" % (state("Path"),))
        check(read_counts() == (started, finished + 1, cancelled) and int(state("CompositionQueries")) == queries + 1,
              "that one read shows the prim (%r, %d queries, was %d)" % (read_counts(), int(state("CompositionQueries")), queries))

        show_tab(0)
        py_viewer.seek(1)
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: settled_on(OTHER) and "otherValue" in table(ATTRIBUTES), POLL_TIMEOUT_S)
        require(ok, "the attributes tab shows the other prim")
        started, finished, cancelled = read_counts()
        hold_reads(True)
        select(find(SMALL))
        ok = yield ("until", lambda: int(state("ReadsStarted")) > started, POLL_TIMEOUT_S)
        require(ok, "the attributes read to keep is under way")
        started = int(state("ReadsStarted"))
        py_viewer.seek(10)
        yield ("sleep", SETTLE_MS)
        py_viewer.seek(20)
        yield ("sleep", SETTLE_MS)
        check(read_counts() == (started, finished, cancelled),
              "frame changes leave an attributes read under way (%r, was %r)" % (read_counts(), (started, finished, cancelled)))
        hold_reads(False)
        ok = yield ("until", lambda: int(state("ReadsFinished")) >= finished + 2 and settled_on(SMALL), POLL_TIMEOUT_S)
        check(ok, "the attributes read finishes and the latest frame is read after it (%r)" % (read_counts(),))
        check(int(state("ReadsCancelled")) == cancelled, "neither read is cancelled (%d, was %d)" % (int(state("ReadsCancelled")), cancelled))
        animated = float(table(ATTRIBUTES).get("animated", ["", "", "nan"])[2])
        check(animated == 200.0, "the rows end at the latest frame (got %r)" % (animated,))

    def full_value_section():
        """Run with the large arrays on the Attributes tab."""
        name = "heavy_0"
        tip = tooltip(ATTRIBUTES, name, 2)
        check(len(tip) < 2 * MAX_TIP_LENGTH, "a large array's tooltip stays short (got %d characters)" % (len(tip),))
        check(tip.endswith("Double-click to show the whole value."), "the tooltip says how to see the rest (got %r)" % (tip[-60:],))
        attributes = child(QTreeView, ATTRIBUTES)
        rows_model = attributes.model()
        row = [r for r in range(rows_model.rowCount()) if str(rows_model.index(r, 0).data()) == name]
        require(bool(row), "the large array has a row")
        attributes.doubleClicked.emit(rows_model.index(row[0], 2))
        window = attributes.findChild(QDialog, "sceneGraphFullValue")
        require(window is not None, "double-clicking the large array opens its full value")
        text = window.findChild(QPlainTextEdit, "sceneGraphFullValueText")
        require(text is not None, "the full value window has its text")
        ok = yield ("until", lambda: not text.property("natronSceneGraphTextLoading"), LOAD_TIMEOUT_S)
        check(ok, "the full value arrives")
        count = text.toPlainText().replace("\n", "").count("1.25")
        check(count == HEAVY_ELEMENTS, "the window holds every element (got %d)" % (count,))
        window.close()

    def layer_text_section(beat):
        """A large text goes up without holding the main thread."""
        show_tab(LAYER_TAB)
        mode = child(QComboBox, LAYER_MODE)
        text_view = child(QPlainTextEdit, LAYER_TEXT)
        mode.setCurrentIndex(1)
        select(find(SMALL))
        ok = yield ("until", lambda: settled_on(SMALL) and not text_view.property("natronSceneGraphTextLoading"), POLL_TIMEOUT_S)
        require(ok, "the small prim's definition is up before the measurement")

        beat.start()
        select(find(HEAVY))
        ok = yield ("until", lambda: settled_on(HEAVY) and not text_view.property("natronSceneGraphTextLoading"), LOAD_TIMEOUT_S)
        worst = beat.stop()
        check(ok, "the large arrays' definition is read (got %r)" % (state("Path"),))
        report("NOTE worst main-thread gap showing the large definition: %.1f ms on a CPU, %.1f ms with blocking, %.1f ms with waiting for a CPU"
               % (beat.worst_cpu_ms, worst, beat.worst_wall_ms))
        check(beat.worst_cpu_ms <= HEARTBEAT_BUDGET_MS, "showing a large text leaves the main thread idle (%.1f ms)" % (beat.worst_cpu_ms,))
        check(worst <= HEARTBEAT_BLOCKED_BUDGET_MS, "showing a large text does not block the main thread (%.1f ms)" % (worst,))
        shown = text_view.toPlainText()
        check("heavy_%d" % (HEAVY_ATTRIBUTES - 1,) in shown, "the large text is shown whole (%d characters)" % (len(shown),))
        show_tab(0)

    def responsiveness_section():
        beat = Heartbeat()

        # One batch only: reading the whole flat Scope here would measure the tree, not the inspector.
        # Switching the Viewer away made the panel compose the pinned USDRead itself. Until that load
        # ends the panel still lists the stage it is leaving, whose rows can no longer be read.
        ok = yield ("until", lambda: not panel().property("natronSceneGraphSwitching") and find(FLAT).isValid(), LOAD_TIMEOUT_S)
        require(ok, "the tree lists the flat Scope (got %r)" % (top_paths(),))
        m = model()

        def flat_rows_read():
            flat = find(FLAT)
            if not flat.isValid():
                return False
            if m.canFetchMore(flat):
                m.fetchMore(flat)
            return m.rowCount(flat) > 0 and path_of(m.index(0, 0, flat)).startswith(FLAT + "/")

        ok = yield ("until", flat_rows_read, POLL_TIMEOUT_S)
        require(ok, "the flat Scope's first rows are read")
        flat = find(FLAT)
        # Selecting a row under a collapsed parent expands it, which is the tree's work, not the
        # inspector's, and a user selects a row they can already see.
        beat.start()
        tree().expand(flat)
        yield ("sleep", SETTLE_MS)
        report("NOTE worst main-thread gap expanding the flat Scope: %.1f ms" % (beat.stop(),))
        flat_child = m.index(0, 0, flat)
        flat_child_path = path_of(flat_child)

        select(find(SMALL))
        ok = yield ("until", lambda: settled_on(SMALL) and "size" in table(ATTRIBUTES), POLL_TIMEOUT_S)
        require(ok, "the small prim is shown before the measurement")

        beat.start()
        select(flat_child)
        ok = yield ("until", lambda: settled_on(flat_child_path), POLL_TIMEOUT_S)
        worst = beat.stop()
        check(ok, "the flat Scope's child is read (got %r)" % (state("Path"),))
        report("NOTE worst main-thread gap selecting in the flat Scope: %.1f ms on a CPU, %.1f ms with blocking, %.1f ms with waiting for a CPU"
               % (beat.worst_cpu_ms, worst, beat.worst_wall_ms))
        check(beat.worst_cpu_ms <= HEARTBEAT_BUDGET_MS, "selecting in the flat Scope leaves the main thread idle (%.1f ms)" % (beat.worst_cpu_ms,))
        check(worst <= HEARTBEAT_BLOCKED_BUDGET_MS, "selecting in the flat Scope does not block the main thread (%.1f ms)" % (worst,))

        select(find(SMALL))
        ok = yield ("until", lambda: settled_on(SMALL) and "size" in table(ATTRIBUTES), POLL_TIMEOUT_S)
        require(ok, "the small prim is shown again")

        hold_reads(True)
        select(find(HEAVY))
        ok = yield ("until", lambda: bool(state("Stale")), POLL_TIMEOUT_S)
        check(ok, "a slow read greys the last result")
        check(str(state("Path")) == SMALL and "size" in table(ATTRIBUTES),
              "the last result stays up during the read (got %r)" % (state("Path"),))
        check(foreground(ATTRIBUTES) is not None, "the last result's rows are greyed")
        shot("scene-prim-inspector-stale.png")
        hold_reads(False)
        ok = yield ("until", lambda: settled_on(HEAVY), LOAD_TIMEOUT_S)
        require(ok, "the large arrays are read for the screenshot (got %r)" % (state("Path"),))
        select(find(SMALL))
        ok = yield ("until", lambda: settled_on(SMALL) and "size" in table(ATTRIBUTES), POLL_TIMEOUT_S)
        require(ok, "the small prim is back before the cancelled read")

        heavy_index = find(HEAVY)
        other_index = find(OTHER)
        started = int(state("ReadsStarted"))
        cancelled = int(state("ReadsCancelled"))
        beat.start()
        hold_reads(True)
        select(heavy_index)
        ok = yield ("until", lambda: int(state("ReadsStarted")) > started, POLL_TIMEOUT_S)
        require(ok, "the read to cancel is under way")

        select(other_index)
        yield ("sleep", SETTLE_MS)
        hold_reads(False)
        ok = yield ("until", lambda: settled_on(OTHER) and "otherValue" in table(ATTRIBUTES), LOAD_TIMEOUT_S)
        check(ok, "a new selection shows its own prim (got %r)" % (state("Path"),))
        ok = yield ("until", lambda: int(state("ReadsCancelled")) > cancelled, LOAD_TIMEOUT_S)
        check(ok, "a new selection cancels the read under way")
        check(not any(name.startswith("heavy_") for name in table(ATTRIBUTES)), "nothing of the cancelled read is shown")
        check(not state("Stale") and foreground(ATTRIBUTES) is None, "the rows are no longer greyed")

        select_time = time.monotonic()
        select(heavy_index)
        ok = yield ("until", lambda: settled_on(HEAVY), LOAD_TIMEOUT_S)
        read_ms = (time.monotonic() - select_time) * 1000.0
        worst = beat.stop()
        check(ok, "the large arrays are read (got %r)" % (state("Path"),))
        heavy = table(ATTRIBUTES)
        check(sum(1 for name in heavy if name.startswith("heavy_")) == HEAVY_ATTRIBUTES, "every large array is listed (got %d rows)" % (len(heavy),))
        report("NOTE worst main-thread gap reading large arrays: %.1f ms on a CPU, %.1f ms with blocking, %.1f ms with waiting for a CPU"
               % (beat.worst_cpu_ms, worst, beat.worst_wall_ms))
        report("NOTE reading the large arrays took %.0f ms" % (read_ms,))
        check(beat.worst_cpu_ms <= HEARTBEAT_BUDGET_MS, "reading large arrays leaves the main thread idle (%.1f ms)" % (beat.worst_cpu_ms,))
        check(worst <= HEARTBEAT_BLOCKED_BUDGET_MS, "reading large arrays does not block the main thread (%.1f ms)" % (worst,))

        yield from full_value_section()
        yield from layer_text_section(beat)

    def steps():
        require(viewer.connectInput(0, edit), "connect the Viewer to the USDEditTransform")
        ok = yield ("until", lambda: viewer_up() or view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")
        ok = yield ("until", lambda: view() is not None, POLL_TIMEOUT_S)
        require(ok, "the scene view is up")
        require(panel() is not None, "the Scene Graph panel exists")

        home = ancestor_of_class(view(), "TabWidget")
        require(home is not None, "the scene view is in a pane")
        split = app.getTabWidget(home.objectName()).splitHorizontally()
        require(split is not None, "split the viewer's pane")
        require(app.moveTab("sceneGraph", split), "move the Scene Graph panel beside the viewer")
        split = None
        camera_root = "/" + camera_prim.strip("/").split("/")[0]
        ok = yield ("until", lambda: panel().isVisible() and camera_root in top_paths(), POLL_TIMEOUT_S)
        require(ok, "the docked panel lists the camera's scene (got %r)" % (top_paths(),))
        require(inspector() is not None, "the panel has its inspector")

        yield from camera_section()
        yield from composition_section()
        yield from opinions_section()
        yield from layer_section()

        py_viewer = hold(app.getViewer(viewer.getScriptName()))
        require(view_input(usd), "switch the Viewer to the USDRead")
        ok = yield ("until", lambda: ROOT in top_paths(), LOAD_TIMEOUT_S)
        require(ok, "the tree shows the file's scene (got %r)" % (top_paths(),))
        yield from file_section(py_viewer)
        yield from layer_prim_switch_section()
        yield from selection_section()
        yield from frame_reads_section(py_viewer)
        yield from frames_under_way_section(py_viewer)

        # Pinned, a selection does not reach the viewer, whose repaint is not what is measured.
        pin = child(QToolButton, "sceneGraphPin")
        require(pin is not None, "the panel has its pin button")
        pin.click()
        ok = yield ("until", lambda: bool(panel().property("natronSceneGraphPinned")) and ROOT in top_paths(), LOAD_TIMEOUT_S)
        require(ok, "the pin button pins the panel to the USDRead (got %r)" % (top_paths(),))
        require(view_input(edit), "switch the Viewer back to the camera")
        yield ("sleep", SETTLE_MS)
        yield from responsiveness_section()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
