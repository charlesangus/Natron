"""A node whose own content changes size (label, name) keeps its top edge and horizontal centre, so
it stays lined up with its neighbours. User resizes through setSize(), backdrops and a save/reload
keep the top-left corner, and an explicitly sized node keeps its size when its label is short.

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
STEP_TIMEOUT_S = 1.0
TOL = 1e-6
PROBE_MARGIN = 20.0
EXPLICIT_SIZE = (200.0, 300.0)

LONG_LABEL = ("a label line that is a good deal wider than the node it sits on\n"
              "second line\n"
              "third line of the label")
LONG_NAME = "Blur_with_a_much_longer_name_than_the_default_one"
ODD_WIDTH_LABELS = ["odd width probe " + "abcdefghijklmnop"[:k] for k in range(17)]
BACKDROP_LABEL = "\n".join("backdrop line %d" % i for i in range(1, 25))

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

    def node_covers(node, scene_x, scene_y):
        """Whether one of node's own graphics items lies under a scene point.

        getSize() reports the size setSize() stored, not the laid-out one, so an explicitly sized
        node's drawn extent is read from the scene. Items reaching above the node or outside its
        horizontal span (input arrows, view overlays) are not the node's body."""
        graph = root_graph()
        if graph is None:
            return False
        x, y = node.getPosition()
        w = node.getSize()[0]
        try:
            for item in graph.items(graph.mapFromScene(scene_x, scene_y)):
                r = item.sceneBoundingRect()
                if r.left() >= x - 5 and r.right() <= x + w + 5 and r.top() >= y - 5:
                    return True
        except RuntimeError:
            return False
        return False

    def check_top_centre(label, before, after, height_changes=True):
        bx, by, bw, bh = before
        ax, ay, aw, ah = after
        report("NOTE %s: %r -> %r" % (label, before, after))
        if height_changes:
            check(aw != bw and ah != bh, "%s: width and height change (%gx%g -> %gx%g)" % (label, bw, bh, aw, ah))
        else:
            check(aw != bw, "%s: the width changes (%gx%g -> %gx%g)" % (label, bw, bh, aw, ah))
        check(abs(ay - by) <= TOL, "%s: the top edge stays at y=%g (got %g)" % (label, by, ay))
        check(abs((ax + aw / 2.0) - (bx + bw / 2.0)) <= TOL,
              "%s: the horizontal centre stays at %g (got %g)" % (label, bx + bw / 2.0, ax + aw / 2.0))

    def check_restored(label, original, got):
        check(got[2:] == original[2:], "%s: the size returns to %r (got %r)" % (label, original[2:], got[2:]))
        check(abs(got[0] - original[0]) <= TOL and abs(got[1] - original[1]) <= TOL,
              "%s: the position returns to (%g, %g) (got (%g, %g))" % (label, original[0], original[1], got[0], got[1]))

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

        graph = root_graph()
        graph.resetTransform()
        del graph

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
        named = create("Blur", -400, 0)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        named_original = geometry(named)
        named_original_name = named.getLabel()
        named.setLabel(LONG_NAME)
        yield ("until", lambda: size_differs(named, named_original[2:]), REFRESH_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        renamed = geometry(named)
        check_top_centre("b-rename", named_original, renamed, height_changes=False)

        gt.section("c-clear")
        label.set("")
        named.setLabel(named_original_name)
        yield ("until", lambda: not size_differs(node, original[2:]) and not size_differs(named, named_original[2:]),
               REFRESH_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        cleared = geometry(node)
        report("NOTE c-clear label: %r -> %r" % (labelled, cleared))
        check_restored("c-clear label", original, cleared)
        named_cleared = geometry(named)
        report("NOTE c-clear name: %r -> %r" % (renamed, named_cleared))
        check_restored("c-clear name", named_original, named_cleared)

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

        gt.section("d-explicit-size-label")
        explicit = create("Blur", 800, 0)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        explicit.setSize(*EXPLICIT_SIZE)
        yield ("sleep", SETTLE_MS)
        before = geometry(explicit)
        ex, ey = before[0], before[1]
        bottom_probe = (ex + EXPLICIT_SIZE[0] / 2.0, ey + EXPLICIT_SIZE[1] - PROBE_MARGIN)
        corner_probe = (ex + EXPLICIT_SIZE[0] - PROBE_MARGIN, ey + EXPLICIT_SIZE[1] - PROBE_MARGIN)
        check(node_covers(explicit, *bottom_probe) and node_covers(explicit, *corner_probe),
              "d-explicit-size-label: the node is drawn %gx%g after setSize()" % EXPLICIT_SIZE)
        explicit_label = explicit.getParam("userTextArea")
        gt.require(explicit_label is not None, "the explicitly sized Blur has a label param")
        explicit_label.set("short")
        yield ("sleep", SETTLE_MS)
        after = geometry(explicit)
        report("NOTE d-explicit-size-label: %r -> %r" % (before, after))
        check(node_covers(explicit, *bottom_probe) and node_covers(explicit, *corner_probe),
              "d-explicit-size-label: a short label keeps the node drawn at least %gx%g" % EXPLICIT_SIZE)
        check(after[2:] == before[2:], "d-explicit-size-label: the size stays %r (got %r)" % (before[2:], after[2:]))
        check(after[0] == before[0] and after[1] == before[1],
              "d-explicit-size-label: the top-left stays at (%g, %g) (got (%g, %g))"
              % (before[0], before[1], after[0], after[1]))

        gt.section("e-backdrop")
        backdrop = create("Backdrop", 0, 400)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        bd_label = string_param(backdrop, ("Label", "label", "userTextArea"))
        gt.require(bd_label is not None, "the Backdrop has a label param")
        before = geometry(backdrop)
        grown_probe = (before[0] + PROBE_MARGIN, before[1] + before[3] + 3 * PROBE_MARGIN)
        check(not node_covers(backdrop, *grown_probe),
              "e-backdrop: before the label, nothing of the backdrop lies at %r" % (grown_probe,))
        bd_label.set(BACKDROP_LABEL)
        backdrop.setLabel("Backdrop_with_a_much_longer_name_than_the_default_one")
        ok = yield ("until", lambda: node_covers(backdrop, *grown_probe), REFRESH_TIMEOUT_S)
        yield ("sleep", SETTLE_MS)
        after = geometry(backdrop)
        check(ok, "e-backdrop: the tall label grows the backdrop down past %r (getSize %r -> %r)"
              % (grown_probe, before[2:], after[2:]))
        check_top_left("e-backdrop", before, after)

        gt.section("f-odd-width")
        start = geometry(node)
        check(start == original, "f-odd-width: the node starts back at %r (got %r)" % (original, start))
        odd = None
        previous = start
        for text in ODD_WIDTH_LABELS:
            label.set(text)
            yield ("until", lambda: size_differs(node, previous[2:]), STEP_TIMEOUT_S)
            yield ("sleep", SETTLE_MS)
            now = geometry(node)
            expected_x = start[0] - (now[2] - start[2]) / 2.0
            report("NOTE f-odd-width %r: %r, expected x %r" % (text, now, expected_x))
            check(abs(now[0] - expected_x) <= TOL and abs(now[1] - start[1]) <= TOL,
                  "f-odd-width %r: the node sits at (%r, %r) (got (%r, %r))"
                  % (text, expected_x, start[1], now[0], now[1]))
            previous = now
            if (now[2] - start[2]) % 2 == 1:
                odd = now
                break
        gt.require(odd is not None, "f-odd-width: one of the probe labels changes the width by an odd amount")
        check((odd[0] - start[0]) % 1 == 0.5,
              "f-odd-width: an odd width change moves x by a half pixel (%r -> %r)" % (start[0], odd[0]))

        gt.section("g-reload")
        nodes = [node, named, sized, explicit, backdrop]
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
            check(got == pos, "g-reload: %s comes back at %r (got %r)" % (name, pos, got))

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
