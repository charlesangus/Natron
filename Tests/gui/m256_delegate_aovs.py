"""Delegate AOV selection (M256 - Delegate AOV Selection), the Phase 256.3 gate: three Cycles passes
ticked plus one custom primvar var give four extra layers in the viewer and in one EXR, and a
MoonRay tick in the same node adds no warning to the Cycles render.

Graph: USDSphere into USDProjectUV (Perspective, through USDCamera1: the st primvar it authors is a
ramp across the sphere), merged with USDCamera1 and a USDLight, into USDRenderSettings (Default
Renderer Cycles) into USDRender, which feeds Viewer1 and a Write. In a 1920x1080 window the script:
  - ticks Cycles' diffuse_direct, ao and mist, MoonRay's albedo (or its first AOV), and authors the
    st primvar as a Custom AOVs row from Python;
  - clicks the table's Add... button, checks Python reads the added row back, and removes it again
    from Python, since it is a second st;
  - waits for the viewer's layer menu to list the four layers with no reconnection;
  - checks USDRender, USDRenderSettings and the viewer carry no warning that names MoonRay;
  - shows each of the four layers in the viewer, one screenshot each (m256-<layer>.png, the viewer
    only), and checks each differs from the others;
  - renders a Write to one EXR and lists its channels with oiiotool: each layer's channels are there.
A build without Cycles, or without a MoonRay checklist, reports SKIP for what it lacks.

Run through Tests/gui/run-gui-test.sh, at 1920x1080. Screenshots, the EXR and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import re
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
RENDER_TIMEOUT_S = 60.0
SETTLE_MS = 800
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

    def normal(text):
        return re.sub(r"[^a-z0-9]", "", text.lower())

    def choose_containing(p, word):
        options = [str(o) for o in p.getOptions()]
        for i, o in enumerate(options):
            if word.lower() in o.lower():
                p.set(i)
                return True
        return False

    app.addFormat("m256Aovs 320x240")
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
        want = normal(aov)
        for name, p in ticks(prefix):
            # A pass the delegate names in its own namespace is Cycles...: cyclesAovCyclesAo.
            if normal(name[len(prefix) + 3:]).replace(prefix, "", 1) == want:
                return name, p
        return None, None

    def layer_combo():
        """The viewer's layer menu: the visible ComboBox that lists a colour layer."""
        for c in widgets_of("ComboBox"):
            try:
                items = [str(i) for i in (c.property("natronItems") or [])]
                if c.isVisible() and any("depth" in i.lower() for i in items):
                    return hold(c), items
            except RuntimeError:
                continue
        return None, []

    def layer_listed(name):
        _, items = layer_combo()
        return any(i.lower().startswith(name.lower()) for i in items)

    def viewer_crop():
        views = [w for w in widgets_of("ViewerGL") if w.isVisible()]
        if not views:
            return None
        v = hold(views[0])
        QApplication.processEvents()
        origin = v.mapToGlobal(v.rect().topLeft())
        return QGuiApplication.primaryScreen().grabWindow(0).toImage().copy(origin.x(), origin.y(), v.width(), v.height())

    def image_signature(image):
        step = 8
        return tuple(image.pixel(x, y) for y in range(0, image.height(), step) for x in range(0, image.width(), step))

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

        signatures = {}
        for n in wanted:
            combo, items = layer_combo()
            index = next((i for i, t in enumerate(items) if t.lower().startswith(n.lower())), None)
            if combo is None or index is None:
                check(False, "the layer menu has %s to show" % n)
                continue
            QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
            QApplication.processEvents()
            yield ("sleep", SETTLE_MS)
            image = viewer_crop()
            if image is None:
                check(False, "the viewer is on screen for %s" % n)
                continue
            path = os.path.join(OUT, "m256-%s.png" % n)
            image.save(path)
            report("SHOT " + path)
            signatures[n] = image_signature(image)
        names = list(signatures)
        for i, a in enumerate(names):
            for b in names[i + 1:]:
                check(signatures[a] != signatures[b], "the viewer shows %s and %s differently" % (a, b))

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
