"""The USD toolbar category: the nodes menu has a top-level USD entry right after 3D, with the
submenus Prims, Lights, Cameras, Materials, Composition, Edit, Render and I/O in that order, each
holding exactly its USD nodes. Every submenu has an icon that is not the default puzzle glyph, no
USD node has a grouping under 3D, and every USD node action carries the same category colour (the
one of the USD node category, which differs from a Filter node's). The USD toolbox button's popup
is saved to $NATRON_GUI_TEST_OUT/usd-toolbar-menu.png and the toolbox to usd-toolbar-toolbox.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/usd_toolbar_menu.py [natron-binary]
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

SUBMENUS = [
    ("Prims", ["USDCard", "USDCube", "USDCylinder", "USDSphere", "USDScopePrim", "USDXformPrim"]),
    ("Lights", ["USDLight"]),
    ("Cameras", ["USDCamera", "USDProject3D", "USDProjectUV"]),
    ("Materials", ["USDAssignMaterial", "USDEmissiveMaterial", "USDPreviewSurface"]),
    ("Composition", ["USDMerge", "USDReference", "USDVariant", "USDPrune"]),
    ("Edit", ["USDEditPrim", "USDEditTransform", "USDSetAttribute"]),
    ("Render", ["USDRender", "USDRenderSettings"]),
    ("IO", ["USDRead", "USDWrite"]),
]
DISPLAY_TITLES = {"IO": "I/O"}
GLYPHS = {
    "Prims": "shapes", "Lights": "sun", "Cameras": "camera", "Materials": "palette",
    "Composition": "layers", "Edit": "pencil", "Render": "circle-play", "IO": "hard-drive",
}
ICON_SIZE = 32
BLUR_ID = "Blur"

try:
    import NatronGui
    import shiboken6
    from PySide6.QtWidgets import QApplication, QMenu, QToolBar

    import guitest as gt
    from guitest import check, report, require

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def owned(action):
        return bool(action.property(PROP_OWNED))

    def owned_submenus(menu):
        return [(a, gt.menu_of(a)) for a in menu.actions() if owned(a) and gt.menu_of(a) is not None]

    def leaves_of(menu):
        return [a for a in menu.actions() if owned(a) and gt.menu_of(a) is None]

    def all_leaves(menu, found):
        for a in menu.actions():
            if not owned(a):
                continue
            sub = gt.menu_of(a)
            if sub is not None:
                all_leaves(sub, found)
            else:
                found.append(a)
        return found

    def top_level(label):
        for action, sub in owned_submenus(NatronGui.natron.nodesMenu()):
            if action.property(PROP_GROUPING) == label:
                return action, sub
        return None, None

    def steps():
        ok = yield ("until", lambda: len([w for w in gt.widgets_of("Gui") if w.isVisible()]) > 0, STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gui = [w for w in gt.widgets_of("Gui") if w.isVisible()][0]
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gt.section("top level")
        menu = NatronGui.natron.nodesMenu()
        require(menu is not None, "natron.nodesMenu() returns a menu")
        tops = [a.property(PROP_GROUPING) for a, _ in owned_submenus(menu)]
        report("NOTE top-level groupings: %r" % tops)
        require("USD" in tops, "the nodes menu has a USD entry")
        if "3D" in tops:
            check(tops.index("USD") == tops.index("3D") + 1, "USD follows 3D")
        elif "Deep" in tops:
            check(tops.index("USD") == tops.index("Deep") - 1, "with no 3D entry, USD sits just before Deep")
        usd_action, usd_menu = top_level("USD")
        require(usd_menu is not None, "the USD entry has a submenu")

        gt.section("submenus")
        subs = owned_submenus(usd_menu)
        names = [a.property(PROP_GROUPING) for a, _ in subs]
        check(names == [n for n, _ in SUBMENUS], "USD's submenus are %r in order (got %r)"
              % ([n for n, _ in SUBMENUS], names))
        check(not leaves_of(usd_menu), "USD holds only submenus")
        by_name = dict((a.property(PROP_GROUPING), (a, m)) for a, m in subs)
        for name, ids in SUBMENUS:
            if name not in by_name:
                check(False, "USD has a %s submenu" % name)
                continue
            action, sub = by_name[name]
            got = [a.property(PROP_ID) for a in leaves_of(sub)]
            check(sorted(got) == sorted(ids), "%s holds %r (got %r)" % (name, ids, got))
            check(not owned_submenus(sub), "%s has no nested submenus" % name)
            check(not action.icon().isNull(), "%s has an icon" % name)
            title = action.text()
            check(title == DISPLAY_TITLES.get(name, name), "%s is titled %r (got %r)" % (name, DISPLAY_TITLES.get(name, name), title))

        gt.section("icons")
        from PySide6.QtCore import QFile
        from PySide6.QtGui import QIcon

        def pixels(icon):
            return icon.pixmap(ICON_SIZE, ICON_SIZE).toImage()

        icons_dir = ":/Resources/Icons/lucide"
        puzzle = None
        for a, _ in owned_submenus(menu):
            if a.property(PROP_GROUPING) == "Misc":
                puzzle = pixels(a.icon())
        images = {}
        for n, _ in SUBMENUS:
            if n not in by_name:
                continue
            images[n] = pixels(by_name[n][0].icon())
            glyph_path = "%s/%s.svg" % (icons_dir, GLYPHS[n])
            if not QFile.exists(glyph_path):
                check(False, "%s glyph resource %s exists" % (n, glyph_path))
                continue
            check(images[n] == pixels(QIcon(glyph_path)), "%s draws the %s glyph" % (n, GLYPHS[n]))
            if puzzle is not None:
                check(images[n] != puzzle, "%s does not draw the default puzzle icon" % n)
        unique = set(img.constBits().tobytes() for img in images.values())
        check(len(unique) == len(images), "the eight USD submenus have eight different icons")
        usd_icon = usd_action.icon()
        check(not usd_icon.isNull(), "the USD entry has an icon")
        usd_pixels = pixels(usd_icon).constBits().tobytes()
        check(usd_pixels not in unique, "the USD icon differs from its submenus' icons")

        gt.section("category colour")
        colours = {}
        for _, ids in SUBMENUS:
            for pid in ids:
                colours[pid] = None
        for a in all_leaves(usd_menu, []):
            colours[a.property(PROP_ID)] = str(a.property(PROP_COLOR) or "")
        missing = sorted(k for k, v in colours.items() if v is None)
        check(not missing, "every USD node is in the menu (missing %r)" % missing)
        distinct = sorted(set(v for v in colours.values() if v))
        report("NOTE USD category colours: %r" % distinct)
        check(len(distinct) == 1 and distinct[0].startswith("#"), "every USD node has one category colour")
        blur = [a for a in all_leaves(menu, []) if a.property(PROP_ID) == BLUR_ID]
        if blur and distinct:
            check(str(blur[0].property(PROP_COLOR)) not in distinct, "the USD colour differs from Filter's")
        check(not any(a.property(PROP_ID) in colours for a in all_leaves(top_level("3D")[1], []))
              if top_level("3D")[1] is not None else True, "no USD node is left under 3D")

        gt.section("toolbox")
        bars = gui.findChildren(QToolBar)
        require(bool(bars), "the window has a toolbox")
        bar = bars[0]
        button = None
        for a in bar.actions():
            w = bar.widgetForAction(a)
            if w is not None and hasattr(w, "menu") and gt.menu_of(w) is not None and ptr(gt.menu_of(w)) == ptr(usd_menu):
                button = w
        require(button is not None, "the toolbox has a USD button")
        check(not button.icon().isNull(), "the USD button draws an icon")
        check("USD" in (button.toolTip() or ""), "the USD button's tooltip names USD")
        report(("SHOT " if bar.grab().save(os.path.join(gt.OUT, "usd-toolbar-toolbox.png")) else "NOTE could not save ")
               + "usd-toolbar-toolbox.png")

        def open_popups():
            return [w for w in QApplication.topLevelWidgets() if isinstance(w, QMenu) and w.isVisible()]

        from PySide6.QtCore import QTimer

        def grab_popup():
            if open_popups():
                gt.shot("usd-toolbar-menu.png")
            for p in open_popups():
                p.close()

        QTimer.singleShot(SETTLE_MS, grab_popup)
        QTimer.singleShot(SETTLE_MS + 3000, lambda: [p.close() for p in open_popups()])
        button.showMenu()
        QApplication.processEvents()

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
