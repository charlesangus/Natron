"""A node whose own content changes size (label, name) keeps its top edge and horizontal centre, so
it stays lined up with its neighbours. User resizes through setSize(), backdrops and a save/reload
keep the top-left corner.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL symlink/Xvfb setup):
    docker exec natron-dev bash -c 'cd /home/bosley/git/Natron && \
        NATRON_GUI_TEST_OUT=build/m397-shots/out GUI_TEST_TIMEOUT=120 \
        Tests/gui/run-gui-test.sh Tests/gui/m397_label_resize_anchor.py build/fast/App/Natron'
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
LOAD_TIMEOUT_S = 30.0
REFRESH_TIMEOUT_S = 5.0
WATCHDOG_S = 120
SETTLE_MS = 400
TOL = 0.5

LONG_LABEL = ("a label line that is a good deal wider than the node it sits on\n"
              "second line\n"
              "third line of the label")
LONG_NAME = "Blur_with_a_much_longer_name_than_the_default_one"
BACKDROP_LABEL = ("a backdrop label line that is a good deal wider than the backdrop\n"
                  "second line\n"
                  "third line")

try:
    import NatronEngine

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def create(plugin_id, x, y):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def geometry(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return (x, y, w, h)

    def size_differs(node, size):
        return tuple(node.getSize()) != tuple(size)

    def check_top_centre(label, before, after):
        bx, by, bw, bh = before
        ax, ay, aw, ah = after
        report("NOTE %s: %r -> %r" % (label, before, after))
        check(aw != bw and ah != bh, "%s: width and height change (%gx%g -> %gx%g)" % (label, bw, bh, aw, ah))
        check(abs(ay - by) <= TOL, "%s: the top edge stays at y=%g (got %g)" % (label, by, ay))
        check(abs((ax + aw / 2.0) - (bx + bw / 2.0)) <= TOL,
              "%s: the horizontal centre stays at %g (got %g)" % (label, bx + bw / 2.0, ax + aw / 2.0))

    def check_top_left(label, before, after):
        report("NOTE %s: %r -> %r" % (label, before, after))
        check(abs(after[0] - before[0]) <= TOL and abs(after[1] - before[1]) <= TOL,
              "%s: the top-left stays at (%g, %g) (got (%g, %g))" % (label, before[0], before[1], after[0], after[1]))

    def string_param(node, names):
        for name in names:
            p = node.getParam(name)
            if p is not None:
                return p
        return None

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        node = create("Blur", 0, 0)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        original = geometry(node)
        original_name = node.getLabel()
        report("NOTE original geometry %r, name %r" % (original, original_name))
        label = node.getParam("userTextArea")
        gt.require(label is not None, "the Blur has a label (userTextArea) param")

        gt.section("a-label")
        label.set(LONG_LABEL)
        yield ("until", lambda: size_differs(node, original[2:]), REFRESH_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        labelled = geometry(node)
        check_top_centre("a-label", original, labelled)

        gt.section("b-rename")
        node.setLabel(LONG_NAME)
        yield ("until", lambda: size_differs(node, labelled[2:]), REFRESH_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        renamed = geometry(node)
        check_top_centre("b-rename", labelled, renamed)

        gt.section("c-clear")
        node.setLabel(original_name)
        label.set("")
        yield ("until", lambda: not size_differs(node, original[2:]), REFRESH_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        cleared = geometry(node)
        report("NOTE c-clear: %r -> %r" % (renamed, cleared))
        check(cleared[2:] == original[2:], "c-clear: the size returns to %r (got %r)" % (original[2:], cleared[2:]))
        check(abs(cleared[0] - original[0]) <= TOL and abs(cleared[1] - original[1]) <= TOL,
              "c-clear: the position returns to (%g, %g) (got (%g, %g))"
              % (original[0], original[1], cleared[0], cleared[1]))

        gt.section("d-setsize")
        sized = create("Blur", 400, 0)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        before = geometry(sized)
        sized.setSize(before[2] + 61, before[3] + 20)
        yield ("sleep", SETTLE_MS)
        after = geometry(sized)
        check(after[2:] != before[2:], "d-setsize: setSize() changes the size (%r -> %r)" % (before[2:], after[2:]))
        check_top_left("d-setsize", before, after)

        gt.section("e-backdrop")
        backdrop = create("Backdrop", 0, 400)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        bd_label = string_param(backdrop, ("Label", "label", "userTextArea"))
        gt.require(bd_label is not None, "the Backdrop has a label param")
        before = geometry(backdrop)
        bd_label.set(BACKDROP_LABEL)
        backdrop.setLabel("Backdrop_with_a_much_longer_name_than_the_default_one")
        yield ("sleep", SETTLE_MS)
        check_top_left("e-backdrop", before, geometry(backdrop))

        gt.section("f-reload")
        label.set(LONG_LABEL)
        yield ("until", lambda: size_differs(node, original[2:]), REFRESH_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        nodes = [node, sized, backdrop]
        saved = dict((n.getScriptName(), tuple(n.getPosition())) for n in nodes)
        report("NOTE saved positions %r" % (saved,))
        path = os.path.join(gt.OUT, "m397_label_resize_anchor.ntpz")
        gt.require(app.saveProjectAs(path), "save the project to " + path)
        gt.require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        reloaded = app.loadProject(path)
        gt.require(reloaded is not None, "reopen " + path)
        host = reloaded
        ok = yield ("until", lambda: all(host.getNode(name) is not None for name in saved), LOAD_TIMEOUT_S)
        gt.require(ok, "every node is back after reopening")
        yield ("sleep", SETTLE_MS)
        for name, pos in sorted(saved.items()):
            got = tuple(host.getNode(name).getPosition())
            check(got == pos, "f-reload: %s comes back at %r (got %r)" % (name, pos, got))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
