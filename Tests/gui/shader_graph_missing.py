"""A GUI test of a shader whose plug-in is missing inside a USD Material Builder: loaded from a
project naming an unknown shader ID, the placeholder draws the ports it was saved with, its inputs
and the outputs its readers named, so its pipes stay where they were; saving keeps every
connection and output name. It is driven by Tests/gui/run-gui-test.sh (1920x1080), not ctest.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \\
        NATRON_GUI_TEST_OUT=build/m351-gui/missing GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/shader_graph_missing.py <AppImage or build/fast/App/Natron>'

Shot: shader_graph_missing.png (the placeholder between the texture and the mix, with its pipes).
"""
import os
import re
import sys
import traceback
import zipfile

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
TEXTURE = "cycles_image_texture"
BSDF = "cycles_principled_bsdf"
MIX = "cycles_mix_closure"
REMOVED = "cycles_removed_shader"
MISSING_PLUGIN = "MissingPlugin"

try:
    import faulthandler

    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, report, require

    import shader_graph_util as sg

    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def rename_plugin(source, dest, old_id, new_id):
        """Copies the project bundle source to dest with the plug-in ID old_id renamed new_id;
        returns how many nodes it renamed."""
        pattern = re.compile(r"(\bplugin:\s*[\"']?)" + re.escape(old_id) + r"\b")
        renamed = 0
        with zipfile.ZipFile(source) as src, zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as out:
            for info in src.infolist():
                data = src.read(info.filename)
                if info.filename.endswith("project.yaml"):
                    text, renamed = pattern.subn(lambda m: m.group(1) + new_id, data.decode("utf-8"))
                    data = text.encode("utf-8")
                out.writestr(info, data)
        return renamed

    def saved_yaml(path):
        with zipfile.ZipFile(path) as archive:
            for name in archive.namelist():
                if name.endswith("project.yaml"):
                    return archive.read(name).decode("utf-8")
        return ""

    def load(path, builder_name):
        """Loads path and returns the builder once it is back, None on a timeout."""
        reset, asked = sg.answering(app.resetProject)
        require(reset, "reset the project")
        yield ("sleep", SETTLE_MS)
        host, asked = sg.answering(lambda: app.loadProject(path))
        require(host is not None, "loaded " + path)
        for text in asked:
            report("NOTE answered %r" % text)
        ok = yield ("until", lambda: host.getNode(builder_name) is not None, LOAD_TIMEOUT_S)
        require(ok, "the builder is back after loading " + path)
        yield ("sleep", SETTLE_MS)
        return host.getNode(builder_name)

    def check_connections(stage, names, bsdf, mix):
        texture_name, bsdf_name = names[0], names[1]
        for port, output in (("base_color", "color"), ("alpha", "alpha")):
            check(sg.input_source(bsdf, port) == texture_name,
                  "%s: %s.%s is connected to %s (got %r)" % (stage, bsdf_name, port, texture_name,
                                                           sg.input_source(bsdf, port)))
        check(sg.input_source(mix, "closure1") == bsdf_name,
              "%s: closure1 is connected to %s (got %r)" % (stage, bsdf_name, sg.input_source(mix, "closure1")))
        check(mix.getOutputForInput("closure1") == "BSDF",
              "%s: closure1 still reads the output BSDF (got %r)" % (stage, mix.getOutputForInput("closure1")))

    def steps():
        if app.getNode("Viewer1") is None:
            app.createNode("Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        sg.park_viewer(app)
        require(ok, "the main window is up")

        gt.section("build")
        builder = sg.create(app, sg.BUILDER, 0, 0)
        builder_name = builder.getScriptName()
        yield ("sleep", SETTLE_MS)
        require(sg.open_graph(app, builder), "the builder's graph has a tab")
        texture = sg.create(app, TEXTURE, -350, 0, builder)
        bsdf = sg.create(app, BSDF, 0, 0, builder)
        mix = sg.create(app, MIX, 400, 0, builder)
        yield ("sleep", SETTLE_MS)
        sg.connect(texture, "color", bsdf, "base_color")
        sg.connect(texture, "alpha", bsdf, "alpha")
        sg.connect(bsdf, "BSDF", mix, "closure1")
        names = (texture.getScriptName(), bsdf.getScriptName(), mix.getScriptName())
        yield ("sleep", SETTLE_MS)

        original = os.path.join(gt.OUT, "shader_graph_missing_original.ntpz")
        missing = os.path.join(gt.OUT, "shader_graph_missing.ntpz")
        require(app.saveProjectAs(original), "saved the project to " + original)
        renamed = rename_plugin(original, missing, BSDF, REMOVED)
        require(renamed == 1, "the project names the unknown shader %s once (renamed %d)" % (REMOVED, renamed))

        gt.section("load")
        builder = yield from load(missing, builder_name)
        placeholder = builder.getNode(names[1])
        texture = builder.getNode(names[0])
        mix = builder.getNode(names[2])
        require(placeholder is not None and texture is not None and mix is not None, "every node is back")
        check(placeholder.getPluginID() == MISSING_PLUGIN,
              "the unknown shader loads as a missing plug-in (got %r)" % placeholder.getPluginID())
        check(sg.input_source(placeholder, "base_color") == names[0], "the placeholder keeps base_color")
        check(sg.input_source(placeholder, "alpha") == names[0], "the placeholder keeps alpha")
        check(sg.input_source(mix, "closure1") == names[1], "the mix still reads the placeholder")
        check(mix.getOutputForInput("closure1") == "BSDF",
              "the mix still reads the output BSDF (got %r)" % mix.getOutputForInput("closure1"))

        require(sg.open_graph(app, builder), "the loaded builder's graph has a tab")
        ok = yield ("until", lambda: sg.graph_of(builder) is not None and sg.graph_of(builder).isVisible(), 10.0)
        require(ok, "the loaded builder's graph is shown")
        graph = sg.graph_of(builder)
        items = sg.port_items(graph, "%s.%s" % (builder_name, names[1]))
        report("NOTE placeholder ports %r" % (items,))
        check(sorted(items["input"]) == ["alpha", "base_color"],
              "the placeholder draws its saved inputs as ports (got %r)" % (items["input"],))
        check(items["output"] == ["BSDF"],
              "the placeholder draws the output its reader named (got %r)" % (items["output"],))
        graph.centerOn(200, 50)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)
        report("NOTE the missing look and the pipes are checked by eye in the screenshot")
        gt.shot("shader_graph_missing.png")

        gt.section("resave")
        resaved = os.path.join(gt.OUT, "shader_graph_missing_resaved.ntpz")
        require(app.saveProjectAs(resaved), "saved the project to " + resaved)
        check(REMOVED in saved_yaml(resaved), "the resaved project still names %s" % REMOVED)
        back = os.path.join(gt.OUT, "shader_graph_missing_back.ntpz")
        renamed = rename_plugin(resaved, back, REMOVED, BSDF)
        require(renamed == 1, "the resaved project names the unknown shader once (renamed %d)" % renamed)
        builder = yield from load(back, builder_name)
        nodes = [builder.getNode(n) for n in names]
        require(all(n is not None for n in nodes), "every node is back with the shader")
        check(nodes[1].getPluginID() == BSDF, "the shader loads again (got %r)" % nodes[1].getPluginID())
        check_connections("shader back", names, nodes[1], nodes[2])
        check(nodes[1].getOutputForInput("base_color") == "color", "base_color still reads color")
        check(nodes[1].getOutputForInput("alpha") == "alpha", "alpha still reads alpha")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
