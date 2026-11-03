"""GUI test of the Roto node's Object Select tool: a click on a still in the viewer prompts a
segmentation model, the mask is previewed as outlines, Return commits it as Beziers keyed on the
frame (the hole as a Stencil shape), and one undo removes them and brings the selection back to
adjust. Object Density changes the committed shapes' point count. Object Inference Resolution
re-runs the segmentation on a smaller image while the click stays put, and the shapes stay at full
resolution; the undo step keeps that smaller mask, so committing again after the undo gives the same
shapes. Leaving the tool drops the selection and picking it again encodes the Source again. When the
Source cannot be encoded, the outline goes and Return commits nothing.

Nothing is downloaded. The manifest is the built-in one plus two entries for the committed fake
segmentation models (Tests/fixtures/ai/seg-*.onnx, whose mask is the image's bright pixels), and
the script installs them under the output folder's XDG_DATA_HOME before the Roto node is created.
The still, a bright disc with a square hole on a dark ground, is written by the script.

Screenshots, in $NATRON_GUI_TEST_OUT (default build/gui-test-out/):
    m205-object-tool.png       the tool picked, its settings in the viewer's toolbar
    m205-object-preview.png    the outline preview after one click
    m205-object-box.png        a box prompt with the SAM-shaped model
    m205-object-committed.png  the committed shapes
    m205-object-undone.png     after undo: the selection is back
    m205-object-dense.png      the selection at a high density, before the commit
    m205-object-quarter.png    the selection re-segmented at 25% inference resolution
    m205-object-reencoded.png  the tool picked again after another tool, and clicked
    m205-object-failed.png     the Source unreadable: no outline is left to commit

Usage, in the natron-dev container:
    Tests/gui/run-gui-test.sh Tests/gui/m205_object_select.py <Natron.AppImage or release binary>
"""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 30.0
SETTLE_MS = 500
RENDER_MS = 1500

# The still, in pixels, top row first: the object, a disc given by its bounding box, and the
# square hole cut in it. A curved outline is what a density change shows on.
IMAGE_W, IMAGE_H = 128, 96
OBJECT = (24, 8, 104, 88)
HOLE = (52, 36, 76, 60)
BRIGHT, DARK = 235, 20

SEGNEXT_ID = "test-seg-segnext"
SAM_ID = "test-seg-sam"

