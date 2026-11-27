"""Checks that the native geometry nodes sit under their own Geometry toolbar category.

Usage, from the repository root: Tests/gui/run-gui-test.sh Tests/gui/geometry_toolbar_menu.py

Asserts:
  - the main window's toolbox has a Geometry button, placed right after 3D;
  - its menu lists every geometry node in GEOMETRY_IDS and nothing else, none of them also under 3D;
  - the button draws its grouping's toolbox icon, which getIconForGrouping() maps to the box glyph
    (lucide/box.svg, shared with 3D); the shot shows it;
  - each geometry node's category colour equals the Native 3D colour preference.

Shot: geometry-toolbar-menu.png (the Geometry menu open).
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
SETTLE_MS = 500
WATCHDOG_S = 120

PROP_OWNED = "natronPluginOwned"
PROP_ID = "natronPluginID"
PROP_COLOR = "natronCategoryColor"
PROP_GROUPING = "natronGrouping"
PROP_TOOLBUTTON_ICON = "natronToolButtonIcon"

GEOMETRY_IDS = [
    "GeoCopyToPoints", "GeoDelete", "GeoDisplace", "GeoEdit",
    "GeoGroup", "GeoImportOBJ", "GeoMerge",
    "GeoNormals", "GeoPrimitive", "GeoPython",
    "GeoScatter", "GeoSubdivide", "GeoToUSD",
    "GeoTransform", "GeoUVFlatten", "GeoUVLayout",
    "GeoUVProject", "GeoWrangle",
    "USDToGeo",
]

try:
    import NatronGui
    from PySide6.QtWidgets import QApplication, QGraphicsView, QMenu, QToolBar
    from PySide6.QtGui import QColor

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def leaves_of(menu):
        found = []
        for a in menu.actions():
            sub = gt.menu_of(a)
            if sub is not None:
                found.extend(leaves_of(sub))
            elif a.property(PROP_ID):
                found.append(a)
        return found

    def top_menu(grouping):
        for a in NatronGui.natron.nodesMenu().actions():
            if a.property(PROP_OWNED) and a.property(PROP_GROUPING) == grouping and gt.menu_of(a) is not None:
                return gt.menu_of(a)
        return None

    def gui_widgets():
        return [w for w in gt.widgets_of("Gui") if w.isVisible()]

    def graph_up():
        guis = gui_widgets()
        return bool(guis) and any(gt.class_name(g) == "NodeGraph" and g.isVisible()
                                  for g in guis[0].findChildren(QGraphicsView))

    def toolbox_buttons(gui):
        bars = gui.findChildren(QToolBar)
        if not bars:
            return []
        buttons = []
        for a in bars[0].actions():
            w = bars[0].widgetForAction(a)
            if w is not None and hasattr(w, "menu") and gt.menu_of(w) is not None:
                buttons.append(w)
        return buttons

    def button_for(buttons, grouping):
        for b in buttons:
            if grouping in (b.toolTip() or ""):
                menu = gt.menu_of(b)
                if menu.menuAction().property(PROP_GROUPING) == grouping:
                    return b
        return None

    def steps():
        ok = yield ("until", graph_up, STARTUP_TIMEOUT_S)
        require(ok, "the main window and its node graph are up")
        gui = gui_widgets()[0]
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("geometry toolbar")
        registered = set(NatronGui.natron.getPluginIDs())
        missing = [i for i in GEOMETRY_IDS if i not in registered]
        require(not missing, "every geometry node is registered", repr(missing))

        buttons = toolbox_buttons(gui)
        groupings = [gt.menu_of(b).menuAction().property(PROP_GROUPING) for b in buttons]
        report("NOTE toolbox groupings: %r" % groupings)
        geo_button = button_for(buttons, "Geometry")
        three_d_button = button_for(buttons, "3D")
        require(geo_button is not None, "the toolbox has a Geometry button")
        if three_d_button is not None:
            check(groupings.index("Geometry") == groupings.index("3D") + 1, "the Geometry button follows 3D")
        check(not geo_button.icon().isNull(), "the Geometry button has an icon")
        check(geo_button.icon().cacheKey() ==
              gt.menu_of(geo_button).menuAction().property(PROP_TOOLBUTTON_ICON).cacheKey(),
              "the Geometry button draws its grouping's toolbox icon")

        geo_menu = gt.menu_of(geo_button)
        listed = sorted(a.property(PROP_ID) for a in leaves_of(geo_menu))
        check(listed == sorted(GEOMETRY_IDS),
              "the Geometry menu lists exactly the geometry nodes (extra %r, missing %r)" %
              (sorted(set(listed) - set(GEOMETRY_IDS)), sorted(set(GEOMETRY_IDS) - set(listed))))

        three_d_menu = top_menu("3D")
        if three_d_menu is not None:
            under_3d = set(a.property(PROP_ID) for a in leaves_of(three_d_menu))
            check(not (under_3d & set(GEOMETRY_IDS)), "no geometry node is still listed under 3D")

        pref = NatronGui.natron.getSettings().getParam("native3DNodesColor")
        require(pref is not None, "the Native 3D colour preference exists")
        value = pref.get()
        expected = QColor.fromRgbF(min(max(value.r, 0.0), 1.0), min(max(value.g, 0.0), 1.0),
                                   min(max(value.b, 0.0), 1.0)).name()
        wrong = [(a.property(PROP_ID), a.property(PROP_COLOR)) for a in leaves_of(geo_menu)
                 if a.property(PROP_COLOR) != expected]
        check(not wrong, "every geometry node carries the Native 3D colour %s (differing: %r)" % (expected, wrong))

        # QToolButton.showMenu() runs the menu's own modal loop and would block this step until the
        # watchdog fires; popup() returns at once and leaves the menu open for the shot.
        geo_menu.popup(geo_button.mapToGlobal(geo_button.rect().bottomLeft()))
        yield ("sleep", SETTLE_MS)
        check(geo_menu.isVisible(), "the Geometry menu opens from its button")
        gt.shot("geometry-toolbar-menu.png")
        geo_menu.close()
        popup = QApplication.activePopupWidget()
        if isinstance(popup, QMenu):
            popup.close()
        yield ("sleep", SETTLE_MS)

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.report("FAIL script error: " + traceback.format_exc())
        gt.finish()
    except Exception:
        traceback.print_exc()
        os._exit(1)
