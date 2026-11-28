"""Natron Utility Render Mode and Raster Samples knobs (M514 - Utility Raster Mode).

Graph: USDRead of a generated stage (a camera facing one coloured card, a distant light and a dome light) into
USDRenderSettings (Default Renderer Natron Utility) into USDRender, a Write on the USDRender. The knobs are
found on the settings node by suffix, as the Natron Utility page prefix is derived from the renderer id.

Checks:
  1. knobs:  Render Mode and Raster Samples exist, are labelled like their script names, Render Mode offers Ray
             and Raster and defaults to Ray, Raster Samples defaults to 32 with a minimum of 1 and a display
             maximum of 256.
  2. ray:    in Ray, Raster Samples is hidden or disabled, and the card renders with no Raster fallback warning.
  3. raster: in Raster, Raster Samples is visible and enabled, and the card renders. Where the device cannot
             rasterize (llvmpipe may lack the capability) the frame renders as Ray with the node warning "Render
             Mode is Raster, but ..."; either outcome passes, a warning without an image does not.
  4. back:   Ray again renders with the warning gone.
The settings panel is shot with Raster selected.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 60.0
SETTLE_MS = 700
PANEL_SETTLE_MS = 1200
UTILITY_ID = "HdNatronUtilityRendererPlugin"
FALLBACK_TEXT = "Render Mode is Raster, but"
SIZE = (480, 270)
SAMPLES = 2
MIN_COVERAGE = 0.05
MIN_COLOURS = 2
DEFAULT_RASTER_SAMPLES = 32
MIN_RASTER_SAMPLES = 1
DISPLAY_MAX_RASTER_SAMPLES = 256

CARD_STAGE = """#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Camera "Cam"
    {
        float focalLength = 24
        float horizontalAperture = 36
        float verticalAperture = 20.25
        float2 clippingRange = (0.1, 200)
        double3 xformOp:translate = (0, 0, 4)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Mesh "Card"
    {
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -1, 0), (2, -1, 0), (2, 1, 0), (-2, 1, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "vertex"
        )
        color3f[] primvars:displayColor = [(0.9, 0.35, 0.1)]
    }

    def DistantLight "Key"
    {
        float inputs:intensity = 3
        float inputs:angle = 0
        float3 xformOp:rotateXYZ = (-30, 20, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }

    def DomeLight "Dome"
    {
        float inputs:intensity = 0.6
        color3f inputs:color = (0.6, 0.75, 1.0)
    }
}
"""


try:
    from PySide6.QtGui import QImage
    from PySide6.QtWidgets import QApplication, QTabBar
    from PySide6.QtCore import QMetaObject

    from guitest import (OUT, check, close_all_panels, finish, plugin_problem, report, require, run, section,
                         shot, widgets_of)

    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    stage_path = os.path.join(OUT, "m514-card.usda")
    with open(stage_path, "w") as f:
        f.write(CARD_STAGE)

    read = app.createNode("USDRead")
    require(read is not None, "create USDRead", plugin_problem("USDRead"))
    settings = app.createNode("USDRenderSettings")
    require(settings is not None, "create USDRenderSettings", plugin_problem("USDRenderSettings"))
    render = app.createNode("USDRender")
    require(render is not None, "create USDRender", plugin_problem("USDRender"))
    read.setPosition(0, 0)
    settings.setPosition(0, 100)
    render.setPosition(0, 200)

    def options(param):
        return [str(o) for o in param.getOptions()]

    def choose(param, *wanted):
        """Sets a choice parameter to the entry whose id or label matches one of wanted, ignoring case."""
        lowered = [i.lower() for i in options(param)]
        for w in wanted:
            if w.lower() in lowered:
                param.set(lowered.index(w.lower()))
                return True
        return False

    def utility_knob(suffix):
        """The settings node's Natron Utility parameter whose script name ends in suffix."""
        for p in settings.getParams():
            name = str(p.getScriptName())
            if name.lower().startswith("natronutility") and name.endswith(suffix):
                return p
        return None

    def analyse(path):
        """(coloured share, distinct colours) of the PNG at path, or None if it does not load."""
        image = QImage(path)
        if image.isNull():
            return None
        coloured = 0
        total = 0
        colours = set()
        for y in range(0, image.height(), 2):
            for x in range(0, image.width(), 2):
                c = image.pixelColor(x, y)
                total += 1
                if c.alpha() > 8 and max(c.red(), c.green(), c.blue()) > 8:
                    coloured += 1
                    colours.add((c.red() >> 5, c.green() >> 5, c.blue() >> 5))
        return (coloured / float(total) if total else 0.0), len(colours)

    writer_state = {}

    def node_messages():
        text = ""
        for node in (render, settings, writer_state.get("writer")):
            if node is not None:
                text += str(node.getPersistentMessage() or "") + "\n"
        return text

    def render_to_png(name):
        """Renders the USDRender through a Write to OUT/<name>-render.png; returns (analysis or None, messages)."""
        path = os.path.join(OUT, name + "-render.png")
        if os.path.exists(path):
            os.remove(path)
        if "writer" not in writer_state:
            writer_state["writer"] = app.createWriter(path)
            require(writer_state["writer"] is not None, "create a Write")
            writer_state["writer"].setPosition(200, 200)
            require(writer_state["writer"].connectInput(0, render), "connect the Write to the USDRender")
        writer = writer_state["writer"]
        writer.getParam("filename").setValue(path)
        app.renderBlocking(writer, 1, 1)
        messages = node_messages()
        report("NOTE render %s -> %s; node messages: %r" % (name, path, messages.strip()))
        if not os.path.isfile(path):
            return None, messages
        report("SHOT " + path)
        return analyse(path), messages

    def check_image(name, result):
        check(result is not None, "%s: the render wrote its image" % name)
        if result is None:
            return
        share, colours = result
        check(share >= MIN_COVERAGE, "%s: the image is not empty (%.1f%% of pixels coloured)" % (name, share * 100.0))
        check(colours >= MIN_COLOURS, "%s: the image has several colours (%d)" % (name, colours))

    def settings_panel():
        label = settings.getLabel()
        for panel in widgets_of("NodeSettingsPanel"):
            if panel.isVisible() and (label in (panel.windowTitle() or "") or settings.getScriptName() in (panel.windowTitle() or "")):
                return panel
        return None

    def graph_view():
        views = [v for v in widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def open_settings_panel():
        """Steps that open the settings node's panel and show its Natron Utility tab; returns whether it opened.
        showSelectedNodeSettingsPanel does nothing unless the node is the only selected node."""
        for _attempt in range(2):
            close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            guis = [w for w in widgets_of("Gui") if w.isVisible()]
            if guis:
                guis[0].activateWindow()
                guis[0].raise_()
            app.selectNode(settings, True)
            QApplication.processEvents()
            view = graph_view()
            if view is not None:
                QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
            ok = yield ("until", lambda: settings_panel() is not None, POLL_TIMEOUT_S / 2)
            if ok:
                panel = settings_panel()
                for bar in panel.findChildren(QTabBar):
                    for i in range(bar.count()):
                        if "natron utility" in bar.tabText(i).lower():
                            bar.setCurrentIndex(i)
                yield ("sleep", PANEL_SETTLE_MS)
                return True
        return False

    def samples_shown(samples):
        return bool(samples.getIsVisible()) and bool(samples.getIsEnabled())

    def main():
        yield ("sleep", SETTLE_MS)
        section("setup")
        require(settings.connectInput(0, read), "connect the USDRenderSettings to the USDRead")
        require(render.connectInput(0, settings), "connect the USDRender to the USDRenderSettings")
        read.getParam("filename").setValue(stage_path)
        ok = yield ("until", lambda: settings.getParam("camera") is not None and "/World/Cam" in options(settings.getParam("camera")),
                    POLL_TIMEOUT_S)
        check(ok, "the settings' Camera menu lists /World/Cam")
        choose(settings.getParam("camera"), "/World/Cam")
        require(choose(settings.getParam("defaultRenderer"), UTILITY_ID), "the settings' Default Renderer lists Natron Utility")
        settings.getParam("NatronParamFormatSize").set(SIZE[0], SIZE[1])
        pixel_samples = utility_knob("PixelSamples")
        if pixel_samples is not None:
            pixel_samples.set(SAMPLES)
        yield ("sleep", SETTLE_MS)

        section("knobs")
        mode = utility_knob("RenderMode")
        require(mode is not None, "the Natron Utility settings have a Render Mode parameter")
        samples = utility_knob("RasterSamples")
        require(samples is not None, "the Natron Utility settings have a Raster Samples parameter")
        check(str(mode.getLabel()) == "Render Mode", "the Render Mode label matches its script name (%r)" % mode.getLabel())
        check(str(samples.getLabel()) == "Raster Samples", "the Raster Samples label matches its script name (%r)" % samples.getLabel())
        lowered = [o.lower() for o in options(mode)]
        check(lowered == ["ray", "raster"], "Render Mode offers Ray and Raster (%r)" % options(mode))
        if "ray" in lowered:
            check(mode.getDefaultValue() == lowered.index("ray"), "Render Mode defaults to Ray (%r)" % mode.getDefaultValue())
            check(mode.get() == lowered.index("ray"), "Render Mode starts on Ray (%r)" % mode.get())
        check(samples.getDefaultValue() == DEFAULT_RASTER_SAMPLES, "Raster Samples defaults to %d (%r)" % (DEFAULT_RASTER_SAMPLES, samples.getDefaultValue()))
        check(samples.getMinimum() == MIN_RASTER_SAMPLES, "Raster Samples has a minimum of %d (%r)" % (MIN_RASTER_SAMPLES, samples.getMinimum()))
        check(samples.getDisplayMaximum(0) == DISPLAY_MAX_RASTER_SAMPLES,
              "Raster Samples has a display maximum of %d (%r)" % (DISPLAY_MAX_RASTER_SAMPLES, samples.getDisplayMaximum(0)))

        section("ray")
        check(not samples_shown(samples), "Raster Samples is hidden or disabled in Ray (visible %r, enabled %r)"
              % (samples.getIsVisible(), samples.getIsEnabled()))
        result, messages = render_to_png("m514-ray")
        check_image("ray", result)
        check(FALLBACK_TEXT not in messages, "Ray renders without a Raster fallback warning")

        section("raster")
        require(choose(mode, "Raster"), "Render Mode switches to Raster")
        samples.set(4)
        yield ("sleep", SETTLE_MS)
        check(samples_shown(samples), "Raster Samples is visible and enabled in Raster (visible %r, enabled %r)"
              % (samples.getIsVisible(), samples.getIsEnabled()))
        result, messages = render_to_png("m514-raster")
        fell_back = FALLBACK_TEXT in messages
        report("NOTE Raster %s on this device" % ("fell back to Ray with the warning" if fell_back else "rendered without a fallback"))
        check_image("raster", result)
        if fell_back:
            check(result is not None, "a Raster fallback warning comes with a rendered Ray image")
        opened = yield from open_settings_panel()
        check(opened, "open the settings node's panel")
        if opened:
            shot("m514-raster-settings.png")
        close_all_panels()

        section("back")
        require(choose(mode, "Ray"), "Render Mode switches back to Ray")
        yield ("sleep", SETTLE_MS)
        check(not samples_shown(samples), "Raster Samples is hidden or disabled again in Ray (visible %r, enabled %r)"
              % (samples.getIsVisible(), samples.getIsEnabled()))
        result, messages = render_to_png("m514-ray-again")
        check_image("ray again", result)
        check(FALLBACK_TEXT not in messages, "Ray renders without a Raster fallback warning after Raster")

    run(main())
except Exception:
    from guitest import check as _check, finish as _finish
    _check(False, "script error: " + traceback.format_exc())
    _finish()
