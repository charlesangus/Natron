"""Scrubs graphs whose layers vary with time and checks errors, images and layer menus per frame.

The graph that registers a project layer named diffuse runs last, so that layer stays out of the
graphs reading diffuse from a file.

Usage: Tests/gui/run-gui-test.sh Tests/gui/m61_uat.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400
STARTUP_TIMEOUT_S = 60.0
ROUND_TRIP = (1, 2, 1, 2, 1, 2, 1)
# The Color layer's ID; "Color" is only its label.
COLOUR = "uk.co.thefoundry.OfxImagePlaneColour"


class GraphAborted(Exception):
    pass


try:
    from PySide6.QtCore import Q_ARG, QMetaObject, QPointF
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QLabel, QLineEdit, QWidget

    import guitest as gt
    from guitest import check, report

    viewer = app.getNode("Viewer1")
    if viewer is None:
        viewer = app.createNode("fr.inria.built-in.Viewer")
    gt.require(viewer is not None, "a Viewer node exists")
    viewer.setPosition(400, 600)
    try:
        app.getProjectParam("frameRange").set(1, 10)
    except Exception:
        report("NOTE could not set the project frame range: " + traceback.format_exc().splitlines()[-1])

    _made = []
    _py_viewer = {}

    def need(ok, label, detail=""):
        """A failed setup step abandons the current graph only."""
        check(ok, label + (" (" + detail + ")" if detail and not ok else ""))
        if not ok:
            raise GraphAborted(label)

    def create(plugin_id, x, y):
        node = app.createNode(plugin_id)
        need(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        _made.append(node)
        node.setPosition(x, y)
        return node

    def create_reader(name, x, y):
        path = os.path.join(gt.FIXTURES, name)
        need(os.path.isfile(path.replace("####", "0001")), "fixture exists: " + name)
        node = app.createReader(path)
        need(node is not None, "create a Read for " + name)
        _made.append(node)
        node.setPosition(x, y)
        return node

    def connect(node, input_nb, source):
        need(node.connectInput(input_nb, source),
             "connect %s input %d to %s" % (node.getLabel(), input_nb, source.getLabel()))

    def destroy_graph():
        viewer.disconnectInput(0)
        while _made:
            _made.pop().destroy(False)

    def py_viewer():
        if "v" not in _py_viewer:
            _py_viewer["v"] = app.getViewer(viewer.getScriptName())
        return _py_viewer["v"]

    def seek(frame):
        py_viewer().seek(frame)

    def display_channels(name):
        """Picks name ("RGB", "Alpha"...) in the viewer's channels menu, as a user would.
        PyViewer.setChannels() cannot be used: it looks entries up as "A", "R"... while the
        menu labels them "Alpha", "Red"..., so it silently does nothing for those."""
        for combo in gt.widgets_of("ComboBox") + gt.widgets_of("ChannelsComboBox"):
            if "Display Channels:" not in (combo.toolTip() or ""):
                continue
            items = [str(i) for i in (combo.property("natronItems") or [])]
            if name in items:
                QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, items.index(name)))
                return
        raise GraphAborted("no viewer channels menu entry " + name)

    # ---- state probes

    COLOURS = {
        "red": lambda c: c[0] > 200 and c[1] < 60 and c[2] < 60,
        "green": lambda c: c[0] < 60 and c[1] > 200 and c[2] < 60,
        "black": lambda c: max(c) < 40,
        "white": lambda c: min(c) > 200,
    }

    def colour_is(name):
        c = gt.viewer_centre_colour()
        return c is not None and COLOURS[name](c)

    def message(node):
        return node.getPersistentMessage() or ""

    def in_viewer(node):
        label = node.getLabel()
        return any(label in m for m in gt.viewer_messages())

    def expect(node, error=None, colour=None):
        """error: None when the node must be clean, else substrings its message must contain. The
        viewer overlay must agree with the node either way."""
        def matches():
            msg = message(node)
            if error is None:
                if msg or in_viewer(node):
                    return False
            else:
                if not msg or any(e.lower() not in msg.lower() for e in error):
                    return False
                if not in_viewer(node):
                    return False
            return colour is None or colour_is(colour)
        return matches

    def describe(node):
        return "message=%r viewer=%r colour=%r" % (message(node), gt.viewer_messages(),
                                                   gt.viewer_centre_colour())

    def verify(predicate, label, detail):
        """Polls predicate, then checks it still holds a moment later, so a late render from a
        frame left behind cannot flip it after the fact."""
        ok = yield ("until", predicate, POLL_TIMEOUT_S)
        if ok:
            yield ("sleep", SETTLE_MS)
            ok = bool(predicate())
        check(ok, label + ("" if ok else " (" + detail() + ")"))
        return ok

    def verify_node(node, label, error=None, colour=None):
        ok = yield from verify(expect(node, error, colour), label, lambda: describe(node))
        if ok and error is not None:
            report("NOTE %s message: %r" % (node.getLabel(), message(node)))
        return ok

    def scrub(node, per_frame, what, shots=None):
        """Seeks along ROUND_TRIP and checks node against per_frame[frame] = (error, colour)."""
        for i, frame in enumerate(ROUND_TRIP):
            seek(frame)
            error, colour = per_frame[frame]
            yield from verify_node(node, "%s: seek #%d (frame %d): %s" % (
                what, i + 1, frame, "error " + "/".join(error) if error else "no error"), error, colour)
            if shots and i < 2:
                gt.shot("%s-frame%d.png" % (shots, frame))

    # ---- widget probes

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            for w in panel.findChildren(QLineEdit) + panel.findChildren(QLabel):
                try:
                    if w.text() == label:
                        return panel
                except RuntimeError:
                    continue
        return None

    def knob_property(node, script_name, widget_class, name):
        """Property name of the widget of class widget_class that belongs to the knob script_name
        in node's panel, found through the knob tooltip, which leads with the script name in
        bold. Read while the child list is still held: a wrapper outliving it can come back as
        already deleted."""
        panel = panel_of(node)
        if panel is None:
            return None
        tag = "<b>%s</b>" % script_name
        children = panel.findChildren(QWidget)
        for w in children:
            try:
                if gt.class_name(w) == widget_class and tag in (w.toolTip() or ""):
                    return w.property(name)
            except RuntimeError:
                continue
        return None

    def combo_state(combo):
        if combo is None:
            return None, None
        try:
            items = combo.property("natronItems")
            current = combo.property("natronCurrentText")
        except RuntimeError:
            # A layer row rebuilds its combo on refresh; the caller polls again.
            return None, None
        return ([str(i) for i in items] if items is not None else None), current

    # The Shuffle's layer knobs are hidden; their dropdowns are rows of the mapping knob's
    # widget, told apart by the tooltip each row takes from its knob's hint.
    LAYER_ROW_HINTS = {
        "in1": "The layer the first slot reads",
        "in2": "The layer the second slot reads",
        "out1": "The first layer this node writes",
        "out2": "The second layer this node writes",
    }

    def layer_menu(node, knob):
        panel = panel_of(node)
        if panel is None:
            return None, None
        try:
            for row in panel.findChildren(QWidget):
                if gt.class_name(row) != "LayerChannelRow":
                    continue
                if not (row.toolTip() or "").startswith(LAYER_ROW_HINTS[knob]):
                    continue
                for w in row.findChildren(QWidget):
                    if gt.class_name(w) == "ComboBox":
                        return combo_state(w)
        except RuntimeError:
            pass
        return None, None

    def viewer_layer_combo():
        for w in gt.widgets_of("ComboBox"):
            try:
                if w.isVisible() and "Layer:" in (w.toolTip() or ""):
                    return w
            except RuntimeError:
                continue
        return None

    def offers(items, layer):
        return items is not None and any(i == layer for i in items)

    def mentions(items, layer):
        return items is not None and any(layer in i for i in items)

    def animation_level(node, knob, widget_class):
        return knob_property(node, knob, widget_class, "animation")

    def input_layers(node, input_nb=0):
        """The layers node sees on an input at the current frame, as its layer menus list them."""
        return sorted(str(l.getLayerName()) for l in node.getAvailableLayers(input_nb))

    def check_colour_out(shuffle):
        layer = shuffle.getParam("out1").getLayer()
        check(layer == COLOUR, "%s's Out 1 is Color (%r)" % (shuffle.getLabel(), layer))

    # QGraphicsItems are left alone: they are not QObjects, so PySide cannot tell when Natron
    # deletes one, and a wrapper it hands back for a reused address crashes when used. The node
    # graph is read through its view's scene-to-screen mapping and the screen instead.

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return views[0] if views else None

    def box_to_screen(view, node, fx, fy):
        """The global screen point at fraction (fx, fy) of node's box."""
        x, y = node.getPosition()[0:2]
        w, h = node.getSize()[0:2]
        return view.viewport().mapToGlobal(view.mapFromScene(QPointF(x + fx * w, y + fy * h)))

    CROSS_SAMPLES = ((0.2, 0.2), (0.8, 0.8), (0.2, 0.8), (0.8, 0.2))

    def crossed(node):
        """Whether node's box shows the disabled cross on screen: True when the darkest pixel
        around each of four points on the box's diagonals is near black (the cross's pen), False
        when none is, None when neither (a mixed or unreadable box)."""
        view = node_graph_view()
        if view is None:
            return None
        image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
        dark = []
        for fx, fy in CROSS_SAMPLES:
            p = box_to_screen(view, node, fx, fy)
            darkest = min(max(image.pixelColor(p.x() + dx, p.y() + dy).getRgb()[0:3])
                          for dx in (-1, 0, 1) for dy in (-1, 0, 1))
            dark.append(darkest < 50)
        if all(dark):
            return True
        if not any(dark):
            return False
        return None

    def open_panel(node):
        """Opens node's settings panel through the node graph's own action for the selected
        node, which a double-click also ends in: nodes made from Python start with it closed,
        and synthesized mouse events do not reach the node graph under Xvfb."""
        view = node_graph_view()
        need(view is not None, "the node graph is on screen")
        app.selectNode(node, True)
        QMetaObject.invokeMethod(view, "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        need(ok, "open %s's settings panel" % node.getLabel())

    # ---- graphs

    def graph1():
        """A Read sequence whose layers change per frame."""
        seek(1)
        read = create_reader("flat-seq-layers.####.exr", 100, 100)
        first, last = read.getParam("firstFrame").get(), read.getParam("lastFrame").get()
        check((first, last) == (1, 2), "the Read picks up frames 1-2 as one sequence (got %d-%d)" % (first, last))
        shuffle = create("fr.natron.Shuffle", 100, 250)
        connect(shuffle, 0, read)
        shuffle.getParam("in2").setLayer("diffuse")
        check_colour_out(shuffle)
        shuffle.getParam("mapping").connect("in2.#1", "out1.#0")
        connect(viewer, 0, shuffle)
        yield from open_panel(shuffle)

        ok_frame = (None, "red")
        bad_frame = (["diffuse.g", "not in the Source input"], "black")
        seek(1)
        yield from verify_node(shuffle, "frame 1: no error badge, viewer renders", *ok_frame)

        def in2():
            return "items=%r current=%r" % layer_menu(shuffle, "in2")

        def in2_offers_diffuse():
            items, current = layer_menu(shuffle, "in2")
            return offers(items, "diffuse") and current == "diffuse"

        def in2_keeps_missing_diffuse():
            items, current = layer_menu(shuffle, "in2")
            return (items is not None and not offers(items, "diffuse")
                    and current is not None and current.startswith("diffuse"))

        yield from verify(in2_offers_diffuse, "frame 1: In 2 offers diffuse and shows it selected", in2)
        gt.shot("g1-frame1.png")

        seek(2)
        yield from verify_node(shuffle, "frame 2: error badge naming diffuse.g, viewer shows the error", *bad_frame)
        yield from verify(in2_keeps_missing_diffuse, "frame 2: In 2 no longer offers diffuse but keeps it as the "
                          "(missing) choice", in2)
        check(shuffle.getParam("in2").getLayer() == "diffuse", "frame 2: In 2's value is still diffuse")
        gt.shot("g1-frame2.png")

        yield from scrub(shuffle, {1: ok_frame, 2: bad_frame}, "round trip")
        gt.shot("g1-after-scrub-frame1.png")

    def layer_menus():
        """Wrap-up item: build on frame 2, then scrub; the menus relist per frame."""
        seek(2)
        read = create_reader("flat-seq-layers.####.exr", 100, 100)
        shuffle = create("fr.natron.Shuffle", 100, 250)
        connect(shuffle, 0, read)
        connect(viewer, 0, shuffle)
        yield from open_panel(shuffle)
        ok = yield ("until", lambda: viewer_layer_combo() is not None, POLL_TIMEOUT_S)
        need(ok, "the viewer's layer menu is on screen")
        combo = viewer_layer_combo()

        def viewer_items():
            return combo_state(combo)[0]

        def viewer_current():
            return combo_state(combo)[1] or ""

        def menus_list(listed):
            def matches():
                shuffle_items = layer_menu(shuffle, "in2")[0]
                return (all(offers(shuffle_items, l) == listed for l in ("diffuse", "specular"))
                        and all(mentions(viewer_items(), l) == listed for l in ("diffuse", "specular")))
            return matches

        def menus():
            return "In 2=%r viewer=%r" % (layer_menu(shuffle, "in2")[0], viewer_items())

        for i, frame in enumerate((2,) + ROUND_TRIP):
            seek(frame)
            listed = frame == 1
            yield from verify(menus_list(listed), "seek #%d (frame %d): In 2 and the viewer's layer menu %s "
                              "diffuse and specular" % (i + 1, frame, "offer" if listed else "do not offer"), menus)
            if i < 2:
                gt.shot("menus-frame%d.png" % frame)

        items = viewer_items() or []
        index = next((n for n, i in enumerate(items) if "diffuse" in i), -1)
        need(index >= 0, "the viewer's layer menu has a diffuse entry", repr(items))
        QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, index))
        yield from verify(lambda: "diffuse" in viewer_current() and colour_is("green"),
                          "frame 1: the viewer shows the chosen diffuse layer (green)",
                          lambda: "current=%r colour=%r" % (viewer_current(), gt.viewer_centre_colour()))
        for frame in (2, 1, 2, 1):
            seek(frame)
            if frame == 2:
                def kept():
                    return "diffuse" in viewer_current() and not mentions(viewer_items(), "specular")
                label = "frame 2: the viewer keeps diffuse selected while specular leaves its menu"
            else:
                def kept():
                    return "diffuse" in viewer_current() and mentions(viewer_items(), "specular")
                label = "frame 1: diffuse still selected, specular listed again"
            yield from verify(kept, label, lambda: "items=%r current=%r" % (viewer_items(), viewer_current()))
            if frame == 2:
                gt.shot("menus-viewer-keeps-diffuse-frame2.png")
        items = viewer_items() or []
        back = next((n for n, i in enumerate(items) if "Color" in i or "RGBA" in i), -1)
        need(back >= 0, "the viewer's layer menu has a colour entry", repr(items))
        QMetaObject.invokeMethod(combo, "setCurrentIndex", Q_ARG(int, back))
        yield from verify(lambda: "diffuse" not in viewer_current(),
                          "the viewer's layer menu is back on the colour layer",
                          lambda: "items=%r current=%r" % (viewer_items(), viewer_current()))

    def graph2():
        """A Switch whose which is keyed."""
        seek(1)
        read_a = create_reader("flat-three-layers.exr", 0, 100)
        read_b = create_reader("flat-rgba-only.exr", 250, 100)
        switch = create("net.sf.openfx.switchPlugin", 120, 250)
        connect(switch, 0, read_a)
        connect(switch, 1, read_b)
        automatic = switch.getParam("automatic")
        if automatic is not None:
            automatic.set(False)
            check(not automatic.get(), "the Switch's Automatic is off")
        which = switch.getParam("which")
        need(which is not None, "the Switch has a which parameter")
        which.setValueAtTime(0, 1)
        which.setValueAtTime(1, 2)
        shuffle = create("fr.natron.Shuffle", 120, 400)
        connect(shuffle, 0, switch)
        shuffle.getParam("in2").setLayer("diffuse")
        check_colour_out(shuffle)
        shuffle.getParam("mapping").connect("in2.#1", "out1.#0")
        connect(viewer, 0, shuffle)
        yield from open_panel(switch)
        yield from open_panel(shuffle)
        channels = switch.getParam("channels")
        need(channels is not None, "the Switch has a channels selector")

        per_frame = {1: (None, "red"), 2: (["diffuse.g", "not in the Source input"], "black")}
        for mode in ("Color", "All"):
            if mode == "All":
                channels.setAll()
            else:
                channels.setLayer(COLOUR)
            yield ("sleep", 300)
            yield from scrub(shuffle, per_frame, mode, shots="g2-%s" % mode.lower())
            for frame in (1, 2, 1):
                seek(frame)
                want = frame == 1

                def listed(w=want):
                    layers = input_layers(shuffle)
                    items = layer_menu(shuffle, "in2")[0]
                    return (("diffuse" in layers) == w and ("specular" in layers) == w
                            and offers(items, "diffuse") == w and offers(items, "specular") == w)
                yield from verify(listed, "%s, frame %d: the Switch's output and the Shuffle's In 2 %s the "
                                  "three-layer input's diffuse/specular" % (mode, frame, "list" if want else "do not list"),
                                  lambda: "switch layers=%r In 2=%r" % (input_layers(shuffle),
                                                                        layer_menu(shuffle, "in2")[0]))

        for frame, level in ((1, 2), (2, 2), (3, 1), (1, 2)):
            seek(frame)
            yield from verify(lambda l=level: animation_level(switch, "which", "SpinBox") == l,
                              "frame %d: the Which field shows %s colouring" % (
                                  frame, "on-keyframe" if level == 2 else "animated, off-keyframe"),
                              lambda: "animation=%r" % animation_level(switch, "which", "SpinBox"))
            if frame == 1:
                gt.shot("g2-which-keyframe-frame1.png")

    def graph4():
        """Default (unmapped) rows are checked too."""
        seek(1)
        read = create_reader("flat-seq-layers.####.exr", 0, 100)
        shuffle = create("fr.natron.Shuffle", 0, 250)
        connect(shuffle, 0, read)
        shuffle.getParam("in1").setLayer("diffuse")
        check_colour_out(shuffle)
        mapping = shuffle.getParam("mapping")
        connect(viewer, 0, shuffle)

        yield from verify_node(shuffle, "In 1 diffuse, frame 1: error badge naming diffuse.A",
                               ["diffuse.A", "not in the Source input"], "black")
        gt.shot("g4-in1-diffuse-alpha-error.png")
        mapping.connect("0", "out1.#3")
        yield from verify_node(shuffle, "A row set to 0, frame 1: badge clears, viewer renders diffuse (green)",
                               None, "green")
        per_frame = {1: (None, "green"), 2: (["diffuse.R", "not in the Source input"], "black")}
        yield from scrub(shuffle, per_frame, "In 1 diffuse round trip", shots="g4-in1-diffuse")

        destroy_graph()
        read_rgb = create_reader("flat-rgb-only.exr", 0, 100)
        plain = create("fr.natron.Shuffle", 0, 250)
        connect(plain, 0, read_rgb)
        connect(viewer, 0, plain)
        for frame in (1, 2, 1):
            seek(frame)
            yield from verify_node(plain, "RGB-only input, default Shuffle, frame %d: error badge naming Color's A, "
                                   "no silent pass-through" % frame, [".A", "not in the Source input"], "black")
        gt.shot("g4-rgb-only-alpha-error.png")
        plain.getParam("mapping").connect("1", "out1.#3")
        yield from verify_node(plain, "A row set to 1: badge clears, viewer renders red", None, "red")
        display_channels("Alpha")
        yield from verify_node(plain, "A row set to 1: the viewer's alpha is 1", None, "white")
        gt.shot("g4-rgb-only-alpha-1.png")
        display_channels("RGB")
        plain.getParam("mapping").disconnect("out1.#3")
        yield from verify_node(plain, "A row back to default: the error returns", [".A"], "black")

        destroy_graph()
        read_rgba = create_reader("flat-rgba-only.exr", 0, 100)
        read_rgb = create_reader("flat-rgb-only.exr", 250, 100)
        copy = create("fr.natron.ShuffleCopy", 120, 250)
        connect(copy, 0, read_rgba)
        connect(copy, 1, read_rgb)
        connect(viewer, 0, copy)
        copy_mapping = copy.getParam("mapping")
        yield from verify_node(copy, "ShuffleCopy, input 1 RGB-only: error badge naming Color's A from input 1",
                               [".A", "not in the 1 input"], "black")
        gt.shot("g4-shufflecopy-alpha-error.png")
        copy_mapping.connect("1", "out1.#3")
        yield from verify_node(copy, "ShuffleCopy A row set to 1: badge clears", None, "red")
        # With the A row left on the constant 1, nothing would read input 1 once it is
        # disconnected, so the row goes back to its default before the disconnect is checked.
        copy_mapping.disconnect("out1.#3")
        yield from verify_node(copy, "ShuffleCopy A row back to default: the error returns",
                               [".A", "not in the 1 input"], "black")
        copy.disconnectInput(1)
        yield from verify_node(copy, "ShuffleCopy input 1 disconnected: no badge, viewer renders red", None, "red")
        display_channels("Alpha")
        yield from verify_node(copy, "ShuffleCopy input 1 disconnected: alpha renders 0", None, "black")
        gt.shot("g4-shufflecopy-disconnected-alpha-0.png")
        display_channels("RGB")
        for frame in (2, 1):
            seek(frame)
            yield from verify_node(copy, "ShuffleCopy input 1 disconnected, frame %d: still silent" % frame,
                                   None, "red")

    def graph3():
        """A Shuffle with a keyed Disable, writing a new diffuse layer."""
        seek(1)
        read = create_reader("flat-rgba-only.exr", 100, 100)
        writer = create("fr.natron.Shuffle", 100, 250)
        connect(writer, 0, read)
        if app.getProjectLayer("diffuse") is None:
            app.addProjectLayer("diffuse", ["R", "G", "B"])
        need(app.getProjectLayer("diffuse") is not None, "the project has a diffuse layer, as New layer... makes")
        writer.getParam("out1").setLayer("diffuse")
        need(writer.getParam("out1").getLayer() == "diffuse", "the first Shuffle's Out 1 is diffuse",
             repr(writer.getParam("out1").getLayer()))
        # diffuse.g takes Color.r (1) rather than Color.g (0), so the downstream read of
        # diffuse.g renders red where it succeeds, which black (the error) cannot be mistaken for.
        writer.getParam("mapping").connect("in1.#0", "out1.#1")
        disable = writer.getParam("disableNode")
        need(disable is not None, "the first Shuffle has a Disable parameter")
        disable.setValueAtTime(False, 1)
        disable.setValueAtTime(True, 2)
        downstream = create("fr.natron.Shuffle", 100, 400)
        connect(downstream, 0, writer)
        downstream.getParam("in2").setLayer("diffuse")
        check_colour_out(downstream)
        downstream.getParam("mapping").connect("in2.#1", "out1.#0")
        connect(viewer, 0, downstream)
        yield from open_panel(writer)

        per_frame = {1: (None, "red"), 2: (["diffuse.g", "not in the Source input"], "black")}

        def cross_matches(frame):
            want = frame == 2
            return lambda: crossed(writer) == want and bool(disable.getValueAtTime(frame)) == want

        def cross_detail():
            return "crossed=%r disable@1=%r disable@2=%r" % (
                crossed(writer), disable.getValueAtTime(1), disable.getValueAtTime(2))

        for how in ("keyed", "expression"):
            if how == "expression":
                disable.removeAnimation()
                check(not disable.getIsAnimated(), "Disable's keyframes are removed")
                need(disable.setExpression("ret = frame >= 2", True), "Disable gets the expression ret = frame >= 2")
            yield from scrub(downstream, per_frame, "%s Disable" % how, shots="g3-%s" % how)
            for i, frame in enumerate(ROUND_TRIP):
                seek(frame)
                yield from verify(cross_matches(frame), "%s Disable, seek #%d (frame %d): the first Shuffle's node "
                                  "box is %s" % (how, i + 1, frame, "crossed out" if frame == 2 else "plain"),
                                  cross_detail)
                if i < 2:
                    gt.shot("g3-%s-nodegraph-frame%d.png" % (how, frame))
            if how == "keyed":
                for frame, level in ((1, 2), (2, 2), (3, 1)):
                    seek(frame)
                    yield from verify(lambda l=level: animation_level(writer, "disableNode", "AnimatedCheckBox") == l,
                                      "frame %d: the Disable check box shows %s colouring" % (
                                          frame, "on-keyframe" if level == 2 else "animated, off-keyframe"),
                                      lambda: "animation=%r" % animation_level(writer, "disableNode",
                                                                               "AnimatedCheckBox"))
        report("NOTE Graph 3: the cross is checked as dark pixels on the box's diagonals; the rest of the "
               "disabled look is screenshot-only (g3-*-nodegraph-frame*.png)")

    GRAPHS = (
        ("Graph 1", graph1),
        ("Layer menus", layer_menus),
        ("Graph 2", graph2),
        ("Graph 4", graph4),
        ("Graph 3", graph3),
    )

    def steps():
        gt.section("Startup")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        check(ok, "viewer widget is up")
        if not ok:
            return
        yield ("sleep", 1000)
        only = os.environ.get("M61_UAT_GRAPHS")
        for name, graph in GRAPHS:
            if only and name not in only.split(","):
                continue
            gt.section(name)
            try:
                yield from graph()
            except GraphAborted:
                pass
            except Exception:
                check(False, "script error: " + traceback.format_exc())
            try:
                display_channels("RGB")
                destroy_graph()
            except Exception:
                check(False, "cleanup error: " + traceback.format_exc())
            yield ("sleep", 500)

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
