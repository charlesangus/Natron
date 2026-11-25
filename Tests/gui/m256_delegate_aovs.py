"""Delegate AOV selection: three Cycles passes ticked plus one custom primvar var give four extra
layers in the viewer and in one EXR, and a MoonRay tick in the same node adds no warning to the
Cycles render. Each tick is found by its script name, cyclesAovAo for Cycles' cycles:ao.

Graph: USDSphere into USDProjectUV (Perspective, through USDCamera1: the st primvar it authors is a
ramp across the sphere), merged with USDCamera1 and a USDLight, into USDRenderSettings (Default
Renderer Cycles) into USDRender, which feeds Viewer1 and a Write. In a 1920x1080 window the script:
  - ticks Cycles' diffuse_direct, ao and mist, MoonRay's albedo (or its first AOV), and authors the
    st primvar as a Custom AOVs row from Python;
  - clicks the table's Add... button, checks Python reads the added row back, and removes it again
    from Python, since it is a second st;
  - waits for the viewer's layer menu to list the four layers with no reconnection;
  - checks USDRender, USDRenderSettings and the viewer carry no warning that names MoonRay;
  - picks each of the four layers, and the beauty, in the viewer's Layer menu (found by its tooltip),
    waits until the viewer's layer knob names it and the image is current and steady, and saves one
    screenshot each (m256-<layer>.png and m256-beauty.png, the viewer only);
  - compares the format's area of those shots: each layer differs from the beauty and from every
    other layer by a mean absolute difference over the lit pixels well above render noise, ao and
    mist are grey, ao is mostly bright, mist is not flat, and st's red (U) rises from left to right
    with no blue (W);
  - renders a Write to one EXR and lists its channels with oiiotool: each layer's channels are there.
A build without Cycles, or without a MoonRay checklist, reports SKIP for what it lacks.

Run through Tests/gui/run-gui-test.sh, at 1920x1080. Screenshots, the EXR and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
RENDER_TIMEOUT_S = 60.0
FORMAT_W = 320
FORMAT_H = 240
STEADY_S = 0.75
LAYER_MENU_TOOLTIP = "The layer that the Viewer node will fetch"
# On-screen 8-bit display values: a pixel is lit when its channels sum above LIT_SUM, and two shots
# differ when their mean absolute difference over pixels lit in either exceeds MIN_LAYER_DIFF.
LIT_SUM = 12
MIN_LAYER_DIFF = 0.03
MAX_GREY_SPREAD = 0.02
STARTUP_TIMEOUT_S = 60.0
CYCLES_ID = "HdCyclesPlugin"
CYCLES_PASSES = ("diffuse_direct", "ao", "mist")
MOONRAY_PREFERRED = "albedo"
# Cycles' pass layers and the custom row's own: the primvar's layer is named after it.
PRIMVAR_LAYER = "st"
PRIMVAR_ROW = ["st", "primvar", "", ""]

try:
    from PySide6.QtCore import Q_ARG, QMetaObject, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QTabWidget, QAbstractButton, QAbstractItemView

    from guitest import (OUT, check, class_name, close_all_panels, finish, plugin_problem, report, require, run, shot,
                         viewer_messages, widgets_of)

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getLabel(), name))
        return p

    def connect_free(node, source):
        for i in range(node.getMaxInputCount()):
            if node.getInput(i) is None:
                return node.connectInput(i, source)
        return False

    def input_named(node, word):
        for i in range(node.getMaxInputCount()):
            if word in str(node.getInputLabel(i)).lower():
                return i
        return None

    def choose_containing(p, word):
        options = [str(o) for o in p.getOptions()]
        for i, o in enumerate(options):
            if word.lower() in o.lower():
                p.set(i)
                return True
        return False

    app.addFormat("m256Aovs %dx%d" % (FORMAT_W, FORMAT_H))
    project_format = app.getProjectParam("outputFormat")
    format_options = [str(o) for o in project_format.getOptions()]
    project_format.setValue(next((i for i, o in enumerate(format_options) if "m256Aovs" in o), len(format_options) - 1))

    sphere = make("USDSphere", 0, 0)
    camera = make("USDCamera", 200, 0)
    project_uv = make("USDProjectUV", 0, 100)
    light = make("USDLight", 400, 0)
    merge = make("USDMerge", 0, 200)
    settings = make("USDRenderSettings", 0, 300)
    render = make("USDRender", 0, 400)
    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("Viewer")
    require(viewer is not None, "a Viewer exists")
    viewer.setPosition(200, 500)
    write = app.createWriter(os.path.join(OUT, "m256_aovs.exr"))
    require(write is not None, "create a Write")
    write.setPosition(0, 500)
    channels_param = write.getParam("channels")
    require(channels_param is not None, "the Write has a channels parameter")
    channels_param.setAll()
    exr_path = os.path.join(OUT, "m256_aovs.exr")
    if os.path.exists(exr_path):
        os.remove(exr_path)

    param(camera, "translate").set(0.0, 0.0, 5.0)
    require(choose_containing(param(project_uv, "projection"), "perspective"), "USDProjectUV offers Perspective")
    light_type = light.getParam("lightType")
    if light_type is not None:
        choose_containing(light_type, "sphere")
    light_translate = light.getParam("translate")
    if light_translate is not None:
        light_translate.set(-3.0, 3.0, 4.0)
    for name in ("intensity",):
        if light.getParam(name) is not None:
            light.getParam(name).set(300.0)

    camera_input = input_named(project_uv, "camera")
    require(camera_input is not None, "USDProjectUV has a camera input")
    require(project_uv.connectInput(0, sphere), "connect USDSphere1 to USDProjectUV1")
    require(project_uv.connectInput(camera_input, camera), "connect USDCamera1 to USDProjectUV1's camera input")
    require(connect_free(merge, project_uv), "connect USDProjectUV1 to USDMerge1")
    require(connect_free(merge, camera), "connect USDCamera1 to USDMerge1")
    require(connect_free(merge, light), "connect USDLight1 to USDMerge1")
    require(settings.connectInput(0, merge), "connect USDMerge1 to USDRenderSettings1")
    require(render.connectInput(0, settings), "connect USDRenderSettings1 to USDRender1")
    viewer.disconnectInput(0)
    require(viewer.connectInput(0, render), "connect Viewer1 to USDRender1")
    require(write.connectInput(0, render), "connect the Write to USDRender1")

    # A child widget's wrapper dies with its parent's; widgets found once are held.
    _held = []

    def hold(w):
        if w is not None:
            _held.append(w)
        return w

    def main_gui():
        guis = [w for w in widgets_of("Gui") if w.isVisible()]
        return hold(guis[0]) if guis else None

    def graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def activate_main_window():
        gui = main_gui()
        if gui is not None:
            gui.activateWindow()
            gui.raise_()
        QApplication.processEvents()

    def node_panel(node):
        label = node.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            if not panel.isVisible():
                continue
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return hold(panel)
                except RuntimeError:
                    continue
            if label in (panel.windowTitle() or ""):
                return hold(panel)
        return None

    def open_panel(node):
        for attempt in range(2):
            close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            activate_main_window()
            app.selectNode(node, True)
            QApplication.processEvents()
            gv = graph_view()
            if gv is not None:
                QMetaObject.invokeMethod(gv, "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: node_panel(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
        return False

    def show_tab(node, name):
        panel = node_panel(node)
        tabs = [t for t in panel.findChildren(QTabWidget) if t.count() > 0] if panel is not None else []
        if not tabs:
            return False
        hold(tabs[0])
        for i in range(tabs[0].count()):
            if tabs[0].tabText(i).replace("&", "").lower() == name.lower():
                tabs[0].setCurrentIndex(i)
                QApplication.processEvents()
                return True
        return False

    def add_custom_row():
        panel = node_panel(settings)
        buttons = []
        for w in (panel.findChildren(QAbstractButton) if panel is not None else []):
            try:
                if w.isVisible() and "Add a new value" in (w.toolTip() or ""):
                    buttons.append(w)
            except RuntimeError:
                continue
        if len(buttons) != 1:
            return len(buttons)
        QTest.mouseClick(hold(buttons[0]), Qt.LeftButton)
        QApplication.processEvents()
        return 1

    def custom_rows():
        panel = node_panel(settings)
        for tv in (panel.findChildren(QAbstractItemView) if panel is not None else []):
            try:
                if tv.isVisible() and class_name(tv) == "TableView" and tv.model() is not None:
                    return tv.model().rowCount()
            except RuntimeError:
                continue
        return None

    def ticks(prefix):
        """(script name, param) of every checklist tick of a renderer, by script-name prefix."""
        found = []
        for p in settings.getParams():
            name = str(p.getScriptName())
            if name.startswith(prefix + "Aov") and hasattr(p, "get") and not hasattr(p, "getOptions"):
                found.append((name, p))
        return found

    def find_tick(prefix, aov):
        """The tick of an AOV, whose script name is the prefix, Aov and the AOV's name without the renderer's namespace."""
        want = prefix + "Aov" + "".join(w[:1].upper() + w[1:] for w in re.split(r"[^A-Za-z0-9]+", aov) if w)
        for name, p in ticks(prefix):
            if name == want:
                return name, p
        return None, None

    def layer_combo():
        """The viewer's Layer menu, told from its Alpha Channel menu by its tooltip."""
        for c in widgets_of("ComboBox"):
            try:
                if c.isVisible() and LAYER_MENU_TOOLTIP in (c.toolTip() or ""):
                    return hold(c), [str(i) for i in (c.property("natronItems") or [])]
            except RuntimeError:
                continue
        return None, []

    def layer_index(items, name):
        return next((i for i, t in enumerate(items) if t.lower() == name.lower()), None)

    def beauty_index(items):
        return next((i for i, t in enumerate(items) if t.lower() in ("rgba", "rgb", "color.rgba", "color.rgb")), None)

    def layer_listed(name):
        _, items = layer_combo()
        return layer_index(items, name) is not None

    def viewer_layer():
        p = viewer.getParam("layer")
        return str(p.getLayer()) if p is not None and hasattr(p, "getLayer") else None

    def viewer_gl():
        views = [w for w in widgets_of("ViewerGL") if w.isVisible()]
        return hold(views[0]) if views else None

    def viewer_crop():
        v = viewer_gl()
        if v is None:
            return None
        QApplication.processEvents()
        origin = v.mapToGlobal(v.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(origin.x(), origin.y(), v.width(), v.height())

    def format_crop():
        """The project format's area of the viewer, from the screen, 3 px inside its frame."""
        v = viewer_gl()
        mapping = v.property("natronCanonicalToWidget") if v is not None else None
        if not mapping or len(mapping) != 4:
            return None
        ox, oy, sx, sy = [float(m) for m in mapping]
        xs = sorted((ox, ox + FORMAT_W * sx))
        ys = sorted((oy, oy + FORMAT_H * sy))
        x0, y0 = int(round(xs[0])) + 3, int(round(ys[0])) + 3
        w, h = int(round(xs[1])) - 3 - x0, int(round(ys[1])) - 3 - y0
        if w < 16 or h < 16:
            return None
        QApplication.processEvents()
        origin = v.mapToGlobal(v.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(origin.x() + x0, origin.y() + y0, w, h)

    def pixels(image, step=2):
        """(x fraction, r, g, b) of every step-th pixel, rows top to bottom."""
        out = []
        width = max(1, image.width() - 1)
        for y in range(0, image.height(), step):
            for x in range(0, image.width(), step):
                p = image.pixel(x, y)
                out.append((x / float(width), (p >> 16) & 255, (p >> 8) & 255, p & 255))
        return out

    def lit(px):
        return px[1] + px[2] + px[3] > LIT_SUM

    def mean_difference(a, b):
        """Mean absolute channel difference, 0..1, over the pixels lit in either; None if sizes differ."""
        if len(a) != len(b):
            return None
        total, count = 0, 0
        for pa, pb in zip(a, b):
            if lit(pa) or lit(pb):
                total += abs(pa[1] - pb[1]) + abs(pa[2] - pb[2]) + abs(pa[3] - pb[3])
                count += 3
        return total / (255.0 * count) if count else 0.0

    def grey_spread(px):
        """Mean of max - min channel over the lit pixels, 0..1."""
        shown = [p for p in px if lit(p)]
        if not shown:
            return None
        return sum(max(p[1:]) - min(p[1:]) for p in shown) / (255.0 * len(shown))

    def steady_viewer(name):
        """A predicate true once the viewer's layer knob is name, its image is current, and the
        format's area has not changed for STEADY_S."""
        seen = []
        since = [0.0]

        def settled():
            if viewer_layer() != name:
                return False
            v = viewer_gl()
            if v is None or bool(v.property("natronViewerStale")):
                del seen[:]
                return False
            image = format_crop()
            if image is None:
                return False
            signature = tuple(image.pixel(x, y) for y in range(0, image.height(), 4) for x in range(0, image.width(), 4))
            if not seen or seen[-1] != signature:
                del seen[:]
                seen.append(signature)
                since[0] = time.time()
            return time.time() - since[0] >= STEADY_S

        return settled

    def mentions_moonray(text):
        return "moonray" in (text or "").lower()

    def exr_channels(path):
        try:
            out = subprocess.run(["oiiotool", "--info", "-v", path], capture_output=True, text=True, timeout=60).stdout
        except (OSError, subprocess.SubprocessError):
            return None
        match = re.search(r"channel list:\s*(.*)", out)
        return [c.strip() for c in match.group(1).split(",")] if match else []

    def steps():
        defaults = settings.getParam("defaultRenderer")
        require(defaults is not None and choose_containing(defaults, "cycles"), "the Default Renderer menu offers Cycles")
        cam = settings.getParam("camera")
        if cam is not None:
            report("NOTE camera options %s" % [str(o) for o in cam.getOptions()])
            options = [str(o) for o in cam.getOptions()]
            real = [i for i, o in enumerate(options) if "camera" in o.lower() or "cam" in o.lower()]
            if real:
                cam.set(real[0])

        cycles_ticks = ticks("cycles")
        report("NOTE Cycles ticks %s" % [n for n, _ in cycles_ticks])
        moonray_ticks = ticks("moonray")
        report("NOTE MoonRay ticks %s" % [n for n, _ in moonray_ticks])
        for aov in CYCLES_PASSES:
            name, p = find_tick("cycles", aov)
            require(p is not None, "the Cycles checklist offers %s" % aov)
            p.set(True)
            report("NOTE %s ticked: %r" % (name, p.get()))
        moonray_pick = find_tick("moonray", MOONRAY_PREFERRED)[1] or (moonray_ticks[0][1] if moonray_ticks else None)
        if moonray_pick is None:
            report("SKIP the MoonRay checklist is not in this build")
        else:
            moonray_pick.set(True)
        custom = param(settings, "customAovs")
        custom.setTable([PRIMVAR_ROW])
        check(custom.getTable() == [PRIMVAR_ROW], "Python authors the st row of Custom AOVs (%r)" % custom.getTable())

        ok = yield from open_panel(settings)
        require(ok, "USDRenderSettings1's panel opens")
        require(show_tab(settings, "AOVs"), "the panel has an AOVs tab")
        yield ("sleep", 300)
        added = add_custom_row()
        check(added == 1, "the Custom AOVs table's Add... button is found once (%d)" % added)
        yield ("sleep", 1500)
        shot("m256-settings.png")
        rows = custom_rows()
        check(rows == 2, "Add... put a second row in the Custom AOVs table (%r)" % rows)
        table = custom.getTable()
        check(table == [PRIMVAR_ROW, PRIMVAR_ROW], "Python reads back the row Add... put in Custom AOVs (%r)" % table)
        custom.removeRow(1)
        yield ("sleep", 500)
        check(custom.getTable() == [PRIMVAR_ROW], "Python removes the added row (%r)" % custom.getTable())
        rows = custom_rows()
        check(rows == 1, "the Custom AOVs table shows the row Python left (%r)" % rows)
        for node, names in ((render, ("renderer", "settingsPrim")), (settings, ("defaultRenderer", "makeActive"))):
            for name in names:
                p = node.getParam(name)
                if p is not None:
                    report("NOTE %s.%s = %s of %s" % (node.getScriptName(), name, p.getOption(p.get()) if hasattr(p, "getOption") else p.get(),
                                                  [str(o) for o in p.getOptions()] if hasattr(p, "getOptions") else ""))
        wanted = list(CYCLES_PASSES) + [PRIMVAR_LAYER]
        ok = yield ("until", lambda: all(layer_listed(n) for n in wanted), RENDER_TIMEOUT_S)
        _, items = layer_combo()
        check(ok, "the viewer's layer menu lists the new layers with no reconnection (%r)" % items)
        if not ok:
            viewer.disconnectInput(0)
            yield ("sleep", 500)
            viewer.connectInput(0, render)
            ok = yield ("until", lambda: all(layer_listed(n) for n in wanted), RENDER_TIMEOUT_S)
            report("NOTE reconnecting the viewer: layers listed %s" % ok)
        _, items = layer_combo()
        report("NOTE viewer layer menu %s" % items)
        for n in wanted:
            check(layer_listed(n), "the viewer's layer menu lists %s" % n)
        yield ("sleep", 1500)

        for label, text in (("USDRender1", render.getPersistentMessage()),
                            ("USDRenderSettings1", settings.getPersistentMessage())):
            report("NOTE %s message: %r" % (label, text))
            check(not mentions_moonray(text), "%s carries no warning naming MoonRay" % label)
        messages = viewer_messages()
        report("NOTE viewer messages: %r" % messages)
        check(not any(mentions_moonray(m) for m in messages), "the viewer shows no warning naming MoonRay")

        shown = {}
        for n in ["beauty"] + wanted:
            combo, items = layer_combo()
            index = beauty_index(items) if n == "beauty" else layer_index(items, n)
            if combo is None or index is None:
                check(False, "the viewer's Layer menu has %s to show (%r)" % (n, items))
                continue
            target = items[index]
            QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
            QApplication.processEvents()
            ok = yield ("until", steady_viewer(target), RENDER_TIMEOUT_S)
            check(ok, "picking %s in the Layer menu sets the viewer's layer knob and the viewer settles on it "
                      "(knob %r, stale %r)" % (target, viewer_layer(), viewer_gl() and viewer_gl().property("natronViewerStale")))
            image = viewer_crop()
            area = format_crop()
            if image is None or area is None:
                check(False, "the viewer and its format area are on screen for %s" % n)
                continue
            path = os.path.join(OUT, "m256-%s.png" % n)
            image.save(path)
            report("SHOT " + path)
            shown[n] = pixels(area)
        names = list(shown)
        for i, a in enumerate(names):
            for b in names[i + 1:]:
                d = mean_difference(shown[a], shown[b])
                check(d is not None and d > MIN_LAYER_DIFF,
                      "the viewer shows %s and %s differently (mean difference %s over lit pixels, wanted > %.2f)"
                      % (a, b, "n/a" if d is None else "%.3f" % d, MIN_LAYER_DIFF))
        for n in ("ao", "mist"):
            if n in shown:
                spread = grey_spread(shown[n])
                check(spread is not None and spread < MAX_GREY_SPREAD,
                      "the viewer shows %s grey (mean channel spread %r, wanted < %.2f)" % (n, spread, MAX_GREY_SPREAD))
        if "ao" in shown:
            lit_px = [p for p in shown["ao"] if lit(p)]
            bright = sum(1 for p in lit_px if min(p[1:]) > 0.7 * 255) / float(len(lit_px)) if lit_px else 0.0
            check(bright > 0.5, "most of ao's lit pixels are bright, the sphere being unoccluded (%.2f of them)" % bright)
        if "mist" in shown:
            values = [p[1] for p in shown["mist"]]
            span = (max(values) - min(values)) / 255.0 if values else 0.0
            check(span > 0.2, "mist is not flat: its values span %.2f of the range" % span)
        if PRIMVAR_LAYER in shown:
            lit_px = [p for p in shown[PRIMVAR_LAYER] if lit(p)]
            left = [p[1] for p in lit_px if p[0] < 1.0 / 3.0]
            right = [p[1] for p in lit_px if p[0] > 2.0 / 3.0]
            left_mean = sum(left) / (255.0 * len(left)) if left else None
            right_mean = sum(right) / (255.0 * len(right)) if right else None
            check(left_mean is not None and right_mean is not None and right_mean > left_mean + 0.08,
                  "st's red (U) rises from left to right (left third %r, right third %r)" % (left_mean, right_mean))
            blue = sum(p[3] for p in lit_px) / (255.0 * len(lit_px)) if lit_px else None
            check(blue is not None and blue < 0.02, "st shows no blue, W being zero (mean %r)" % blue)

        report("NOTE Write params %s" % [str(p.getScriptName()) for p in write.getParams()])
        app.renderBlocking(write, 1, 1)
        wrote = os.path.isfile(exr_path)
        check(wrote, "the Write wrote " + exr_path)
        if wrote:
            channels = exr_channels(exr_path)
            report("NOTE EXR channels %s" % channels)
            check(channels is not None, "oiiotool lists the EXR's channels")
            for n in wanted:
                check(any(c.lower().startswith(n.lower() + ".") or c.lower().startswith(n.lower() + "_")
                          or (".%s." % n.lower()) in c.lower() for c in (channels or [])),
                      "the EXR has channels of the %s layer" % n)

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
