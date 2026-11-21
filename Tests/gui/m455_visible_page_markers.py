"""GUI check that the timeline shows only the keys of foreground panels' visible pages.

Graph: a Roto whose Bezier has shape keys and a keyed item translate (on the Transform page), and a
Transform node whose translate is keyed on its first page. The script reads the markers through
TimeLineGui::keyframeMarkerTimes() and, after every step, compares them with the expected frames:
  - pages: switching the Roto between its General and Transform pages drops and brings back the
    translate keys while the shape keys stay; the Transform node's keys go on its Node page;
  - roto-items: only the selected Bezier's keys show, none once nothing is selected; a key on the
    node's translate reaches the selected Bezier's own translate and shows;
  - minimise: a minimised panel is skipped: alone it shows nothing, over another panel the next expanded
    one shows, and expanding it brings its keys back;
  - order: opening a second panel puts it on top and only its keys show; bringing the first one
    back to the top swaps them;
  - floating: a floating panel shows its keys alongside the top docked panel's, and still follows
    its own visible page;
  - close: closing the panels clears the markers;
  - sub-tabs: a CornerPin keyed on its To and From sub-tabs, which share one page, shows only the
    keys of the sub-tab in view, and none while another page is shown.

Run through Tests/gui/run-gui-test.sh. Screenshots of the timeline and results.txt go to
$NATRON_GUI_TEST_OUT (default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 15.0
WATCHDOG_S = 280
SETTLE_MS = 300
RENDER_MS = 500

SHAPE_KEYS = [1, 20]
ITEM_TRANSLATE_KEYS = [9, 30]
TRANSFORM_KEYS = [5, 15]
ROTO_ALL = sorted(SHAPE_KEYS + ITEM_TRANSLATE_KEYS)
OTHER_TRANSLATE_KEY = 40
PIN_TO_KEYS = [3, 12]
PIN_FROM_KEYS = [7, 18]

try:
    import NatronEngine
    from PySide6.QtCore import QItemSelectionModel, QMetaObject, Q_ARG, Q_RETURN_ARG, Qt
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import QApplication, QLineEdit, QTabWidget, QTreeWidget

    import guitest as gt
    from guitest import check, report, require
    import roto_gui as rg

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _held = []

    def hold(obj):
        _held.append(obj)
        return obj

    def timeline():
        visible = [w for w in gt.widgets_of("TimeLineGui") if w.isVisible()]
        return visible[0] if visible else None

    def markers():
        w = timeline()
        if w is None:
            return None
        return [int(t) for t in QMetaObject.invokeMethod(w, "keyframeMarkerTimes", Qt.DirectConnection,
                                                         Q_RETURN_ARG("QVariantList"))]

    def timeline_shot(name):
        """Grabs the screen area of the timeline, whose markers are too small to read in a full shot."""
        w = timeline()
        if w is None:
            report("NOTE no timeline for screenshot " + name)
            return
        QApplication.processEvents()
        origin = w.mapToGlobal(w.rect().topLeft())
        path = os.path.join(gt.OUT, name)
        if QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), w.width(), w.height()).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def panel_of(node):
        label = node.getLabel()
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible():
                    continue
                for le in panel.findChildren(QLineEdit):
                    if gt.class_name(le) == "LineEdit" and le.text() == label:
                        return hold(panel)
            except RuntimeError:
                continue
        return None

    def page_tabs(node):
        panel = panel_of(node)
        require(panel is not None, "the settings panel of %s is open" % node.getLabel())
        for tabs in panel.findChildren(QTabWidget):
            labels = [tabs.tabText(i).replace("&", "") for i in range(tabs.count())]
            if "Node" in labels:
                return hold(tabs)
        return None

    def show_page(node, page_label):
        tabs = page_tabs(node)
        require(tabs is not None, "the panel of %s has pages" % node.getLabel())
        for i in range(tabs.count()):
            if tabs.tabText(i).replace("&", "") == page_label:
                tabs.setCurrentIndex(i)
                QApplication.processEvents()
                return
        require(False, "the panel of %s has a %s page" % (node.getLabel(), page_label))

    def show_subtab(node, tab_label):
        panel = panel_of(node)
        require(panel is not None, "the settings panel of %s is open" % node.getLabel())
        for tabs in panel.findChildren(QTabWidget):
            labels = [tabs.tabText(i).replace("&", "") for i in range(tabs.count())]
            if "Node" in labels or tab_label not in labels:
                continue
            tabs.setCurrentIndex(labels.index(tab_label))
            QApplication.processEvents()
            return
        require(False, "the panel of %s has a %s sub-tab" % (node.getLabel(), tab_label))

    def show_first_page(node):
        tabs = page_tabs(node)
        require(tabs is not None and tabs.count() > 0, "the panel of %s has pages" % node.getLabel())
        tabs.setCurrentIndex(0)
        QApplication.processEvents()
        return tabs.tabText(0).replace("&", "")

    def open_panel(node):
        graphs = gt.widgets_of("NodeGraph")
        require(graphs, "found the node graph")
        app.setSelection([node])
        QMetaObject.invokeMethod(graphs[0], "showSelectedNodeSettingsPanel")
        ok = yield ("until", lambda: panel_of(node) is not None, POLL_TIMEOUT_S)
        require(ok, "opened the settings panel of " + node.getLabel())
        app.clearSelection()
        QApplication.processEvents()

    def close_panel(node):
        panel = panel_of(node)
        require(panel is not None, "the settings panel of %s is open" % node.getLabel())
        QMetaObject.invokeMethod(panel, "closePanel")
        ok = yield ("until", lambda: panel_of(node) is None, POLL_TIMEOUT_S)
        require(ok, "closed the settings panel of " + node.getLabel())

    def set_minimized(node, minimized):
        panel = panel_of(node)
        require(panel is not None, "the settings panel of %s is open" % node.getLabel())
        QMetaObject.invokeMethod(panel, "minimizeOrMaximize", Q_ARG(bool, minimized))
        QApplication.processEvents()

    def is_floating(node):
        panel = panel_of(node)
        return panel is not None and panel.window() is not rg.main_window()

    def float_panel(node):
        panel = panel_of(node)
        require(panel is not None, "the settings panel of %s is open" % node.getLabel())
        QMetaObject.invokeMethod(panel, "floatPanel")
        ok = yield ("until", lambda: is_floating(node), POLL_TIMEOUT_S)
        require(ok, "floated the settings panel of " + node.getLabel())
        # A floating window takes activation, and an inactive main window drops graph clicks.
        rg.activate()
        yield ("sleep", SETTLE_MS)

    def bin_order():
        win = rg.main_window()
        out = []
        for panel in gt.widgets_of("NodeSettingsPanel"):
            try:
                if not panel.isVisible() or panel.window() is not win:
                    continue
                for le in panel.findChildren(QLineEdit):
                    if gt.class_name(le) == "LineEdit":
                        out.append((panel.mapTo(win, panel.rect().topLeft()).y(), le.text()))
                        break
            except RuntimeError:
                continue
        return [label for _y, label in sorted(out)]

    def curve_tree(node):
        panel = panel_of(node)
        if panel is None:
            return None
        for tree in panel.findChildren(QTreeWidget):
            parent = tree.parentWidget()
            while parent is not None and parent is not panel:
                if gt.class_name(parent) == "RotoPanel":
                    return hold(tree)
                parent = parent.parentWidget()
        return None

    def select_row(node, item):
        tree = curve_tree(node)
        require(tree is not None, "the RotoPanel of %s is open" % node.getLabel())
        tree.expandAll()
        model = tree.model()

        def walk(parent):
            for r in range(model.rowCount(parent)):
                index = model.index(r, 0, parent)
                if str(index.data()) in (item.getLabel(), item.getScriptName()):
                    return index
                found = walk(index)
                if found is not None:
                    return found
            return None
        index = walk(tree.rootIndex())
        require(index is not None, "the curve list has a row for " + item.getLabel())
        selection = tree.selectionModel()
        selection.clear()
        selection.select(index, QItemSelectionModel.Select | QItemSelectionModel.Rows)
        QApplication.processEvents()

    def clear_selection(node):
        tree = curve_tree(node)
        require(tree is not None, "the RotoPanel of %s is open" % node.getLabel())
        tree.selectionModel().clear()
        QApplication.processEvents()

    def key_times(param):
        times = set()
        for d in range(param.getNumDimensions()):
            for i in range(param.getNumKeys(d)):
                found, t = param.getKeyTime(i, d)
                if found:
                    times.add(int(t))
        return sorted(times)

    def expect(label, frames):
        ok = yield ("until", lambda: markers() == frames, POLL_TIMEOUT_S)
        # A late refresh must not bring back frames the timeline has dropped.
        yield ("sleep", SETTLE_MS)
        got = markers()
        check(ok and got == frames, "%s: markers %r match %r" % (label, got, frames))

    def steps():
        ok = yield ("until", lambda: bool(gt.widgets_of("NodeGraph")) and rg.main_window() is not None,
                    STARTUP_TIMEOUT_S)
        require(ok, "node graph widget is up")
        viewer = app.getNode("Viewer1") or app.createNode("Viewer")
        require(viewer is not None, "a Viewer exists")
        ok = yield ("until", lambda: gt.viewer_up() and timeline() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the viewer and its timeline are visible")
        app.getProjectParam("frameRange").set(1, 100)
        gt.close_all_panels()
        rg.activate()
        yield ("sleep", SETTLE_MS)

        gt.section("setup")
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(False)}
        transform = app.createNode("Transform", -1, None, props)
        require(transform is not None, "create a Transform", gt.plugin_problem("Transform"))
        translate = transform.getParam("translate")
        require(translate is not None, "the Transform has a translate knob")
        translate.setValueAtTime(10.0, TRANSFORM_KEYS[0], 0)
        translate.setValueAtTime(40.0, TRANSFORM_KEYS[1], 0)

        roto, ctx = rg.open_roto(app, "Roto")
        require(roto is not None and ctx is not None, "create a Roto", gt.plugin_problem("Roto"))
        pts = [(100.0, 100.0), (300.0, 100.0), (300.0, 300.0), (100.0, 300.0)]
        bez = ctx.createBezier(pts[0][0], pts[0][1], SHAPE_KEYS[0])
        require(bez is not None, "created a Bezier")
        for x, y in pts[1:]:
            bez.addControlPoint(x, y)
        bez.setCurveFinished(True)
        for t in SHAPE_KEYS:
            for i, (x, y) in enumerate(pts):
                bez.setPointAtIndex(i, t, x + t, y, x + t - 20.0, y, x + t + 20.0, y)
        item_translate = bez.getParam("translate")
        require(item_translate is not None, "the Bezier has a translate knob")
        item_translate.setValueAtTime(5.0, ITEM_TRANSLATE_KEYS[0], 0)
        item_translate.setValueAtTime(25.0, ITEM_TRANSLATE_KEYS[1], 0)
        ok = yield ("until", lambda: (sorted(int(k) for k in bez.getKeyframes()) == SHAPE_KEYS
                                      and key_times(item_translate) == ITEM_TRANSLATE_KEYS
                                      and key_times(translate) == TRANSFORM_KEYS), POLL_TIMEOUT_S)
        require(ok, "the keys are set", "%r, %r, %r" % (list(bez.getKeyframes()), key_times(item_translate),
                                                        key_times(translate)))
        ok = yield ("until", lambda: panel_of(roto) is not None, POLL_TIMEOUT_S)
        require(ok, "the Roto's panel is open")
        yield ("sleep", RENDER_MS)
        select_row(roto, bez)

        gt.section("pages")
        show_page(roto, "General")
        yield from expect("Roto on its General page: the shape keys only", SHAPE_KEYS)
        timeline_shot("m455-roto-general-page.png")
        show_page(roto, "Transform")
        yield from expect("Roto on its Transform page: shape and translate keys", ROTO_ALL)
        timeline_shot("m455-roto-transform-page.png")
        show_page(roto, "General")
        yield from expect("Roto back on its General page", SHAPE_KEYS)
        show_page(roto, "Transform")
        yield from expect("Roto on its Transform page again", ROTO_ALL)

        gt.section("roto-items")
        clear_selection(roto)
        yield from expect("no Roto item selected: the node's knobs keep no item's keys", [])
        select_row(roto, bez)
        yield from expect("the Bezier selected again", ROTO_ALL)
        other = ctx.createBezier(400.0, 100.0, SHAPE_KEYS[0])
        require(other is not None, "created a second Bezier")
        for x, y in ((500.0, 100.0), (500.0, 300.0), (400.0, 300.0)):
            other.addControlPoint(x, y)
        other.setCurveFinished(True)
        other_shape = sorted(int(k) for k in other.getKeyframes())
        ok = yield ("until", lambda: curve_tree(roto) is not None, POLL_TIMEOUT_S)
        require(ok, "the curve list is up after adding a Bezier")
        select_row(roto, other)
        yield from expect("the second Bezier selected: its own keys only", other_shape)
        node_translate = roto.getParam("translate")
        require(node_translate is not None, "the Roto has a translate knob")
        node_translate.setValueAtTime(7.0, OTHER_TRANSLATE_KEY, 0)
        other_translate = other.getParam("translate")
        ok = yield ("until", lambda: OTHER_TRANSLATE_KEY in key_times(other_translate), POLL_TIMEOUT_S)
        check(ok, "a key on the node's translate reaches the selected Bezier's own translate (%r)" %
              (key_times(other_translate),))
        check(abs(other_translate.getValueAtTime(OTHER_TRANSLATE_KEY, 0) - 7.0) < 1e-6,
              "the second Bezier's translate is 7 at frame %d" % OTHER_TRANSLATE_KEY)
        check(key_times(item_translate) == ITEM_TRANSLATE_KEYS,
              "the first Bezier's translate keeps its keys %r (%r)" % (ITEM_TRANSLATE_KEYS, key_times(item_translate)))
        yield from expect("the second Bezier's new translate key", sorted(set(other_shape + [OTHER_TRANSLATE_KEY])))
        show_page(roto, "General")
        yield from expect("the second Bezier on the General page", other_shape)
        show_page(roto, "Transform")
        select_row(roto, bez)
        yield from expect("back to the first Bezier: the second one's key goes", ROTO_ALL)
        clear_selection(roto)
        yield from expect("nothing selected after showing two Beziers", [])
        select_row(roto, bez)

        gt.section("minimise")
        set_minimized(roto, True)
        yield from expect("Roto minimised", [])
        set_minimized(roto, False)
        yield from expect("Roto expanded", ROTO_ALL)

        gt.section("order")
        yield from open_panel(transform)
        first = show_first_page(transform)
        check(bin_order()[:1] == [transform.getLabel()], "the Transform's panel opened on top (%r)" % (bin_order(),))
        yield from expect("Transform on top, on its %s page" % first, TRANSFORM_KEYS)
        show_page(transform, "Node")
        yield from expect("Transform on top, on its Node page", [])
        show_page(transform, first)
        yield from expect("Transform on top, back on its %s page" % first, TRANSFORM_KEYS)
        yield from open_panel(roto)
        check(bin_order()[:1] == [roto.getLabel()], "the Roto's panel is back on top (%r)" % (bin_order(),))
        yield from expect("Roto back on top", ROTO_ALL)
        set_minimized(roto, True)
        yield from expect("Roto minimised on top of the Transform: the Transform shows", TRANSFORM_KEYS)
        set_minimized(roto, False)
        yield from expect("Roto expanded on top of the Transform", ROTO_ALL)
        yield from open_panel(transform)
        yield from expect("Transform on top again", TRANSFORM_KEYS)

        gt.section("floating")
        yield from open_panel(roto)
        yield from expect("Roto on top before floating the Transform", ROTO_ALL)
        yield from float_panel(transform)
        yield from expect("Transform floating, Roto on top: the union", sorted(ROTO_ALL + TRANSFORM_KEYS))
        timeline_shot("m455-floating-union.png")
        show_page(transform, "Node")
        yield from expect("floating Transform on its Node page", ROTO_ALL)
        show_page(transform, first)
        yield from expect("floating Transform back on its %s page" % first, sorted(ROTO_ALL + TRANSFORM_KEYS))
        show_page(roto, "General")
        yield from expect("Roto on its General page beside the floating Transform",
                          sorted(SHAPE_KEYS + TRANSFORM_KEYS))
        set_minimized(transform, True)
        yield from expect("floating Transform minimised", SHAPE_KEYS)
        set_minimized(transform, False)
        yield from expect("floating Transform expanded", sorted(SHAPE_KEYS + TRANSFORM_KEYS))

        gt.section("close")
        yield from close_panel(transform)
        check(panel_of(transform) is None, "the floating Transform's panel is not visible")
        check(transform.getLabel() not in bin_order(), "the closed Transform is not in the properties bin (%r)" %
              (bin_order(),))
        check(bin_order()[:1] == [roto.getLabel()], "the Roto's panel is on top (%r)" % (bin_order(),))
        yield from expect("the floating Transform closed: the Roto's visible page", SHAPE_KEYS)
        yield from open_panel(transform)
        check(not is_floating(transform), "the reopened Transform's panel is docked")
        check(bin_order()[:1] == [transform.getLabel()], "the reopened Transform's panel is on top (%r)" %
              (bin_order(),))
        yield from expect("the Transform reopened on top", TRANSFORM_KEYS)
        yield from close_panel(transform)
        yield from expect("the Transform closed", SHAPE_KEYS)
        yield from close_panel(roto)
        yield from expect("every panel closed", [])

        gt.section("sub-tabs")
        pin = app.createNode("CornerPin", -1, None, props)
        require(pin is not None, "create a CornerPin", gt.plugin_problem("CornerPin"))
        to1 = pin.getParam("to1")
        from1 = pin.getParam("from1")
        require(to1 is not None and from1 is not None, "the CornerPin has to1 and from1 knobs")
        for t in PIN_TO_KEYS:
            to1.setValueAtTime(float(t), t, 0)
        for t in PIN_FROM_KEYS:
            from1.setValueAtTime(float(t), t, 0)
        ok = yield ("until", lambda: key_times(to1) == PIN_TO_KEYS and key_times(from1) == PIN_FROM_KEYS,
                    POLL_TIMEOUT_S)
        require(ok, "the CornerPin keys are set", "%r, %r" % (key_times(to1), key_times(from1)))
        yield from open_panel(pin)
        pin_page = show_first_page(pin)
        show_subtab(pin, "To")
        yield from expect("CornerPin on its To sub-tab", PIN_TO_KEYS)
        timeline_shot("m455-cornerpin-to-subtab.png")
        show_subtab(pin, "From")
        yield from expect("CornerPin on its From sub-tab", PIN_FROM_KEYS)
        timeline_shot("m455-cornerpin-from-subtab.png")
        show_subtab(pin, "To")
        yield from expect("CornerPin back on its To sub-tab", PIN_TO_KEYS)
        show_page(pin, "Node")
        yield from expect("CornerPin on its Node page", [])
        show_subtab(pin, "From")
        yield from expect("CornerPin's From sub-tab switched while its Node page shows", [])
        show_page(pin, pin_page)
        yield from expect("CornerPin back on its %s page, on the From sub-tab" % pin_page, PIN_FROM_KEYS)
        yield from close_panel(pin)
        yield from expect("the CornerPin closed", [])
        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
