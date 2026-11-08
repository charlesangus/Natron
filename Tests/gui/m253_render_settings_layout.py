"""Layout of the USDRenderSettings panel: its Controls, Storm, Cycles and MoonRay tabs.

In a 1920x1080 window at scale 1 the script opens the panel and, for each tab, shoots it in its
default state and with every group expanded, one PNG per screenshot (an expanded page taller than
the window is shot at successive scroll positions, one file each). It asserts:
  - the groups, and the knobs inside them, appear in the order the renderer's table lays them out,
    and each group starts open or closed as the table says;
  - a knob inside a group has its label indented by 12 px, and a top-level one has none;
  - the knobs that share a row are exactly the ones the table pairs;
  - the default Cycles and MoonRay tabs show no vertical scrollbar;
  - dependent knobs are greyed, both as parameters and as labels, and live again as their
    switches change: Use Denoise, Use Light Tree and Max Bounce on Cycles, Enable Denoise on
    MoonRay.

Run through Tests/gui/run-gui-test.sh. Screenshots and results.txt go to $NATRON_GUI_TEST_OUT
(default build/gui-test-out/). Exit status 0 only if every check passed.
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 30.0
SETTLE_MS = 500
EXPAND_SETTLE_MS = 800
ROW_TOLERANCE_PX = 6
GROUP_INDENT_PX = 12

try:
    from PySide6.QtCore import QMetaObject, QPoint
    from PySide6.QtWidgets import QApplication, QLabel, QLineEdit, QScrollArea, QTabWidget

    from guitest import check, close_all_panels, class_name, finish, plugin_problem, report, require, run, section, shot, widgets_of

    def G(name, is_open, *children):
        return {"name": name, "open": is_open, "children": list(children)}

    CYCLES = [
        G("cyclesSampling", True, "cyclesSamples", "cyclesUseAdaptiveSampling", "cyclesAdaptiveThreshold",
          "cyclesAdaptiveMinSamples", "cyclesTimeLimit", "cyclesSeed"),
        G("cyclesDenoise", True, "cyclesUseDenoise", "cyclesDenoiserType", "cyclesDenoiserQuality",
          "cyclesDenoiserPrefilter", "cyclesDenoiserPasses", "cyclesDenoiseStartSample",
          "cyclesDenoiserUpscaleFactor", "cyclesDenoiseUseGpu"),
        G("cyclesLightPaths", True,
          G("cyclesBounces", False, "cyclesMaxBounce", "cyclesMaxDiffuseBounce", "cyclesMaxGlossyBounce",
            "cyclesMaxTransmissionBounce", "cyclesMaxVolumeBounce", "cyclesTransparentMaxBounce"),
          G("cyclesClamping", False, "cyclesSampleClampDirect", "cyclesSampleClampIndirect"),
          G("cyclesCaustics", False, "cyclesCausticsReflective", "cyclesCausticsRefractive", "cyclesFilterGlossy"),
          G("cyclesFastGi", False, "cyclesAoBounces", "cyclesAoFactor", "cyclesAoDistance", "cyclesAoAdditiveFactor")),
        G("cyclesLights", False, "cyclesUseLightTree", "cyclesLightSamplingThreshold"),
        G("cyclesVolumes", False, "cyclesVolumeRayMarching", "cyclesVolumeMaxSteps", "cyclesVolumeStepRate"),
        G("cyclesPathGuiding", False, "cyclesUseGuiding", "cyclesDeterministicGuiding", "cyclesUseSurfaceGuiding",
          "cyclesSurfaceGuidingProbability", "cyclesUseVolumeGuiding", "cyclesVolumeGuidingProbability",
          "cyclesGuidingTrainingSamples", "cyclesUseGuidingDirectLight", "cyclesUseGuidingMisWeights",
          "cyclesGuidingDistributionType", "cyclesGuidingDirectionalSamplingType", "cyclesGuidingRoughnessThreshold"),
        G("cyclesLightContributions", False, "cyclesUseDirectLight", "cyclesUseIndirectLight", "cyclesUseDiffuse",
          "cyclesUseGlossy", "cyclesUseTransmission", "cyclesUseEmission"),
        G("cyclesAdvanced", False, "cyclesSamplingPattern", "cyclesScramblingDistance", "cyclesAaSamples",
          "cyclesMinBounce", "cyclesTransparentMinBounce", "cyclesMotionBlur", "cyclesUsePixelJitter",
          "cyclesUseCustomPixelJitterSample", "cyclesCustomPixelJitterSample",
          G("cyclesSampleSubset", False, "cyclesSampleOffset", "cyclesUseSampleSubset", "cyclesSampleSubsetOffset",
            "cyclesSampleSubsetLength")),
    ]
    CYCLES_PAIRS = [("cyclesCausticsReflective", "cyclesCausticsRefractive")]

    MOONRAY = [
        G("moonraySampling", True, "moonrayPixelSamples", "moonrayLightSamples", "moonrayBsdfSamples",
          "moonrayBssrdfSamples"),
        G("moonrayDepth", True, "moonrayMaxDepth", "moonrayMaxDiffuseDepth", "moonrayMaxGlossyDepth",
          "moonrayMaxMirrorDepth", "moonrayMaxVolumeDepth"),
        G("moonrayFiltering", True, "moonrayPixelFilter", "moonrayPixelFilterWidth"),
        G("moonrayDenoise", True, "moonrayEnableDenoise", "moonrayEnableOIDN", "moonrayDenoiseAlbedoGuiding",
          "moonrayDenoiseNormalGuiding"),
        G("moonraySceneToggles", False, "moonrayEnableMotionBlur", "moonrayDisableLighting", "moonrayDoubleSided",
          "moonrayDecodeNormals", "moonrayForcePolygon",
          G("moonrayPrune", False, "moonrayPruneWillow", "moonrayPruneFurDeform", "moonrayPruneVolume",
            "moonrayPruneWrapDeform", "moonrayPruneCurveDeform")),
        G("moonrayPerformance", False, "moonrayExecutionMode", "moonrayMaxFps"),
        G("moonrayRemote", False, "moonrayUseRemoteHosts", "moonrayRemoteHosts"),
        G("moonrayLogging", False, "moonrayLogLevel", "moonrayDebug", "moonrayInfo", "moonrayRdlOutput"),
    ]
    MOONRAY_PAIRS = [("moonrayDenoiseAlbedoGuiding", "moonrayDenoiseNormalGuiding")]

    settings = app.createNode("USDRenderSettings")
    require(settings is not None, "create USDRenderSettings", plugin_problem("USDRenderSettings"))
    settings.setPosition(0, 0)

    # Wrappers whose children are used later: a child's wrapper dies with its parent's.
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
        """Steps that open node's settings panel on its own, retrying once; returns whether it opened.
        showSelectedNodeSettingsPanel does nothing unless node is the only selected node."""
        for attempt in range(2):
            close_all_panels()
            yield ("sleep", 300)
            app.clearSelection()
            activate_main_window()
            app.selectNode(node, True)
            QApplication.processEvents()
            gv = graph_view()
            invoked = gv is not None and QMetaObject.invokeMethod(gv, "showSelectedNodeSettingsPanel")
            if not invoked:
                report("NOTE showSelectedNodeSettingsPanel was not invoked (attempt %d)" % (attempt + 1))
            ok = yield ("until", lambda: node_panel(node) is not None, POLL_TIMEOUT_S / 2)
            if ok:
                return True
            report("NOTE %s's settings panel did not open (attempt %d)" % (node.getScriptName(), attempt + 1))
        return False

    def panel_tabs(node):
        panel = node_panel(node)
        if panel is None:
            return None
        tabs = [t for t in panel.findChildren(QTabWidget) if t.count() > 0]
        return hold(tabs[0]) if tabs else None

    def tab_texts(node):
        tabs = panel_tabs(node)
        return [tabs.tabText(i).replace("&", "") for i in range(tabs.count())] if tabs is not None else []

    def show_tab(node, names):
        """Shows the first tab of node's panel whose label is one of names (case-insensitive);
        returns its label, or None."""
        tabs = panel_tabs(node)
        if tabs is None:
            return None
        wanted = [n.lower() for n in names]
        for i in range(tabs.count()):
            text = tabs.tabText(i).replace("&", "")
            if text.lower() in wanted:
                tabs.setCurrentIndex(i)
                QApplication.processEvents()
                return text
        return None

    def scroll_area(node):
        """The Properties bin's scroll area: a node panel's tabs have none of their own."""
        tabs = panel_tabs(node)
        w = tabs.parentWidget() if tabs is not None else None
        while w is not None and not isinstance(w, QScrollArea):
            w = hold(w).parentWidget()
        return hold(w)

    def current_page(node):
        tabs = panel_tabs(node)
        return hold(tabs.currentWidget()) if tabs is not None else None

    def knob_labels(node):
        """The knob labels on the current tab, as (text, widget, visible)."""
        page = current_page(node)
        if page is None:
            return []
        found = []
        for w in page.findChildren(QLabel):
            try:
                if class_name(w) != "ClickableLabel":
                    continue
                text = w.text().replace("&", "").strip()
            except RuntimeError:
                continue
            if text:
                found.append((text, hold(w), w.isVisible()))
        return found

    def visible_rows(node):
        """The visible knob labels of the current tab grouped into on-screen rows, top to bottom
        and left to right: a list of rows, each a list of (text, widget)."""
        content = current_page(node)
        placed = []
        for text, w, visible in knob_labels(node):
            if visible:
                p = w.mapTo(content, QPoint(0, w.height() // 2))
                placed.append((p.y(), p.x(), text, w))
        placed.sort(key=lambda t: (t[0], t[1]))
        rows = []
        last_y = None
        for y, x, text, w in placed:
            if last_y is None or y - last_y > ROW_TOLERANCE_PX:
                rows.append([])
                last_y = y
            rows[-1].append((text, w))
        return rows

    def is_group(item):
        return isinstance(item, dict)

    def walk(tree, parent=None):
        """Every (item, parent group) of tree in layout order, groups before their children."""
        for item in tree:
            yield item, parent
            if is_group(item):
                for pair in walk(item["children"], item):
                    yield pair

    def group_names(tree):
        return [i["name"] for i, _ in walk(tree) if is_group(i)]

    def shown(tree, group_open):
        """The script names on screen, in order, when group_open(name) says which groups are open."""
        names = []

        def visit(items):
            for item in items:
                if is_group(item):
                    names.append(item["name"])
                    if group_open(item["name"]):
                        visit(item["children"])
                else:
                    names.append(item)

        visit(tree)
        return names

    def label_of(name):
        return settings.getParam(name).getLabel()

    def set_opened(tree, opened):
        for name in group_names(tree):
            settings.getParam(name).setOpened(opened)
        QApplication.processEvents()

    def check_layout(tree, pairs, group_open, what):
        """Compares the visible rows with what the table says is on screen."""
        names = shown(tree, group_open)
        expected = [label_of(n) for n in names]
        rows = visible_rows(settings)
        actual = [t for row in rows for t, _ in row]
        start = actual.index(expected[0]) if expected and expected[0] in actual else 0
        if start:
            report("NOTE %s: labels above the first group: %r" % (what, actual[:start]))
        check(actual[start:start + len(expected)] == expected,
              "%s: the rows are in table order (wanted %r, got %r)" % (what, expected, actual[start:start + len(expected)]))
        tail = actual[start + len(expected):]
        check(not tail or tail[0] == "Other Settings", "%s: nothing after the table's rows but Other Settings (%r)" % (what, tail))
        headers = [label_of(n) for n in names if n in group_names(tree)]
        check([t for t in actual if t in headers] == headers, "%s: the groups are in table order (%r)" % (what, headers))

        visible_pairs = [[label_of(n) for n in p] for p in pairs if all(n in names for n in p)]
        shared = [[t for t, _ in row] for row in rows if len(row) > 1]
        check(shared == visible_pairs, "%s: the knobs sharing a row are %r (got %r)" % (what, visible_pairs, shared))

        parent_of = {(i["name"] if is_group(i) else i): p for i, p in walk(tree)}
        pair_second = {label_of(p[1]) for p in pairs}
        wrong = []
        for row in rows:
            for text, w in row:
                if text in pair_second:
                    continue
                name = next((n for n in names if label_of(n) == text), None)
                if name is None:
                    continue
                want = GROUP_INDENT_PX if parent_of[name] is not None else 0
                try:
                    container = w.parentWidget()
                    row_layout = container.layout()
                    got = row_layout.contentsMargins().left()
                except (AttributeError, RuntimeError):
                    got = None
                if got != want:
                    wrong.append("%s %r wanted %r" % (text, got, want))
        check(not wrong, "%s: labels are indented %d px inside a group and 0 outside (%s)" % (what, GROUP_INDENT_PX, "; ".join(wrong)))

    def check_open_states(tree, what):
        wrong = ["%s is %r" % (i["name"], settings.getParam(i["name"]).getIsOpened())
                 for i, _ in walk(tree) if is_group(i) and settings.getParam(i["name"]).getIsOpened() != i["open"]]
        check(not wrong, "%s: every group starts as the table says (%s)" % (what, "; ".join(wrong)))

    def check_no_scrollbar(what):
        sa = scroll_area(settings)
        bar = sa.verticalScrollBar()
        check(bar.maximum() == 0 and not bar.isVisible(),
              "%s: no vertical scrollbar (range 0 to %d, visible %r)" % (what, bar.maximum(), bar.isVisible()))

    def shoot_expanded(slug):
        """Steps that shoot the current tab, at successive scroll positions when it scrolls."""
        bar = scroll_area(settings).verticalScrollBar()
        bar.setValue(0)
        yield ("sleep", SETTLE_MS)
        if bar.maximum() == 0:
            shot("m253-layout-%s-expanded.png" % slug)
            return
        position = 0
        index = 1
        while True:
            bar.setValue(position)
            yield ("sleep", SETTLE_MS)
            shot("m253-layout-%s-expanded-%d.png" % (slug, index))
            if position >= bar.maximum():
                break
            position = min(position + max(bar.pageStep(), 1), bar.maximum())
            index += 1
        bar.setValue(0)

    def check_grey(names, grey, what):
        labels = {}
        for text, w, _ in knob_labels(settings):
            labels.setdefault(text, w)
        wrong = []
        for n in names:
            p = settings.getParam(n)
            if p.getIsEnabled(0) == grey:
                wrong.append("%s parameter enabled=%r" % (n, p.getIsEnabled(0)))
            w = labels.get(p.getLabel())
            if w is None:
                wrong.append("%s has no label" % n)
            elif bool(w.property("readOnly")) != grey:
                wrong.append("%s label readOnly=%r" % (n, w.property("readOnly")))
        check(not wrong, "%s: %s %s (%s)" % (what, ", ".join(names), "greyed" if grey else "live", "; ".join(wrong)))

    def toggle(name, value):
        settings.getParam(name).set(value)
        QApplication.processEvents()
        yield ("sleep", SETTLE_MS)

    def check_tab(slug, names, make_tree, pairs, table_defaults=True):
        """Steps that run a renderer tab's checks and shots, leaving every group expanded; returns
        the tab's tree, or None when the tab is missing."""
        section(slug)
        shown_as = show_tab(settings, names)
        check(shown_as is not None, "the panel has a %s tab (%r)" % (slug, tab_texts(settings)))
        if shown_as is None:
            return None
        tree = make_tree()
        yield ("sleep", EXPAND_SETTLE_MS)
        if table_defaults:
            check_open_states(tree, slug + " default")
        check_layout(tree, pairs, lambda n: settings.getParam(n).getIsOpened(), slug + " default")
        if slug in ("cycles", "moonray"):
            check_no_scrollbar(slug + " default")
        shot("m253-layout-%s-default.png" % slug)
        set_opened(tree, True)
        yield ("sleep", EXPAND_SETTLE_MS)
        check_layout(tree, pairs, lambda n: True, slug + " expanded")
        yield from shoot_expanded(slug)
        return tree

    def storm_tree():
        """The Storm tab's table from its parameters: groups, with their knobs, in layout order."""
        everything = [hold(p) for p in settings.getParams()]
        mine = [p for p in everything if p.getScriptName().startswith("storm") and p.getScriptName() != "storm"]
        by_name = {p.getScriptName(): p for p in mine}
        children = {}
        for p in mine:
            parent = p.getParent()
            parent_name = parent.getScriptName() if parent is not None else None
            children.setdefault(parent_name if parent_name in by_name else None, []).append(p.getScriptName())

        def build(names):
            items = []
            for n in names:
                if hasattr(by_name[n], "getIsOpened"):
                    items.append(G(n, by_name[n].getIsOpened(), *build(children.get(n, []))))
                else:
                    items.append(n)
            return items

        return build(children.get(None, []))

    def steps():
        gui = main_gui()
        if gui is not None:
            gui.resize(1920, 1080)
        opened = yield from open_panel(settings)
        require(opened, "the USDRenderSettings panel opens")
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: {"cycles", "moonray"} <= {t.lower() for t in tab_texts(settings)}, POLL_TIMEOUT_S)
        report("NOTE USDRenderSettings tabs (%s): %r" % ("all renderers present" if ok else "a renderer missing", tab_texts(settings)))

        section("controls")
        shown_as = show_tab(settings, ("Controls",))
        check(shown_as is not None, "the panel has a Controls tab (%r)" % (tab_texts(settings),))
        if shown_as is not None:
            yield ("sleep", EXPAND_SETTLE_MS)
            shot("m253-layout-controls-default.png")
            yield from shoot_expanded("controls")

        tree = yield from check_tab("storm", ("Storm", "GL"), storm_tree, [], False)
        if tree is not None:
            names = [i["name"] for i in tree if is_group(i)]
            report("NOTE Storm tab groups: %r" % (group_names(tree),))
            for wanted in ("stormVolumes", "stormLights", "stormPerformance"):
                check(wanted in names, "Storm has a %s group (%r)" % (wanted, names))
            check("stormOtherSettings" not in names or names[-1] == "stormOtherSettings",
                  "Storm's Other Settings comes last (%r)" % (names,))

        found = yield from check_tab("cycles", ("Cycles",), lambda: CYCLES, CYCLES_PAIRS)
        if found is not None:
            denoise_dependents = ["cyclesDenoiserQuality", "cyclesDenoiserPrefilter", "cyclesDenoiserPasses",
                                  "cyclesDenoiseStartSample", "cyclesDenoiserUpscaleFactor"]
            always_off = ["cyclesDenoiserType", "cyclesDenoiseUseGpu"]
            bounce_dependents = ["cyclesMaxDiffuseBounce", "cyclesMaxGlossyBounce", "cyclesMaxTransmissionBounce",
                                 "cyclesMaxVolumeBounce"]
            check_grey(denoise_dependents, True, "Use Denoise off")
            yield from toggle("cyclesUseDenoise", True)
            check_grey(denoise_dependents, False, "Use Denoise on")
            check_grey(always_off, True, "Use Denoise on")
            yield from toggle("cyclesUseDenoise", False)
            check_grey(denoise_dependents, True, "Use Denoise off again")

            check_grey(["cyclesLightSamplingThreshold"], True, "Use Light Tree on")
            yield from toggle("cyclesUseLightTree", False)
            check_grey(["cyclesLightSamplingThreshold"], False, "Use Light Tree off")
            yield from toggle("cyclesUseLightTree", True)
            check_grey(["cyclesLightSamplingThreshold"], True, "Use Light Tree on again")

            check_grey(bounce_dependents, False, "Max Bounce above 0")
            yield from toggle("cyclesMaxBounce", 0)
            check_grey(bounce_dependents, True, "Max Bounce 0")
            check_grey(["cyclesTransparentMaxBounce"], False, "Max Bounce 0")
            yield from toggle("cyclesMaxBounce", 7)
            check_grey(bounce_dependents, False, "Max Bounce back above 0")

        found = yield from check_tab("moonray", ("MoonRay", "Moonray"), lambda: MOONRAY, MOONRAY_PAIRS)
        if found is not None:
            guiding = ["moonrayDenoiseAlbedoGuiding", "moonrayDenoiseNormalGuiding"]
            check_grey(guiding, True, "Enable Denoise off")
            check_grey(["moonrayEnableOIDN"], True, "Enable Denoise off")
            yield from toggle("moonrayEnableDenoise", True)
            check_grey(guiding, False, "Enable Denoise on")
            check_grey(["moonrayEnableOIDN"], True, "Enable Denoise on")
            yield from toggle("moonrayEnableDenoise", False)
            check_grey(guiding, True, "Enable Denoise off again")

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
