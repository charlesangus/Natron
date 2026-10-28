"""M52 - USD Layer Stack Completion: USDEditPrim, USDVariant, USDReference and USDSetAttribute in the
3D viewer, the PrimSelector pattern knob, and the authored-only edit semantics.

Fixtures: Tests/fixtures/m52-layer-stack.usda (World/Cam, World/Asset/Geo, a cube whose `look` variant set
moves it left+red or right+blue) and Tests/fixtures/m52-referenced.usda (Root/Marker).

Checks:
 1. USDEditPrim (Edit, schema Camera) on /World/Cam: untouched it authors nothing; after touching only
    focalLength the layered USDWrite's root layer holds exactly that one property, and only that knob's
    Authored toggle is on. Panel screenshot.
 2. USDVariant on /World/Asset switches the `look` variant: the Variants menu lists left and right, and
    the cube's red pixels move to blue pixels on the other side of the viewer (relative pixel comparison,
    Un-tone-mapped view) and the flattened stage agrees.
 3. A PrimSelector expression ("//Cam + //Geo") typed into USDEditPrim's Prim Pattern is accepted
    without a node error, the panel shows it, and it selects both prims.
 4. USDEditPrim in Define mode (schema Sphere, /rig/ball) creates the prim and the missing /rig Xform;
    the viewer changes and the flattened stage has both.
 5. USDReference brings /World/Ext (from m52-referenced.usda) into the scene: /World/Ext/Marker composes
    and the viewer changes.
 6. USDSetAttribute authors a typed custom float, and a color3f primvar that the viewer shows.

Screenshots: m52_editprim_panel.png, m52_variant_left.png, m52_variant_right.png, m52_variant_panel.png,
m52_selector_panel.png, m52_define_panel.png, m52_define_viewer.png, m52_reference_panel.png,
m52_setattribute_panel.png, m52_setattribute_primvar.png (full window), scene shots with _viewer.png crops.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 40.0
SETTLE_MS = 800
STARTUP_TIMEOUT_S = 60.0
UNTONED_VIEW = "Un-tone-mapped"
EXPRESSION = "//Cam + //Geo"

try:
    from PySide6.QtCore import Q_ARG, QMetaObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QPlainTextEdit, QTextEdit
    from pxr import Sdf, Usd

    from guitest import FIXTURES, OUT, check, close_all_panels, finish, plugin_problem, report, require, run, shot, widgets_of

    layer_path = os.path.join(FIXTURES, "m52-layer-stack.usda")
    referenced_path = os.path.join(FIXTURES, "m52-referenced.usda")
    require(os.path.isfile(layer_path), "fixture exists: " + layer_path)
    require(os.path.isfile(referenced_path), "fixture exists: " + referenced_path)
    work_dir = os.path.join(OUT, "work")
    if not os.path.isdir(work_dir):
        os.makedirs(work_dir)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getLabel(), name))
        return p

    def set_choice(p, entry_id):
        for i in range(p.getNumOptions()):
            if p.getOption(i) == entry_id:
                p.setValue(i)
                return True
        return False

    # The Viewer first; every node and writer below is held in a variable for the whole run, since PySide
    # child wrappers die with temporary parents.
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(0, 500)

    read = make("USDRead", 0, 0)
    param(read, "filename").setValue(layer_path)
    probe = make("USDWrite", 600, 0)
    layered = make("USDWrite", 600, 100)
    edit = make("USDEditPrim", 0, 100)
    variant = make("USDVariant", 200, 100)
    define = make("USDEditPrim", 400, 100)
    reference = make("USDReference", 0, 250)
    set_attr = make("USDSetAttribute", 200, 250)
    set_primvar = make("USDSetAttribute", 400, 250)
    for node in (edit, variant, define, reference, set_attr, set_primvar):
        require(node.connectInput(0, read), "connect USDRead1 to %s" % node.getLabel())
    counter = [0]

    def view():
        views = [w for w in widgets_of("SceneViewport") if w.isVisible()]
        return views[0] if views else None

    def scene_drawn():
        v = view()
        return (v is not None and bool(v.property("natronSceneSynced")) and bool(v.property("natronSceneShown"))
                and not v.property("natronSceneError"))

    def grab_view():
        v = view()
        if v is None:
            return None
        QApplication.processEvents()
        origin = v.mapToGlobal(v.rect().topLeft())
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        return image.copy(origin.x(), origin.y(), v.width(), v.height())

    def hue(c):
        r, g, b = c.red(), c.green(), c.blue()
        if r > 50 and r > 2 * g and r > 2 * b:
            return "r"
        if b > 50 and b > 2 * g and b > 2 * r:
            return "b"
        if g > 50 and g > 2 * r and g > 2 * b:
            return "g"
        return None

    def coloured(step=6):
        """{hue: (count, centroid x)} over a step-px grid of the scene view."""
        image = grab_view()
        out = {}
        if image is None:
            return out
        for y in range(0, image.height(), step):
            for x in range(0, image.width(), step):
                h = hue(image.pixelColor(x, y))
                if h:
                    n, sx = out.get(h, (0, 0.0))
                    out[h] = (n + 1, sx + x)
        return {h: (n, sx / n) for h, (n, sx) in out.items()}

    def differing_pixels(a, b, step=4):
        if a is None or b is None or a.size() != b.size():
            return 0
        count = 0
        for y in range(0, a.height(), step):
            for x in range(0, a.width(), step):
                pa, pb = a.pixelColor(x, y), b.pixelColor(x, y)
                if abs(pa.red() - pb.red()) + abs(pa.green() - pb.green()) + abs(pa.blue() - pb.blue()) > 30:
                    count += 1
        return count

    def save_shots(name):
        shot(name + ".png")
        image = grab_view()
        if image is not None:
            path = os.path.join(OUT, name + "_viewer.png")
            image.save(path)
            report("SHOT " + path)
        return image

    def untoned_combo():
        for w in widgets_of("ComboBox"):
            try:
                if w.isVisible() and UNTONED_VIEW in [str(i) for i in (w.property("natronItems") or [])]:
                    return w
            except RuntimeError:
                continue
        return None

    def select_untoned():
        w = untoned_combo()
        if w is None:
            return False
        items = [str(i) for i in w.property("natronItems")]
        QMetaObject.invokeMethod(w, "setCurrentIndex", Q_ARG(int, items.index(UNTONED_VIEW)))
        QApplication.processEvents()
        return str(w.property("natronCurrentText")) == UNTONED_VIEW

    def node_graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
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

    def panel_texts(panel):
        texts = []
        for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
            texts.append(w.text())
        for w in panel.findChildren(QTextEdit) + panel.findChildren(QPlainTextEdit):
            texts.append(w.toPlainText())
        return texts

    def open_panel(node):
        graph = node_graph_view()
        require(graph is not None, "the node graph is on screen")
        close_all_panels()
        app.selectNode(node, True)
        QMetaObject.invokeMethod(graph, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "open %s's settings panel" % node.getLabel())
        yield ("sleep", SETTLE_MS)

    def show(node):
        viewer.disconnectInput(0)
        require(viewer.connectInput(0, node), "connect the Viewer to %s" % node.getLabel())

    def write(source, writer, mode):
        """Writes source through writer (0 layered, 1 flattened) to a fresh .usda and returns its path."""
        counter[0] += 1
        path = os.path.join(work_dir, "probe%d.usda" % counter[0])
        writer.disconnectInput(0)
        require(writer.connectInput(0, source), "connect the writer to %s" % source.getLabel())
        writer.getParam("filename").setValue(path)
        writer.getParam("exportMode").setValue(mode)
        app.renderBlocking(writer, 1, 1)
        return path if os.path.isfile(path) else None

    def flattened(source):
        path = write(source, probe, 1)
        return Usd.Stage.Open(path) if path else None

    def authored_properties(source):
        """{prim path: [property names]} in the root layer a layered write of source produced."""
        path = write(source, layered, 0)
        if path is None:
            return None
        layer = Sdf.Layer.FindOrOpen(path)
        if layer is None:
            return None
        out = {}

        def walk(spec):
            names = [p.name for p in spec.properties]
            if names:
                out[str(spec.path)] = sorted(names)
            for child in spec.nameChildren:
                walk(child)

        for root in layer.rootPrims:
            walk(root)
        return out

    def prim_paths(stage):
        return sorted(str(p.GetPath()) for p in stage.Traverse()) if stage else []

    def steps():
        show(read)
        ok = yield ("until", scene_drawn, STARTUP_TIMEOUT_S)
        v = view()
        require(ok, "the scene view shows the USDRead", repr(v.property("natronSceneError")) if v else "no scene view")
        ok = yield ("until", select_untoned, POLL_TIMEOUT_S)
        check(ok, "the Viewer's View menu offers %s" % UNTONED_VIEW)

        # 1. USDEditPrim over a camera authors only what was touched.
        param(edit, "primPattern").setValue("/World/Cam")
        require(set_choice(param(edit, "schema"), "Camera"), "the Schema menu lists Camera")
        ok = yield ("until", lambda: edit.getParam("focalLength") is not None, POLL_TIMEOUT_S)
        require(ok, "choosing the Camera schema builds a focalLength knob")
        show(edit)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        untouched = authored_properties(edit)
        check(untouched == {}, "untouched, USDEditPrim authors no properties (%r)" % (untouched,))
        param(edit, "focalLength").setValue(50.0)
        yield ("sleep", SETTLE_MS)
        touched = authored_properties(edit)
        check(touched == {"/World/Cam": ["focalLength"]},
              "touching only focalLength authors exactly that property on /World/Cam (%r)" % (touched,))
        check(bool(param(edit, "focalLengthAuthored").get()), "focalLength's Authored toggle is on")
        others = [n for n in ("horizontalApertureAuthored", "verticalApertureAuthored", "clippingRangeAuthored",
                              "projectionAuthored") if edit.getParam(n) is not None and edit.getParam(n).get()]
        check(not others, "no other Authored toggle is on (%r)" % (others,))
        yield from open_panel(edit)
        shot("m52_editprim_panel.png")
        panel = panel_of(edit)
        texts = panel_texts(panel) if panel is not None else []
        check(any("Focal Length" in t for t in texts), "the EditPrim panel lists Focal Length")

        # 3. A PrimSelector expression is accepted and shown.
        param(edit, "primPattern").setValue(EXPRESSION)
        yield ("sleep", SETTLE_MS)
        yield from open_panel(edit)
        shot("m52_selector_panel.png")
        panel = panel_of(edit)
        check(panel is not None and any(EXPRESSION in t for t in panel_texts(panel)),
              "the panel shows the expression %r" % EXPRESSION)
        check(edit.getPersistentMessage() == "", "the expression raises no node error (%r)" % edit.getPersistentMessage())
        selected = authored_properties(edit)
        check(selected == {"/World/Cam": ["focalLength"], "/World/Asset/Geo": ["focalLength"]},
              "the expression selects /World/Cam and /World/Asset/Geo (%r)" % (selected,))

        # 2. USDVariant switches the variant in the 3D view.
        show(variant)
        param(variant, "primPattern").setValue("/World/Asset")
        param(variant, "variantSet").setValue("look")
        param(variant, "variant").setValue("left")
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        left_view = save_shots("m52_variant_left")
        left = coloured()
        report("NOTE left variant hues %r" % (left,))
        check("r" in left and left["r"][0] > 20, "the left variant shows the red cube")
        ok = yield ("until", lambda: variant.getParam("variants").getNumOptions() >= 2, POLL_TIMEOUT_S)
        options = [variant.getParam("variants").getOption(i) for i in range(variant.getParam("variants").getNumOptions())]
        check(ok and "left" in options and "right" in options, "the Variants menu lists left and right (%r)" % (options,))
        param(variant, "variant").setValue("right")
        ok = yield ("until", lambda: differing_pixels(left_view, grab_view()) > 100, POLL_TIMEOUT_S)
        check(ok, "selecting the right variant changes the viewer")
        yield ("sleep", SETTLE_MS)
        right_view = save_shots("m52_variant_right")
        right = coloured()
        report("NOTE right variant hues %r" % (right,))
        check("b" in right and right["b"][0] > 20 and right.get("r", (0, 0))[0] < right["b"][0] // 4,
              "the right variant shows the blue cube, not the red one")
        if "r" in left and "b" in right:
            check(abs(right["b"][1] - left["r"][1]) > 20,
                  "the cube is on the other side of the viewer (%.0f px apart)" % abs(right["b"][1] - left["r"][1]))
        stage = flattened(variant)
        geo = stage.GetPrimAtPath("/World/Asset") if stage else None
        selection = geo.GetVariantSets().GetVariantSet("look").GetVariantSelection() if geo else None
        check(selection == "right", "the composed stage's look variant is right (%r)" % (selection,))
        yield from open_panel(variant)
        shot("m52_variant_panel.png")

        # 4. Define mode creates a typed prim and its missing ancestor.
        define_pattern = "/rig/ball"
        param(define, "mode").setValue(1)
        param(define, "primPath").setValue(define_pattern)
        show(define)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        before = save_shots("m52_define_before")
        before_stage = flattened(define)
        check("/rig/ball" not in prim_paths(before_stage) or before_stage.GetPrimAtPath("/rig/ball").GetTypeName() == "",
              "with schema None Define makes no typed prim")
        require(set_choice(param(define, "schema"), "Sphere"), "the Schema menu lists Sphere")
        ok = yield ("until", lambda: define.getParam("radius") is not None, POLL_TIMEOUT_S)
        require(ok, "choosing the Sphere schema builds a radius knob")
        param(define, "radius").setValue(1.5)
        ok = yield ("until", lambda: differing_pixels(before, grab_view()) > 100, POLL_TIMEOUT_S)
        check(ok, "defining a Sphere changes the viewer")
        yield ("sleep", SETTLE_MS)
        save_shots("m52_define_viewer")
        stage = flattened(define)
        paths = prim_paths(stage)
        report("NOTE define prims %r" % (paths,))
        check("/rig" in paths and "/rig/ball" in paths, "the stage has /rig and /rig/ball")
        if stage and stage.GetPrimAtPath("/rig/ball") and stage.GetPrimAtPath("/rig"):
            check(stage.GetPrimAtPath("/rig/ball").GetTypeName() == "Sphere", "/rig/ball is a Sphere")
            check(stage.GetPrimAtPath("/rig").GetTypeName() == "Xform", "the missing ancestor /rig is an Xform")
            radius = stage.GetPrimAtPath("/rig/ball").GetAttribute("radius").Get()
            check(radius is not None and abs(radius - 1.5) < 1e-6, "the touched radius is authored (%r)" % (radius,))
        yield from open_panel(define)
        shot("m52_define_panel.png")

        # 5. USDReference brings in a file.
        show(read)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        plain = grab_view()
        param(reference, "primPath").setValue("/World/Ext")
        require(set_choice(param(reference, "source"), "file"), "the Source menu offers File")
        param(reference, "file").setValue(referenced_path)
        show(reference)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        ok = yield ("until", lambda: differing_pixels(plain, grab_view()) > 50, POLL_TIMEOUT_S)
        check(ok, "the referenced cube changes the viewer")
        yield ("sleep", SETTLE_MS)
        save_shots("m52_reference")
        stage = flattened(reference)
        paths = prim_paths(stage)
        report("NOTE reference prims %r" % (paths,))
        check("/World/Ext/Marker" in paths, "the referenced file's Marker composes under /World/Ext")
        check("/World/Asset/Geo" in paths, "the input scene is still there")
        check(reference.getPersistentMessage() == "", "USDReference reports no error (%r)" % reference.getPersistentMessage())
        yield from open_panel(reference)
        shot("m52_reference_panel.png")

        # 6. USDSetAttribute sets typed values.
        param(set_attr, "primPattern").setValue("/World/Asset/Geo")
        param(set_attr, "attributeName").setValue("m52:gain")
        require(set_choice(param(set_attr, "valueType"), "float"), "the Value Type menu offers float")
        ok = yield ("until", lambda: set_attr.getParam("value") is not None, POLL_TIMEOUT_S)
        require(ok, "the float type builds a value knob")
        param(set_attr, "value").setValue(2.5)
        show(set_attr)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        stage = flattened(set_attr)
        attr = stage.GetPrimAtPath("/World/Asset/Geo").GetAttribute("m52:gain") if stage else None
        check(attr is not None and attr.IsValid(), "the custom attribute m52:gain exists on Geo")
        if attr is not None and attr.IsValid():
            check(str(attr.GetTypeName()) == "float", "m52:gain is a float (%s)" % attr.GetTypeName())
            check(abs(float(attr.Get()) - 2.5) < 1e-6, "m52:gain holds the value set (%r)" % (attr.Get(),))
        yield from open_panel(set_attr)
        shot("m52_setattribute_panel.png")

        show(read)
        ok = yield ("until", scene_drawn, POLL_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        base = grab_view()
        param(set_primvar, "primPattern").setValue("/World/Asset/Geo")
        param(set_primvar, "attributeName").setValue("displayColor")
        require(set_choice(param(set_primvar, "valueType"), "color3f"), "the Value Type menu offers color3f")
        param(set_primvar, "primvar").setValue(True)
        ok = yield ("until", lambda: set_primvar.getParam("value") is not None, POLL_TIMEOUT_S)
        require(ok, "the color3f type builds a value knob")
        value = set_primvar.getParam("value")
        value.set(0.0, 1.0, 0.0, 1.0)
        report("NOTE primvar colour reads back %r" % ([value.get(i) if hasattr(value, "get") else None for i in range(3)],))
        show(set_primvar)
        ok = yield ("until", lambda: differing_pixels(base, grab_view()) > 100, POLL_TIMEOUT_S)
        check(ok, "the displayColor primvar changes the viewer")
        yield ("sleep", SETTLE_MS)
        save_shots("m52_setattribute_primvar")
        green = coloured()
        check("g" in green and green["g"][0] > 20, "the cube is green (%r)" % (green,))
        stage = flattened(set_primvar)
        pv = stage.GetPrimAtPath("/World/Asset/Geo").GetAttribute("primvars:displayColor") if stage else None
        check(pv is not None and pv.IsValid() and len(pv.Get() or []) == 1 and tuple(pv.Get()[0])[1] > 0.9,
              "primvars:displayColor is a one-element green color3f[] (%r)" % (pv.Get() if pv is not None and pv.IsValid() else None,))

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
