"""The node graph's GPU region overlay on CheckerBoard -> Blur -> Grade -> TimeBlur -> Blur -> Saturation -> Viewer.

TimeBlur has no GPU path, so on a GPU host the chain splits into a region above it and one below it: an exit with
a red X and "no GPU path" where the upper region feeds TimeBlur, and an entry tick where TimeBlur feeds the lower
one. Placement is weight-based: a region of Point nodes alone (Saturation, Grade) weighs under kGpuRegionMinWeight
and stays on the CPU, so each side of the TimeBlur has a Blur, a Filter that is enough on its own. The overlay's
outline item publishes what it draws as properties (regionCount, exitCount, entryCount, fallbackCount, fadedCount,
fadedNodes, mixedCount, exitLabels, regionRects, pipeLines, refreshCount, redrawCount), which the checks read.
regionRects holds one scene rectangle per region ("x,y,w,h"), pipeLines one "region,x1,y1,x2,y2" per marked pipe;
refreshCount counts full rebuilds of the overlay and redrawCount the single-region redraws. A faded node is covered by a child item whose data(0) is
"gpuFadeScrim"; its own opacity stays 1.

Whether a GPU is usable is read from natron.log: "GPU compute ready on" or "GPU compute is unavailable:".

Cases:
  toggle     the Show GPU Regions preference, Shift+G in the node graph and the node graph's View > Show GPU
             Regions action set one state, and the overlay shows and hides with it
  no GPU     with no usable GPU (the remote --gui jobs: auto mode refuses lavapipe) the overlay draws no region
             and the viewer shows no error
  mixed      with a usable GPU, the chain places at least one region, the regions round the TimeBlur draw at
             least one exit or entry, every exit label is one of the overlay's reasons, and TimeBlur's input
             reads "no GPU path"
  cached     with a GPU, editing the lower Saturation re-renders only it: every node above it, TimeBlur included,
             is served from the cache and faded, Saturation stays coloured, no item of the graph has its opacity
             lowered, and the pipes inside a region stay below the nodes
  fallback   with NATRON_GPU_FAULT=lost@region:1 the first operation of the first GPU region run loses the device,
             and that region gets the fallback outline. The preference is on from the start in this mode, so that
             the render the fault lands in is one the overlay records
  moved      with a GPU, moving a region's member node redraws that region's outline round the node and leaves the
             other regions' outlines alone, without a full rebuild
  selection  with a GPU, selecting and deselecting every node rebuilds nothing and changes no outline or pipe
  low detail with a GPU, zoomed out into the low level of detail, the faded node's pixels differ with the overlay on
             and return to the unfaded pixels with it off; screenshots m116_lowdetail_on.png and m116_lowdetail_off.png
  inputs     with a GPU, hiding a node's inputs removes the marked pipe at that node, and showing them puts it back
  stable     with a GPU, moving the main window changes neither the regions nor the fading, and turning the
             preference off and on shows the regions again without a render or an edit

Screenshots in $NATRON_GUI_TEST_OUT, one per state: m116_off.png, m116_on.png, m116_no_gpu.png or
m116_mixed.png, m116_cached.png, m116_retoggled.png, m116_fallback.png. Each has a NOTE caption in results.txt.

Usage (no GPU, as the remote --gui jobs run it):
    Tests/gui/run-gui-test.sh Tests/gui/m116_gpu_overlay.py
With a GPU (natron-dev-gpu for a hardware device; lavapipe's cost model may keep few regions on the GPU):
    docker exec -e NATRON_GPU=force -w /home/bosley/git/Natron natron-dev-gpu \\
        Tests/gui/run-gui-test.sh Tests/gui/m116_gpu_overlay.py build/fast/App/Natron
    docker exec -e NATRON_GPU=force -e NATRON_GPU_FAULT=lost@region:1 -w /home/bosley/git/Natron natron-dev-gpu \\
        Tests/gui/run-gui-test.sh Tests/gui/m116_gpu_overlay.py build/fast/App/Natron
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 20.0
STARTUP_TIMEOUT_S = 60.0
FALLBACK_ATTEMPTS = 8
GPU_STATE_TIMEOUT_S = 20.0
GPU_READY_LINE = "GPU compute ready on"
GPU_UNAVAILABLE_LINE = "GPU compute is unavailable:"
BLUR_SIZE = 6.0
NODE_SPACING = 92
GPU_ENV = os.environ.get("NATRON_GPU", "")
FAULT_ENV = os.environ.get("NATRON_GPU_FAULT", "")
PREF_NAME = "showGpuRegions"
SCRIM_NAME = "gpuFadeScrim"
LINE_ITEM_TYPE = 6
ACTION_TEXT = "Show GPU Regions"
REASONS = ("region split", "graph has a cycle", "no GPU path", "no downscale", "cached", "identity",
           "raw run on the CPU", "would form a cycle", "cost model")

try:
    import NatronEngine
    from PySide6.QtCore import QPoint, Qt, QTimer
    from PySide6.QtTest import QTest
    from PySide6.QtGui import QFontMetrics
    from PySide6.QtWidgets import QApplication

    import guitest as gt
    from guitest import check, menu_of, require

    _held = []

    def hold(w):
        """Keeps w's wrapper alive: a child wrapper dies with a temporary parent wrapper."""
        if w is not None and not any(x is w for x in _held):
            _held.append(w)
        return w

    pref = hold(NatronEngine.natron.getSettings().getParam(PREF_NAME))

    def node_graph_view():
        views = [v for v in gt.widgets_of("NodeGraph") if v.isVisible()]
        return hold(views[0]) if views else None

    def overlay_item(name="GpuRegionOutlineItem"):
        view = node_graph_view()
        if view is None:
            return None
        # NodeGraph.scene() raises "already deleted" in this binary; the view lists its items itself.
        try:
            for item in view.items():
                obj = item.toGraphicsObject()
                if obj is not None and obj.objectName() == name:
                    return hold(obj)
        except RuntimeError:
            return None
        return None

    def prop(name):
        item = overlay_item()
        if item is None:
            return 0
        value = item.property(name)
        if name in ("exitLabels", "fadedNodes", "regionRects", "pipeLines"):
            return [str(v) for v in value] if value else []
        return int(value) if value is not None else 0

    def region_rects():
        return [tuple(float(v) for v in text.split(",")) for text in prop("regionRects")]

    def pipe_lines():
        return [tuple(float(v) for v in text.split(",")) for text in prop("pipeLines")]

    def rect_has(rect, x, y):
        return rect[2] > 0 and rect[0] <= x <= rect[0] + rect[2] and rect[1] <= y <= rect[1] + rect[3]

    def node_box(node):
        x, y = node.getPosition()[0:2]
        w, h = node.getSize()[0:2]
        return x, y, w, h

    def node_centre(node):
        x, y, w, h = node_box(node)
        return x + w / 2.0, y + h / 2.0

    def pipes_into(node):
        """The marked pipes whose destination end (the first point) is at node's inputs."""
        x, y, w, h = node_box(node)
        pad = 20.0
        return [line for line in pipe_lines() if x - pad <= line[1] <= x + w + pad and y - pad <= line[2] <= y + h + pad]

    def viewport_pixel(view, scene_x, scene_y):
        """The colour of the node graph's viewport at a scene point, rendered from the widget itself."""
        viewport = hold(view.viewport())
        at = view.mapFromScene(scene_x, scene_y)
        c = viewport.grab().toImage().pixelColor(at.x(), at.y())
        return (c.red(), c.green(), c.blue())

    def graph_items():
        view = node_graph_view()
        if view is None:
            return []
        try:
            return list(view.items())
        except RuntimeError:
            return []

    def read_items(read):
        """read(item) for every live scene item; items deleted by an overlay rebuild mid-pass are skipped."""
        values = []
        for it in graph_items():
            try:
                values.append(read(it))
            except RuntimeError:
                continue
        return values

    def scrim_parent_depths():
        # Found from the node's side: PySide takes ownership of whatever parentItem() returns and
        # deletes the node when that wrapper dies, leaving its pipes pointing at freed memory.
        def depth(it):
            covered = any(child.data(0) == SCRIM_NAME for child in it.childItems())
            return it.zValue() if covered else None
        return [d for d in read_items(depth) if d is not None]

    def scrim_count():
        return sum(1 for hit in read_items(lambda it: it.data(0) == SCRIM_NAME) if hit)

    def shown():
        item = overlay_item()
        markers = overlay_item("GpuRegionMarkerItem")
        return item is not None and markers is not None and item.isVisible() and markers.isVisible()

    def state():
        return ("pref=%r shown=%r regions=%d exits=%d entries=%d fallbacks=%d faded=%d fadedNodes=%r mixed=%d "
                "labels=%r") % (
            pref.getValue(), shown(), prop("regionCount"), prop("exitCount"), prop("entryCount"),
            prop("fallbackCount"), prop("fadedCount"), prop("fadedNodes"), prop("mixedCount"), prop("exitLabels"))

    def gpu_log_state():
        """("ready", line) or ("unavailable", line) from the first GPU compute start-up line in natron.log, else None."""
        path = os.path.join(gt.OUT, "natron.log")
        if not os.path.isfile(path):
            return None
        with open(path, errors="replace") as f:
            for line in f:
                if GPU_READY_LINE in line:
                    return ("ready", line.strip())
                if GPU_UNAVAILABLE_LINE in line:
                    return ("unavailable", line.strip())
        return None

    def caption(name, text):
        gt.shot(name)
        gt.report("NOTE %s: %s (%s)" % (name, text, state()))

    def create(plugin_id):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        return node

    def set_all(node, name, value):
        param = node.getParam(name)
        require(param is not None, "%s has %s" % (node.getScriptName(), name))
        for d in range(param.getNumDimensions()):
            param.setValue(value, d)

    def open_view_menu(menu):
        """Right-clicks the node graph's empty corner; the graph's context menu is modal, so the click is posted."""
        view = node_graph_view()
        viewport = hold(view.viewport())
        corner = QPoint(12, 12)

        def press():
            QTest.mousePress(viewport, Qt.RightButton, Qt.NoModifier, corner)
            QTest.mouseRelease(viewport, Qt.RightButton, Qt.NoModifier, corner)
            menu["closed"] = True

        QTimer.singleShot(0, press)

    def gpu_action(popup):
        view_entry = [a for a in popup.actions() if a.text().replace("&", "") == "View"]
        submenu = menu_of(hold(view_entry[0])) if view_entry else None
        if submenu is None:
            return None
        actions = [a for a in submenu.actions() if a.text().replace("&", "") == ACTION_TEXT]
        return hold(actions[0]) if actions else None

    def toggle_through_menu(trigger):
        """Opens the View menu, records whether its action is checked, triggers it if asked, and closes the menu."""
        menu = {}
        open_view_menu(menu)
        ok = yield ("until", lambda: QApplication.activePopupWidget() is not None, POLL_TIMEOUT_S)
        if not check(ok, "a right-click opens the node graph's context menu"):
            return None
        popup = hold(QApplication.activePopupWidget())
        action = gpu_action(popup)
        checked = None
        if check(action is not None, "the context menu's View submenu has %s" % ACTION_TEXT):
            check(action.isCheckable(), "%s is checkable" % ACTION_TEXT)
            checked = action.isChecked()
            if trigger:
                action.trigger()
        popup.close()
        ok = yield ("until", lambda: menu.get("closed", False), POLL_TIMEOUT_S)
        check(ok, "the context menu closes")
        return checked

    def steps():
        gt.section("setup")
        ok = yield ("until", lambda: node_graph_view() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the node graph is on screen")
        gt.close_all_panels()
        require(pref is not None, "the preferences have %s" % PREF_NAME)
        pref.setValue(bool(FAULT_ENV))
        ok = yield ("until", lambda: overlay_item() is not None and overlay_item("GpuRegionMarkerItem") is not None,
                    POLL_TIMEOUT_S)
        require(ok, "the graph has the overlay's outline and marker items")
        gt.report("NOTE NATRON_GPU=%r NATRON_GPU_FAULT=%r" % (GPU_ENV, FAULT_ENV))

        checker = create("CheckerBoard")
        upper = create("Blur")
        grade = create("Grade")
        timeblur = create("TimeBlur")
        lower_blur = create("Blur")
        lower = create("Saturation")
        viewer = app.getNode("Viewer1") or create("Viewer")
        chain = (checker, upper, grade, timeblur, lower_blur, lower)
        set_all(upper, "size", BLUR_SIZE)
        set_all(grade, "multiply", 1.5)
        set_all(lower_blur, "size", BLUR_SIZE)
        lower.getParam("saturation").setValue(0.8)
        for i, node in enumerate(chain):
            node.setPosition(0, i * NODE_SPACING)
            if i > 0:
                require(node.connectInput(0, chain[i - 1]), "connect %s" % node.getScriptName())
        viewer.setPosition(0, len(chain) * NODE_SPACING)
        require(viewer.connectInput(0, lower), "connect the Viewer to the lower Saturation")
        app.clearSelection()
        py_viewer = hold(app.getViewer(viewer.getScriptName()))
        node_graph_view().centerOn(0, len(chain) * NODE_SPACING // 2)
        yield ("sleep", 1500)

        if FAULT_ENV:
            gt.section("fallback")
            yield ("until", lambda: gpu_log_state() is not None, GPU_STATE_TIMEOUT_S)
            gpu = gpu_log_state()
            if gpu is None or gpu[0] != "ready":
                gt.report("NOTE fallback case skipped: no usable GPU (%s)" % (gpu[1] if gpu else "no start-up line"))
            else:
                fell_back = yield ("until", lambda: prop("fallbackCount") >= 1, 3.0)
                for attempt in range(FALLBACK_ATTEMPTS):
                    if fell_back:
                        break
                    # Until the GPU is up, renders place no region and the fault waits for the first that does.
                    set_all(grade, "multiply", 1.5 + 0.1 * (attempt + 1))
                    fell_back = yield ("until", lambda: prop("fallbackCount") >= 1, 3.0)
                check(fell_back, "a device fault in a region gives it the fallback outline (%s)" % state())
                caption("m116_fallback.png", "the region that lost the device has the dashed fallback outline")
            pref.setValue(False)
            yield ("until", lambda: not shown(), POLL_TIMEOUT_S)

        gt.section("toggle")
        check(not shown(), "with the preference off the overlay is hidden")
        check(prop("regionCount") == 0 and prop("exitCount") == 0 and prop("fadedCount") == 0,
              "with the preference off the overlay reports nothing (%s)" % state())
        caption("m116_off.png", "the preference off: no outlines, markers or fading")

        checked = yield from toggle_through_menu(False)
        check(checked is False, "with the preference off the View action is unchecked")

        pref.setValue(True)
        ok = yield ("until", shown, POLL_TIMEOUT_S)
        check(ok, "turning the preference on shows the overlay")
        checked = yield from toggle_through_menu(False)
        check(checked is True, "with the preference on the View action is checked")

        view = node_graph_view()
        view.setFocus()
        QTest.keyClick(view, Qt.Key_G, Qt.ShiftModifier)
        ok = yield ("until", lambda: not pref.getValue() and not shown(), POLL_TIMEOUT_S)
        check(ok, "Shift+G turns the preference off and hides the overlay (%s)" % state())
        QTest.keyClick(view, Qt.Key_G, Qt.ShiftModifier)
        ok = yield ("until", lambda: pref.getValue() and shown(), POLL_TIMEOUT_S)
        check(ok, "Shift+G again turns the preference on and shows the overlay (%s)" % state())

        yield from toggle_through_menu(True)
        ok = yield ("until", lambda: not pref.getValue() and not shown(), POLL_TIMEOUT_S)
        check(ok, "the View action turns the preference off and hides the overlay (%s)" % state())
        checked = yield from toggle_through_menu(True)
        check(checked is False, "after the View action turned it off the action is unchecked")
        ok = yield ("until", lambda: pref.getValue() and shown(), POLL_TIMEOUT_S)
        check(ok, "the View action turns the preference back on (%s)" % state())

        gt.section("placement")
        yield ("until", lambda: gpu_log_state() is not None, GPU_STATE_TIMEOUT_S)
        gpu = gpu_log_state()
        gpu_ready = gpu is not None and gpu[0] == "ready"
        gt.report("NOTE GPU compute: %s" % (gpu[1] if gpu else "no start-up line in natron.log"))
        if FAULT_ENV and gpu is not None and not gpu_ready and "injected fault" in gpu[1]:
            check(False, "NATRON_GPU_FAULT=%s spares GPU start-up (raise N past the operations start-up uses)"
                  % FAULT_ENV)
        py_viewer.renderCurrentFrame(False)
        has_regions = yield ("until", lambda: prop("regionCount") >= 1, POLL_TIMEOUT_S)
        caption("m116_on.png", "the preference on after a render of the chain")
        if not gpu_ready:
            gt.section("no GPU")
            yield ("sleep", 1000)
            check(prop("regionCount") == 0 and prop("exitCount") == 0 and prop("entryCount") == 0
                  and prop("fallbackCount") == 0,
                  "with no GPU the overlay draws no region (%s)" % state())
            check(shown(), "with no GPU the overlay stays on and empty")
            check(not gt.viewer_messages(), "with no GPU the viewer shows no error (%r)" % gt.viewer_messages())
            caption("m116_no_gpu.png", "no usable GPU: the overlay is on and draws nothing")
            gt.report("NOTE the mixed, cached and fallback cases need a usable GPU")
        elif not check(has_regions, "with a usable GPU the chain places at least one region (%s)" % state()):
            gt.report("NOTE mixed, cached and fallback cases skipped: no region was placed")
        else:
            gt.section("mixed")
            labels = prop("exitLabels")
            check(prop("exitCount") + prop("entryCount") >= 1,
                  "a region next to the TimeBlur has an exit or an entry (%s)" % state())
            check(len(labels) == prop("exitCount"), "every exit has a label (%s)" % state())
            for label in labels:
                check(any(label.startswith(r) for r in REASONS),
                      "the exit label %r names a reason" % label)
            upper_member = any(label.startswith("no GPU path") for label in labels)
            if not upper_member:
                gt.report("NOTE no exit reads \"no GPU path\": the cost model kept the upper chain on the CPU")
            caption("m116_mixed.png", "regions split round the TimeBlur: red X and reason on its input, entry tick below")

            gt.section("cached")
            if not FAULT_ENV:
                above = sorted(n.getScriptName() for n in chain[:-1])
                lower.getParam("saturation").setValue(0.6)

                def faded_as_expected():
                    faded = prop("fadedNodes")
                    return all(name in faded for name in above) and lower.getScriptName() not in faded

                ok = yield ("until", faded_as_expected, POLL_TIMEOUT_S)
                check(ok, "editing the Saturation fades every node above it, TimeBlur included, and not the "
                          "Saturation (expected %r faded; %s)" % (above, state()))
                check(prop("regionCount") >= 1, "the faded regions keep their outlines (%s)" % state())
                check(scrim_count() == prop("fadedCount"),
                      "every faded node has one fade cover (%d covers; %s)" % (scrim_count(), state()))
                lowered = [o for o in read_items(lambda it: it.opacity()) if o < 0.999]
                check(not lowered, "fading lowers no item's opacity, so no pipe shows through a node (%d items)"
                      % len(lowered))
                node_depths = scrim_parent_depths()
                pipe_depths = [d for d in read_items(
                    lambda it: it.zValue() if it.type() == LINE_ITEM_TYPE else None) if d is not None]
                check(node_depths and pipe_depths and max(pipe_depths) < min(node_depths),
                      "the pipes stay below the faded nodes (pipes %r, nodes %r)"
                      % (sorted(set(pipe_depths)), sorted(set(node_depths))))
                caption("m116_cached.png", "every node above the edited Saturation is faded; Saturation is not")
            else:
                gt.report("NOTE cached case skipped: a device fault is injected")

            gt.section("moved")
            moved = chain[1]
            rects_before = prop("regionRects")
            parsed_before = region_rects()
            centre_before = node_centre(moved)
            own = [i for i, r in enumerate(parsed_before) if rect_has(r, *centre_before)]
            if not check(own, "a region outlines %s before it moves (rects %r)" % (moved.getScriptName(), rects_before)):
                gt.report("NOTE moved case skipped: the cost model kept %s on the CPU" % moved.getScriptName())
            else:
                region = own[0]
                refreshes = prop("refreshCount")
                redraws = prop("redrawCount")
                moved.setPosition(400, NODE_SPACING)
                new_box = node_box(moved)
                corners = [(new_box[0], new_box[1]), (new_box[0] + new_box[2], new_box[1] + new_box[3])]

                def outline_follows():
                    rects = region_rects()
                    return (len(rects) == len(parsed_before)
                            and all(rect_has(rects[region], x, y) for x, y in corners))

                ok = yield ("until", outline_follows, POLL_TIMEOUT_S)
                check(ok, "the region's outline follows %s to its new place (was %r, now %r)"
                      % (moved.getScriptName(), rects_before[region:region + 1], prop("regionRects")[region:region + 1]))
                after = prop("regionRects")
                check(after[region] != rects_before[region], "the moved region's bounds changed")
                check(all(after[i] == rects_before[i] for i in range(len(after)) if i != region),
                      "no other region's outline changed (before %r, after %r)" % (rects_before, after))
                check(prop("redrawCount") > redraws and prop("refreshCount") == refreshes,
                      "the move redraws regions without rebuilding the overlay (redraws %d -> %d, rebuilds %d -> %d)"
                      % (redraws, prop("redrawCount"), refreshes, prop("refreshCount")))
                moved.setPosition(0, NODE_SPACING)
                ok = yield ("until", lambda: prop("regionRects") == rects_before, POLL_TIMEOUT_S)
                check(ok, "moving the node back restores every outline (%r)" % prop("regionRects"))

            gt.section("selection")
            yield ("sleep", 500)
            snap_before = (prop("refreshCount"), prop("regionRects"), prop("pipeLines"), prop("regionCount"))
            app.selectAllNodes()
            yield ("sleep", 1000)
            snap_selected = (prop("refreshCount"), prop("regionRects"), prop("pipeLines"), prop("regionCount"))
            check(snap_selected == snap_before and shown(),
                  "selecting every node rebuilds nothing and leaves the outlines and pipes alone (%r -> %r)"
                  % (snap_before, snap_selected))
            app.clearSelection()
            yield ("sleep", 1000)
            snap_cleared = (prop("refreshCount"), prop("regionRects"), prop("pipeLines"), prop("regionCount"))
            check(snap_cleared == snap_before and shown(),
                  "deselecting every node rebuilds nothing and leaves the outlines and pipes alone (%r -> %r)"
                  % (snap_before, snap_cleared))

            gt.section("low detail")
            faded_names = prop("fadedNodes")
            faded_node = app.getNode(faded_names[0]) if faded_names else None
            if faded_node is None:
                gt.report("NOTE low detail case skipped: no node is faded (a device fault is injected or nothing was cached)")
            else:
                graph = node_graph_view()
                saved_transform = graph.transform()
                switch_zoom = 4.0 / QFontMetrics(QApplication.font()).height()
                zoom = switch_zoom * 0.3
                chain_centre = node_centre(chain[len(chain) // 2])
                graph.resetTransform()
                graph.scale(zoom, zoom)
                graph.centerOn(chain_centre[0], chain_centre[1])
                graph.viewport().update()
                yield ("sleep", 1000)
                cx, cy = node_centre(faded_node)
                bg_at = graph.mapToScene(QPoint(12, 12))
                background = viewport_pixel(graph, bg_at.x(), bg_at.y())
                on_pixel = viewport_pixel(graph, cx, cy)
                caption("m116_lowdetail_on.png", "zoomed out to the low level of detail, overlay on: %s is faded"
                        % faded_node.getScriptName())
                pref.setValue(False)
                ok = yield ("until", lambda: not shown() and viewport_pixel(graph, cx, cy) != on_pixel, POLL_TIMEOUT_S)
                off_pixel = viewport_pixel(graph, cx, cy)
                check(ok, "turning the overlay off repaints the low-detail layer: the faded node's pixel changes "
                          "(on %r, off %r)" % (on_pixel, off_pixel))

                def distance(a, b):
                    return sum(abs(u - v) for u, v in zip(a, b))

                check(distance(on_pixel, background) < distance(off_pixel, background),
                      "the faded node sits closer to the background than the unfaded one (on %r, off %r, background %r)"
                      % (on_pixel, off_pixel, background))
                caption("m116_lowdetail_off.png", "zoomed out to the low level of detail, overlay off: no fade")
                pref.setValue(True)
                ok = yield ("until", lambda: shown() and viewport_pixel(graph, cx, cy) == on_pixel, POLL_TIMEOUT_S)
                check(ok, "turning the overlay back on fades the node again in the low-detail layer (now %r, wanted %r)"
                      % (viewport_pixel(graph, cx, cy), on_pixel))
                graph.setTransform(saved_transform)
                graph.centerOn(0, len(chain) * NODE_SPACING // 2)
                graph.viewport().update()
                yield ("sleep", 1000)

            gt.section("inputs")
            tested = False
            for target in (timeblur, lower_blur):
                pipes = pipes_into(target)
                if not pipes:
                    continue
                tested = True
                total = pipe_lines()
                hide = target.getParam("hideInputs")
                if not check(hide is not None, "%s has hideInputs" % target.getScriptName()):
                    continue
                hide.setValue(True)
                ok = yield ("until", lambda: not pipes_into(target), POLL_TIMEOUT_S)
                check(ok, "hiding the inputs of %s removes its marked pipe (%r left)" % (target.getScriptName(), pipes_into(target)))
                check(len(pipe_lines()) == len(total) - len(pipes),
                      "no other marked pipe is removed (%d of %d left)" % (len(pipe_lines()), len(total)))
                hide.setValue(False)
                ok = yield ("until", lambda: sorted(pipe_lines()) == sorted(total), POLL_TIMEOUT_S)
                check(ok, "showing the inputs of %s again restores the marked pipe (%r)" % (target.getScriptName(), pipe_lines()))
            if not tested:
                gt.report("NOTE inputs case skipped: no marked pipe reaches the TimeBlur or the lower Blur")

            gt.section("stable")
            regions_before = prop("regionCount")
            faded_before = sorted(prop("fadedNodes"))
            window = gt._find_main_window()
            if check(window is not None, "the main window is found"):
                where = window.pos()
                window.move(where.x() + 40, where.y() + 30)
                yield ("sleep", 2000)
                check(prop("regionCount") == regions_before and sorted(prop("fadedNodes")) == faded_before and shown(),
                      "moving the window leaves the regions and the fading alone (before regions=%d faded=%r; %s)"
                      % (regions_before, faded_before, state()))
                window.move(where)
                yield ("sleep", 500)
            pref.setValue(False)
            ok = yield ("until", lambda: not shown(), POLL_TIMEOUT_S)
            check(ok, "turning the preference off hides the overlay")
            pref.setValue(True)
            ok = yield ("until", lambda: shown() and prop("regionCount") == regions_before, POLL_TIMEOUT_S)
            check(ok, "turning the preference back on shows the regions again with no render or edit (%s)" % state())
            caption("m116_retoggled.png", "the regions after the preference was turned off and on, with no render")
            if not FAULT_ENV:
                gt.report("NOTE fallback case skipped: set NATRON_GPU_FAULT=lost@region:1 to inject a device fault")

        gt.section("off again")
        pref.setValue(False)
        ok = yield ("until", lambda: not shown(), POLL_TIMEOUT_S)
        check(ok, "turning the preference off hides the overlay")
        check(prop("regionCount") == 0 and prop("fadedCount") == 0,
              "turning the preference off clears the counts and the fading (%s)" % state())
        for node in reversed(chain):
            node.destroy()

    gt.run(steps())
except Exception:
    import traceback
    report_line = traceback.format_exc()
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + report_line + "\n")
    os._exit(1)