try:
    import guitest as gt

    work = os.path.join(gt.OUT, "m205-object")
    data_home = os.path.join(work, "data")
    os.makedirs(data_home, exist_ok=True)
    with open(os.path.join(gt.ROOT, "Engine", "Ai", "data", "ai_models.json")) as f:
        manifest = json.load(f)

    def fixture_entry(model_id, name):
        return {
            "id": model_id, "task": "mask-object", "name": name, "package": model_id,
            "files": ["encoder.onnx", "decoder.onnx"],
            "url": "file://" + os.path.join(work, model_id + ".dtmodel"),
            "sha256": "0" * 64, "size": 1, "weightsLicence": "MIT", "trainingDataLicence": "none",
            "osaid": "Open Source AI", "cardUrl": "https://example.invalid/card", "default": False,
        }

    manifest["models"].append(fixture_entry(SEGNEXT_ID, "Test SegNext"))
    manifest["models"].append(fixture_entry(SAM_ID, "Test SAM"))
    manifest_path = os.path.join(work, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=1)
    os.environ["NATRON_AI_MANIFEST"] = manifest_path
    os.environ["XDG_DATA_HOME"] = data_home

    from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QPoint, QPointF, QStandardPaths, Qt
    from PySide6.QtGui import QColor, QGuiApplication, QImage, QKeyEvent, QMouseEvent
    from PySide6.QtWidgets import QApplication, QMainWindow

    try:
        from PySide6.QtTest import QTest
    except ImportError:
        QTest = None

    import NatronEngine
    from guitest import check, report, require

    def install(model_id, arch, prefix):
        """Lays the package out as the downloader leaves it; the models folder is the one
        ModelRegistry derives from the data location."""
        models = os.path.join(QStandardPaths.writableLocation(QStandardPaths.AppLocalDataLocation), "models")
        folder = os.path.join(models, model_id)
        os.makedirs(folder, exist_ok=True)
        config = {"id": model_id, "name": model_id, "task": "mask", "arch": arch, "backend": "onnx",
                  "version": "1.0", "model_card": {"license": "MIT", "training_data_license": "none"},
                  "attributes": {"input_size": 64}}
        with open(os.path.join(folder, "config.json"), "w") as f:
            json.dump(config, f, indent=1)
        for part in ("encoder", "decoder"):
            shutil.copyfile(os.path.join(gt.FIXTURES, "ai", "%s-%s.onnx" % (prefix, part)),
                            os.path.join(folder, part + ".onnx"))
        return folder

    def write_still(path):
        image = QImage(IMAGE_W, IMAGE_H, QImage.Format_RGB32)
        image.fill(QColor(DARK, DARK, DARK))
        for y in range(IMAGE_H):
            for x in range(IMAGE_W):
                if in_disc(x + 0.5, y + 0.5) and not inside(x + 0.5, y + 0.5, HOLE):
                    image.setPixelColor(x, y, QColor(BRIGHT, BRIGHT, BRIGHT))
        return image.save(path)

    def inside(x, y, rect):
        return rect[0] <= x < rect[2] and rect[1] <= y < rect[3]

    def in_disc(x, y):
        cx, cy = 0.5 * (OBJECT[0] + OBJECT[2]), 0.5 * (OBJECT[1] + OBJECT[3])
        r = 0.5 * (OBJECT[2] - OBJECT[0])
        return (x - cx) ** 2 + (y - cy) ** 2 < r * r

    def truth(x, y):
        """Whether canonical (x, y), y up, is in the object."""
        row = IMAGE_H - y
        return in_disc(x, row) and not inside(x, row, HOLE)

    def visible_gl():
        found = [w for w in gt.widgets_of("ViewerGL") if w.isVisible()]
        return found[0] if found else None

    def untone_viewer():
        """Switches the viewer to a view without tone mapping, so the still's bright pixels stay
        bright on screen: the default ACES 2.0 SDR view shows 235 (linear 0.83) as about 171.
        Returns the view picked, or None."""
        for combo in gt.widgets_of("ComboBox"):
            try:
                if not combo.isVisible() or "<b>View:</b>" not in (combo.toolTip() or ""):
                    continue
            except RuntimeError:
                continue
            items = [str(i) for i in (combo.property("natronItems") or [])]
            for wanted in ("Un-tone-mapped", "Raw"):
                if wanted in items:
                    QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(wanted)))
                    QApplication.processEvents()
                    return wanted
        return None

    def bright_box(gl):
        """The bounding box of the still's bright pixels on screen, in gl's coordinates."""
        origin = gl.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()
        xs, ys = [], []
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                c = img.pixelColor(x, y)
                if c.red() > 180 and c.green() > 180 and c.blue() > 180:
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return None
        return (min(xs), min(ys), max(xs), max(ys))

    def outline_pixels(gl, box, margin=12):
        """How many pixels of the preview's orange outline the viewer shows around the still's
        bright box. Only that area counts: the tool's messages are drawn in the same orange at the
        viewport's top left."""
        origin = gl.mapToGlobal(QPoint(0, 0))
        img = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), gl.width(), gl.height()).toImage()
        n = 0
        for y in range(max(0, box[1] - margin), min(img.height(), box[3] + margin + 1), 2):
            for x in range(max(0, box[0] - margin), min(img.width(), box[2] + margin + 1), 2):
                c = img.pixelColor(x, y)
                if c.red() > 200 and 120 < c.green() < 210 and c.blue() < 90:
                    n += 1
        return n

    def send_mouse(widget, etype, pos, button, buttons, modifiers=Qt.NoModifier):
        QApplication.sendEvent(widget, QMouseEvent(etype, QPointF(pos), button, buttons, modifiers))
        QApplication.processEvents()

    def click(widget, pos, modifiers=Qt.NoModifier):
        send_mouse(widget, QEvent.MouseMove, pos, Qt.NoButton, Qt.NoButton, modifiers)
        send_mouse(widget, QEvent.MouseButtonPress, pos, Qt.LeftButton, Qt.LeftButton, modifiers)
        send_mouse(widget, QEvent.MouseButtonRelease, pos, Qt.LeftButton, Qt.NoButton, modifiers)

    def drag(widget, start, end, steps=8):
        send_mouse(widget, QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
        send_mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
        for i in range(1, steps + 1):
            p = QPoint(start.x() + (end.x() - start.x()) * i // steps, start.y() + (end.y() - start.y()) * i // steps)
            send_mouse(widget, QEvent.MouseMove, p, Qt.NoButton, Qt.LeftButton)
        send_mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)

    def key(widget, k):
        if QTest is not None:
            QTest.keyClick(widget, k)
        else:
            QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, k, Qt.NoModifier))
            QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyRelease, k, Qt.NoModifier))
        QApplication.processEvents()

    def tool_action(tool_id):
        for w in gt.widgets_of("ViewerToolButton"):
            if not w.isVisible():
                continue
            for a in w.actions():
                data = a.data()
                if data and len(data) > 1 and str(data[1]) == tool_id:
                    return a
        return None

    def main_window():
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                return w
        return None

    def edit_action(prefix):
        win = main_window()
        if win is None or win.menuBar() is None:
            return None
        for act in win.menuBar().actions():
            menu = gt.menu_of(act)
            if menu is not None and act.text().lower().startswith("edit"):
                for a in menu.actions():
                    if a.text().lower().startswith(prefix):
                        return a
        return None

    def shapes(roto):
        """The Beziers the tool made, depth first."""
        found = []
        ctx = roto.getRotoContext()

        def walk(layer):
            for child in layer.getChildren():
                # Layer.getChildren yields plain ItemBase wrappers; getItemByName returns the Layer/BezierCurve.
                item = ctx.getItemByName(child.getScriptName()) or child
                if hasattr(item, "getChildren"):
                    walk(item)
                elif item.getScriptName().startswith("Object"):
                    found.append(item)

        walk(ctx.getBaseLayer())
        return found

    def polygon(curve, time, samples=16):
        n = curve.getNumControlPoints()
        pts = [curve.getControlPointPosition(i, time) for i in range(n)]
        out = []
        for i in range(n):
            x0, y0, _, _, rx, ry = pts[i]
            x1, y1, lx, ly, _, _ = pts[(i + 1) % n]
            for s in range(samples):
                t = s / float(samples)
                m = 1.0 - t
                out.append((m * m * m * x0 + 3 * m * m * t * rx + 3 * m * t * t * lx + t * t * t * x1,
                            m * m * m * y0 + 3 * m * m * t * ry + 3 * m * t * t * ly + t * t * t * y1))
        return out

    def contains(poly, x, y):
        hit = False
        j = len(poly) - 1
        for i in range(len(poly)):
            xi, yi = poly[i]
            xj, yj = poly[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                hit = not hit
            j = i
        return hit

    def matte_iou(curves, time):
        """The IoU of the shapes' matte (holes subtracted) and the object, sampled at pixel centres."""
        stencil = NatronEngine.Natron.MergingFunctionEnum.eMergeStencil
        outlines = [polygon(c, time) for c in curves if c.getCompositingOperator() != stencil]
        holes = [polygon(c, time) for c in curves if c.getCompositingOperator() == stencil]
        both = either = 0
        for y in range(IMAGE_H):
            for x in range(IMAGE_W):
                px, py = x + 0.5, y + 0.5
                a = any(contains(p, px, py) for p in outlines) and not any(contains(p, px, py) for p in holes)
                b = truth(px, py)
                both += a and b
                either += a or b
        return both / float(max(1, either))

    def point_count(curves):
        return sum(c.getNumControlPoints() for c in curves)

    def pick_model(roto, model_id):
        """Selects the model by its option ID; getOptions() lists IDs, not the menu labels."""
        param = roto.getParam("model")
        options = [str(o) for o in param.getOptions()]
        index = options.index(model_id) if model_id in options else -1
        if index >= 0:
            param.setValue(index)
            QApplication.processEvents()
        return index >= 0

    def steps():
        gt.section("object-select")
        # A fresh profile has AI off: the tool then only shows "AI is disabled" and adds no prompt.
        ai_enabled = NatronEngine.natron.getSettings().getParam("aiEnabled")
        require(ai_enabled is not None, "Settings has a parameter named aiEnabled")
        ai_enabled.setValue(True)
        require(ai_enabled.getValue(), "AI is enabled in Preferences")
        app = NatronEngine.natron.getActiveInstance()
        viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
        require(viewer is not None, "a Viewer node exists")
        ok = yield ("until", lambda: gt.viewer_up(), STARTUP_TIMEOUT_S)
        require(ok, "the main window and viewer are on screen")

        for model_id, arch, prefix in ((SEGNEXT_ID, "segnext", "seg-segnext"), (SAM_ID, "sam2", "seg-sam")):
            report("NOTE installed %s under %s" % (model_id, install(model_id, arch, prefix)))
        still = os.path.join(work, "object.png")
        require(write_still(still), "wrote the still " + still)

        read = app.createReader(still)
        require(read is not None, "created a Read of the still")
        read.setPosition(0, 0)
        gt.close_all_panels()

        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        roto = app.createNode("fr.inria.built-in.RotoPaint", -1, None, props)
        require(roto is not None, "created a RotoPaint node", gt.plugin_problem("fr.inria.built-in.RotoPaint"))
        roto.setPosition(0, 200)
        require(roto.getParam("model") is not None, "the Roto node has a model parameter")
        for name in ("objectThreshold", "objectDensity", "objectMinArea", "objectHoles", "objectSmooth",
                     "objectFeather", "objectInferenceResolution", "objectClearPrompts", "downloadModel"):
            check(roto.getParam(name) is not None, "the Roto node has a %s parameter" % name)
        require(pick_model(roto, SEGNEXT_ID), "picked the Test SegNext model")

        yield ("sleep", SETTLE_MS)
        require(roto.connectInput(0, read), "the Read feeds the Roto node's Source")
        if viewer.getInput(0) is not None:
            viewer.disconnectInput(0)
        require(viewer.connectInput(0, roto), "the Viewer shows the Roto node")
        app.getProjectParam("frameRange").setValue(1, 0)
        app.getProjectParam("frameRange").setValue(1, 1)
        view = untone_viewer()
        require(view is not None, "the viewer shows the image without tone mapping (Un-tone-mapped or Raw view)")
        yield ("sleep", RENDER_MS)

        ok = yield ("until", lambda: tool_action("ObjectSelectTool") is not None, POLL_TIMEOUT_S)
        require(ok, "the viewer toolbar offers the Object Select tool (action data ObjectSelectTool)")
        action = tool_action("ObjectSelectTool")
        check(action.isEnabled(), "the tool is enabled with the Source connected")
        action.trigger()
        yield ("sleep", SETTLE_MS)

        gl = visible_gl()
        require(gl is not None, "the viewer is visible")
        win = main_window()
        check(win is not None and win.frameGeometry().width() <= 1920 and win.frameGeometry().height() <= 1080,
              "the main window fits 1920x1080")
        ok = yield ("until", lambda: bright_box(gl) is not None, POLL_TIMEOUT_S)
        box = bright_box(gl)
        if not ok or box is None:
            gt.shot("m205-object-no-still.png")
        require(ok and box is not None, "the still is on screen (%s view)" % view)
        gt.shot("m205-object-tool.png")

        # Viewer pixels per still pixel, from the bright box (the object's outer edges).
        sx = (box[2] - box[0]) / float(OBJECT[2] - OBJECT[0])
        sy = (box[3] - box[1]) / float(OBJECT[3] - OBJECT[1])

        def at(px, py):
            return QPoint(int(round(box[0] + (px - OBJECT[0]) * sx)), int(round(box[1] + (py - OBJECT[1]) * sy)))

        click(gl, at(30, 48))
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        check(ok, "a click previews the object's outline (%d outline pixels)" % outline_pixels(gl, box))
        gt.shot("m205-object-preview.png")

        # A box needs a SAM model; the click above belongs to the SegNext selection, cleared first.
        key(gl, Qt.Key_Escape)
        ok = yield ("until", lambda: outline_pixels(gl, box) < 10, POLL_TIMEOUT_S)
        check(ok, "Escape clears the preview")
        if check(pick_model(roto, SAM_ID), "picked the Test SAM model"):
            yield ("sleep", SETTLE_MS)
            drag(gl, at(10, 4), at(118, 92))
            ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
            check(ok, "a box prompt previews the object's outline with the SAM model")
            gt.shot("m205-object-box.png")
            key(gl, Qt.Key_Escape)
            ok = yield ("until", lambda: outline_pixels(gl, box) < 10, POLL_TIMEOUT_S)
            check(ok, "Escape clears the box preview")
            pick_model(roto, SEGNEXT_ID)
            yield ("sleep", SETTLE_MS)

        check(not shapes(roto), "no shape exists before the commit")
        click(gl, at(30, 48))
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        require(ok, "the click previews the outline again")
        key(gl, Qt.Key_Return)
        ok = yield ("until", lambda: len(shapes(roto)) >= 2, POLL_TIMEOUT_S)
        made = shapes(roto)
        require(ok, "Return commits the outline and its hole as two shapes (got %d)" % len(made))
        time = 1.0
        for curve in made:
            keys = list(curve.getKeyframes())
            check(time in keys, "%s is keyed on frame 1 (keys %r)" % (curve.getScriptName(), keys))
            check(curve.isCurveFinished(), "%s is closed" % curve.getScriptName())
        stencil = NatronEngine.Natron.MergingFunctionEnum.eMergeStencil
        check(sum(1 for c in made if c.getCompositingOperator() == stencil) == 1, "the hole is a Stencil shape")
        iou = matte_iou(made, time)
        check(iou > 0.9, "the shapes' matte matches the object (IoU %.3f)" % iou)
        yield ("sleep", RENDER_MS)
        gt.shot("m205-object-committed.png")

        density = roto.getParam("objectDensity")
        require(density is not None, "the Roto node has an objectDensity parameter")
        default_density = density.getValue()
        first_points = point_count(made)

        undo = edit_action("undo")
        require(undo is not None and undo.isEnabled(), "the Edit menu's Undo is enabled")
        undo.trigger()
        ok = yield ("until", lambda: not shapes(roto), POLL_TIMEOUT_S)
        check(ok, "one undo removes every committed shape")
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        check(ok, "the undo brings the selection's outline back (%d outline pixels)" % outline_pixels(gl, box))
        yield ("sleep", RENDER_MS)
        gt.shot("m205-object-undone.png")

        # The selection is live again: Backspace takes its click away, and a new click adds one.
        key(gl, Qt.Key_Backspace)
        ok = yield ("until", lambda: outline_pixels(gl, box) < 10, POLL_TIMEOUT_S)
        check(ok, "Backspace removes the click the undo brought back")
        click(gl, at(30, 48))
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        check(ok, "a new click previews the outline again")

        redo = edit_action("redo")
        if check(redo is not None and redo.isEnabled(), "the Edit menu's Redo is enabled"):
            redo.trigger()
            ok = yield ("until", lambda: len(shapes(roto)) == len(made), POLL_TIMEOUT_S)
            check(ok, "redo brings the first commit's shapes back")
            check(point_count(shapes(roto)) == first_points,
                  "the redone shapes have the first commit's %d points (got %d)" % (first_points, point_count(shapes(roto))))
            undo = edit_action("undo")
            require(undo is not None and undo.isEnabled(), "Undo is enabled after the redo")
            undo.trigger()
            ok = yield ("until", lambda: not shapes(roto), POLL_TIMEOUT_S)
            check(ok, "undoing the redone commit removes its shapes")
            ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
            check(ok, "and brings the selection back")

        # Density re-traces the selection before the commit, and is kept for the next one.
        for value, label, more in ((90.0, "dense", True), (10.0, "sparse", False)):
            if shapes(roto):
                undo = edit_action("undo")
                require(undo is not None and undo.isEnabled(), "Undo is enabled before the %s commit" % label)
                undo.trigger()
                ok = yield ("until", lambda: not shapes(roto), POLL_TIMEOUT_S)
                require(ok, "one undo removes the previous commit's shapes")
            ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
            require(ok, "the selection is shown before the %s commit" % label)
            density.setValue(value)
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)
            check(outline_pixels(gl, box) > 40, "the outline is still shown at Object Density %g" % value)
            if more:
                gt.shot("m205-object-dense.png")
            key(gl, Qt.Key_Return)
            ok = yield ("until", lambda: len(shapes(roto)) >= 2, POLL_TIMEOUT_S)
            require(ok, "Return commits the %s selection" % label)
            n = point_count(shapes(roto))
            if more:
                check(n > first_points, "Object Density %g gives more points than %g (%d against %d)" % (value, default_density, n, first_points))
            else:
                check(n < first_points, "Object Density %g gives fewer points than %g (%d against %d)" % (value, default_density, n, first_points))
            check(density.getValue() == value, "the density stays at %g for the next shape" % value)
            stencil_count = sum(1 for c in shapes(roto) if c.getCompositingOperator() == stencil)
            check(stencil_count == 1, "the %s commit still makes one Stencil shape" % label)

        undo = edit_action("undo")
        require(undo is not None and undo.isEnabled(), "Undo is enabled after the last commit")
        undo.trigger()
        ok = yield ("until", lambda: not shapes(roto), POLL_TIMEOUT_S)
        check(ok, "a single undo removes every shape of the last commit")
        density.setValue(default_density)
        QApplication.processEvents()

        # Inference Resolution: the click stays where it is and a new inference runs on the
        # smaller image, whose mask comes back at full resolution.
        resolution = roto.getParam("objectInferenceResolution")
        require(resolution is not None, "the Roto node has an objectInferenceResolution parameter")
        default_resolution = resolution.getValue()
        check(default_resolution == 4, "Object Inference Resolution defaults to 2K (index %d)" % default_resolution)
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        require(ok, "the selection is back before the resolution change")
        resolution.setValue(0)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        require(ok, "the outline is shown at Full")
        key(gl, Qt.Key_Return)
        ok = yield ("until", lambda: len(shapes(roto)) >= 1, POLL_TIMEOUT_S)
        require(ok, "Return commits the selection at Full")
        full_points = sorted(tuple(round(v, 3) for v in c.getControlPointPosition(i, time)[:2])
                             for c in shapes(roto) for i in range(c.getNumControlPoints()))
        undo = edit_action("undo")
        require(undo is not None and undo.isEnabled(), "Undo is enabled after the Full commit")
        undo.trigger()
        ok = yield ("until", lambda: not shapes(roto), POLL_TIMEOUT_S)
        require(ok, "undo removes the Full commit's shapes")
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        require(ok, "and brings the selection back")

        resolution.setValue(2)
        QApplication.processEvents()
        yield ("sleep", RENDER_MS)
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        check(ok, "the outline is shown at 25%% without a new click (%d outline pixels)" % outline_pixels(gl, box))
        gt.shot("m205-object-quarter.png")
        key(gl, Qt.Key_Return)
        ok = yield ("until", lambda: len(shapes(roto)) >= 1, POLL_TIMEOUT_S)
        require(ok, "Return commits the selection at 25%")
        quarter = shapes(roto)
        quarter_points = sorted(tuple(round(v, 3) for v in c.getControlPointPosition(i, time)[:2])
                                for c in quarter for i in range(c.getNumControlPoints()))
        check(quarter_points != full_points,
              "a new inference ran at 25%%: its shapes differ from Full's (%d against %d points)" % (len(quarter_points), len(full_points)))
        iou = matte_iou(quarter, time)
        check(iou > 0.8, "the 25%% shapes still match the object at full resolution, so the click stayed in place (IoU %.3f)" % iou)

        undo = edit_action("undo")
        require(undo is not None and undo.isEnabled(), "Undo is enabled after the 25% commit")
        undo.trigger()
        ok = yield ("until", lambda: not shapes(roto), POLL_TIMEOUT_S)
        check(ok, "one undo removes the 25% commit")
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        check(ok, "and brings the selection back")

        # The undo step keeps the mask at the inference resolution, so the same shapes come back.
        key(gl, Qt.Key_Return)
        ok = yield ("until", lambda: len(shapes(roto)) >= 1, POLL_TIMEOUT_S)
        if check(ok, "Return commits the selection the undo brought back"):
            again_points = sorted(tuple(round(v, 3) for v in c.getControlPointPosition(i, time)[:2])
                                  for c in shapes(roto) for i in range(c.getNumControlPoints()))
            check(again_points == quarter_points,
                  "the commit after the undo has the 25%% commit's shapes (%d against %d points)" % (len(again_points), len(quarter_points)))
            undo = edit_action("undo")
            require(undo is not None and undo.isEnabled(), "Undo is enabled after committing again")
            undo.trigger()
            ok = yield ("until", lambda: not shapes(roto), POLL_TIMEOUT_S)
            check(ok, "undo removes the commit made after the undo")
            ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
            check(ok, "and brings the selection back again")
        key(gl, Qt.Key_Backspace)
        ok = yield ("until", lambda: outline_pixels(gl, box) < 10, POLL_TIMEOUT_S)
        check(ok, "the resolution changes kept a single click: one Backspace clears the selection")
        resolution.setValue(default_resolution)
        QApplication.processEvents()

        # Leaving the tool frees its encodings and drops the selection; picking it again re-encodes.
        click(gl, at(30, 48))
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        require(ok, "a click previews the outline before the tool is left")
        other = tool_action("SelectAllTool")
        require(other is not None, "the viewer toolbar offers the Select All tool")
        other.trigger()
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: outline_pixels(gl, box) < 10, POLL_TIMEOUT_S)
        check(ok, "leaving the tool drops the selection's outline")
        tool_action("ObjectSelectTool").trigger()
        yield ("sleep", SETTLE_MS)
        check(outline_pixels(gl, box) < 10, "picking the tool again brings no old selection back")
        click(gl, at(30, 48))
        ok = yield ("until", lambda: outline_pixels(gl, box) > 40, POLL_TIMEOUT_S)
        check(ok, "a click after picking the tool again encodes the Source again and previews the outline")
        gt.shot("m205-object-reencoded.png")

        # An encoding that fails must not leave another image's outline up to be committed.
        filename = read.getParam("filename")
        threshold = roto.getParam("objectThreshold")
        require(filename is not None and threshold is not None, "the Read has a filename and the Roto an objectThreshold")
        still_name = filename.getValue()
        default_threshold = threshold.getValue()
        filename.setValue(os.path.join(work, "missing.png"))
        QApplication.processEvents()
        threshold.setValue(default_threshold + 0.05)
        QApplication.processEvents()
        ok = yield ("until", lambda: outline_pixels(gl, box) < 10, POLL_TIMEOUT_S)
        check(ok, "when the Source cannot be encoded the outline goes (%d outline pixels)" % outline_pixels(gl, box))
        gt.shot("m205-object-failed.png")
        key(gl, Qt.Key_Return)
        yield ("sleep", SETTLE_MS)
        check(not shapes(roto), "Return commits nothing after a failed encoding")
        filename.setValue(still_name)
        threshold.setValue(default_threshold)
        QApplication.processEvents()

    gt.run(steps())
except Exception:
    import traceback
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + traceback.format_exc() + "\n")
    os._exit(1)
