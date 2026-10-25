"""Screenshots and checks for node label wrapping, take-over picker labels and alignment, themed
icons, the link cursor's hotspot, and layout density.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh for the GL/Xvfb setup):
    docker exec -e NATRON_GUI_TEST_OUT=build/m56-shots/out -e GUI_TEST_TIMEOUT=300 \
        -w /home/bosley/git/Natron natron-dev \
        Tests/gui/run-gui-test.sh Tests/gui/m56_visual.py build/fast/App/Natron
Run it again with -e QT_SCALE_FACTOR=2 for the hi-DPI shots. Set M56_BASELINE_DIR to the output
directory of an earlier run to also check that the themed icons differ from that run's icons. Set
M56_TOOLBOX_BASELINE to an icons-toolbox.png from an earlier run to also check that the left
toolbox is drawn pixel for pixel as it was then; unset, that comparison is reported as SKIP, and a
path that does not exist fails.

Sizes are compared in logical px scaled by the screen's logical DPI / 96, the same factor
TO_DPIX/TO_DPIY apply.

Knob widgets carry no object name, but KnobGui puts the knob's script name in bold at the start
of the tooltip it sets on both the label and the field, so that is how a knob's widgets are found.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
WATCHDOG_S = 280
SETTLE_MS = 500
PANEL_MS = 900

NODE_WIDTH = 80
NODE_MAX_WIDTH = NODE_WIDTH * 2.5
ALIGN_TOLERANCE = 2
# Effect.getSize() is the node's boundingRect(), which carries its 1 px outline pen on top of the
# laid-out width.
NODE_OUTLINE = 1

ROW_GAP = 4
GROUP_INDENT = 12
PANEL_MARGIN = 6
PANE_TOP_MARGIN = 2
TAB_HEIGHT = 26
MENU_TOP_PADDING = 4

THEMED_COLOUR_TOLERANCE = 12
THEMED_PIXEL_FRACTION = 0.9
THEMED_ICON_FRACTION = 0.8
ICON_PROBE_SIZE = 24

PLUGIN_ICON_COLUMN = 20 + 2 * 2
LABEL_WRAP_INDENT = 8
CHANNEL_BUTTON_CHROMA_RATIO = 0.65
CHANNEL_BUTTON_MIN_CHROMA = 25
ROW_LABEL_BRIGHTNESS_RATIO = 0.85

HEADER_BUTTON = 22
HEADER_BUTTON_ICON = 18
FULL_STRENGTH_ALPHA = 230
GLYPH_FILL_FRACTION = 0.4
DROP_DOWN_ICON = 6
ICON_TEXT_TOLERANCE = 2
THEME_RELOAD_MS = 1500
RECOLOUR_TEST_COLOUR = (0.35, 0.85, 0.45)

try:
    import NatronEngine
    import shiboken6
    from PySide6.QtCore import QBuffer, QByteArray, QEvent, QIODevice, QMetaObject, QPoint, QPointF, QRect, QRectF, QSize, Qt, QTimer
    from PySide6.QtGui import QCursor, QFontMetrics, QGuiApplication, QIcon, QImage, QMouseEvent, QPalette
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QGraphicsTextItem, QLabel, QMenu, QScrollArea,
                                   QTabBar, QToolBar, QWidget)

    import guitest as gt
    from guitest import check, report

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    _icon_hashes = {}

    def dpi_scale():
        return QGuiApplication.primaryScreen().logicalDotsPerInchX() / 96.0

    def px(logical):
        return logical * dpi_scale()

    def near(value, logical, tol=0.5):
        return abs(value - px(logical)) <= tol

    def ptr(obj):
        return shiboken6.getCppPointer(obj)[0]

    def record_binary():
        binary = QApplication.applicationFilePath()
        try:
            mtime = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(binary)))
        except OSError:
            mtime = "?"
        try:
            describe = subprocess.run(["git", "-C", gt.ROOT, "describe", "--always", "--dirty", "--tags"],
                                      capture_output=True, text=True, timeout=20).stdout.strip()
        except Exception:
            describe = "?"
        report("NOTE binary %s (built %s); git describe of the tree it was run from: %s"
               % (binary, mtime, describe or "?"))
        report("NOTE QT_SCALE_FACTOR=%s, logical DPI scale %.3f, device pixel ratio %.2f"
               % (os.environ.get("QT_SCALE_FACTOR", "unset"), dpi_scale(), QApplication.instance().devicePixelRatio()))

    # ---- nodes, panes and widgets --------------------------------------------------------------

    def create(plugin_id, x, y, label=None):
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        node = app.createNode(plugin_id, -1, None, props)
        gt.require(node is not None, "create " + plugin_id, gt.plugin_problem(plugin_id))
        node.setPosition(x, y)
        if label is not None:
            node.setLabel(label)
        return node

    def root_graph():
        graphs = [w for w in gt.widgets_of("NodeGraph") if w.isVisible()]
        return graphs[0] if graphs else None

    def gui_widget():
        found = gt.widgets_of("Gui")
        return found[0] if found else None

    def visible_panels():
        return [w for w in gt.widgets_of("NodeSettingsPanel") if w.isVisible()]

    def panel_for(node):
        """The visible settings panel whose name field shows node's label."""
        for p in visible_panels():
            for le in p.findChildren(QWidget):
                try:
                    if hasattr(le, "text") and gt.class_name(le) == "LineEdit" and le.text() == node.getLabel():
                        return p
                except (RuntimeError, TypeError):
                    continue
        panels = visible_panels()
        return panels[0] if len(panels) == 1 else None

    def descendants(root, cls=QWidget):
        try:
            return [w for w in root.findChildren(cls) if w.isVisible()]
        except RuntimeError:
            return []

    def is_inside(widget, ancestor_ptrs):
        w = widget
        while w is not None:
            if ptr(w) in ancestor_ptrs:
                return True
            w = w.parentWidget()
        return False

    def pane(name):
        for w in gt.widgets_of("TabWidget"):
            if w.objectName() == name:
                return w
        return None

    def make_tab_current(pane_name, label_substr):
        tw = app.getTabWidget(pane_name)
        if tw is None:
            return False
        for i in range(tw.count()):
            if label_substr.lower() in tw.getTabLabel(i).lower():
                tw.setCurrentIndex(i)
                QApplication.processEvents()
                return True
        return False

    def global_rect(widget):
        tl = widget.mapToGlobal(QPoint(0, 0))
        return tl.x(), tl.y(), widget.width(), widget.height()

    def crop_shot(widget, name, pad=0):
        """A screen crop rather than widget.grab() where possible, so GL children (viewer,
        timeline, dope sheet) come out drawn."""
        if widget is None or not widget.isVisible():
            report("NOTE %s: nothing visible to shoot" % name)
            return
        QApplication.processEvents()
        x, y, w, h = global_rect(widget)
        screen = QGuiApplication.primaryScreen()
        if screen.geometry().contains(QRect(x - pad, y - pad, w + 2 * pad, h + 2 * pad)):
            pix = screen.grabWindow(0, x - pad, y - pad, w + 2 * pad, h + 2 * pad)
        else:
            pix = widget.grab()
            report("NOTE %s: partly off screen, grabbed from the widget instead" % name)
        path = os.path.join(gt.OUT, name + ".png")
        report(("SHOT " if pix.save(path) else "NOTE could not save ") + path)

    def open_menus():
        menus = []
        popup = QApplication.activePopupWidget()
        if isinstance(popup, QMenu):
            menus.append(popup)
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QMenu) and w.isVisible() and all(ptr(w) != ptr(m) for m in menus):
                menus.append(w)
        return menus

    def close_popups():
        for _ in range(4):
            menus = open_menus()
            if not menus:
                return
            for m in reversed(menus):
                m.close()
            QApplication.processEvents()

    def right_click(widget, pos, on_menu, settle_ms=500):
        """QMenu::exec runs a nested event loop inside the press handler, so on_menu is scheduled
        before the press and runs inside that loop; it must close the menu when done. A watchdog
        keeps closing menus until the press returns, so a missed menu cannot hang the run."""
        done = [False]

        def fire():
            menus = open_menus()
            try:
                on_menu(menus[0] if menus else None)
            except Exception:
                check(False, "menu handler error: " + traceback.format_exc())
                close_popups()

        def guard():
            if done[0]:
                return
            close_popups()
            QTimer.singleShot(2000, guard)
        QTimer.singleShot(settle_ms, fire)
        QTimer.singleShot(settle_ms + 4000, guard)
        ev = QMouseEvent(QEvent.MouseButtonPress, QPointF(pos), widget.mapToGlobal(QPointF(pos)),
                         Qt.RightButton, Qt.RightButton, Qt.NoModifier)
        QApplication.sendEvent(widget, ev)
        done[0] = True
        QApplication.processEvents()

    # ---- icon size, colour and live theme probes ---------------------------------------------

    def text_colour():
        param = NatronEngine.natron.getSettings().getParam("text")
        return tuple(int(round(param.getValue(i) * 255)) for i in range(3))

    def opaque_colour(img, min_alpha=250):
        """Mean colour of the fully opaque pixels: the stroke cores, free of edge blending."""
        n = r = g = b = 0
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                if c.alpha() >= min_alpha:
                    n += 1
                    r += c.red()
                    g += c.green()
                    b += c.blue()
        return (r / float(n), g / float(n), b / float(n)) if n else None

    def colour_near(a, b, tol):
        return a is not None and b is not None and all(abs(x - y) <= tol for x, y in zip(a, b))

    def fmt_colour(c):
        return "none" if c is None else "(%.0f, %.0f, %.0f)" % tuple(c)

    def glyph_extent(pix):
        """(max alpha, glyph bounding-box size in logical px) of a rendered icon."""
        img = pix.toImage()
        dpr = pix.devicePixelRatio() or 1.0
        top_alpha = 0
        xs = []
        ys = []
        for y in range(img.height()):
            for x in range(img.width()):
                a = img.pixelColor(x, y).alpha()
                top_alpha = max(top_alpha, a)
                if a >= 128:
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return top_alpha, 0.0
        return top_alpha, max(max(xs) - min(xs) + 1, max(ys) - min(ys) + 1) / dpr

    def check_header_buttons(area, buttons):
        probed = 0
        for b in buttons:
            ic = b.icon()
            if ic is None or ic.isNull() or b.width() > px(HEADER_BUTTON) + 1:
                continue
            probed += 1
            name = describe_icon_owner(b)
            size = b.iconSize()
            check(size.width() >= px(HEADER_BUTTON_ICON) - 0.5,
                  "%s: %s draws its icon at %d px in a %d px button" % (area, name, size.width(), b.width()))
            top_alpha, extent = glyph_extent(ic.pixmap(size))
            check(top_alpha >= FULL_STRENGTH_ALPHA,
                  "%s: %s's strokes reach full strength at the button's icon size (max alpha %d)" % (area, name, top_alpha))
            check(extent >= GLYPH_FILL_FRACTION * b.width(),
                  "%s: %s's glyph spans %.1f px of its %d px button" % (area, name, extent, b.width()))
        check(probed > 0, "%s: found header icon buttons to probe (%d)" % (area, probed))

    def themed_label():
        for w in QApplication.allWidgets():
            if isinstance(w, QLabel) and w.isVisible() and w.property("_natronThemedIcon") is not None:
                return w
        return None

    def label_text_colour():
        for w in QApplication.allWidgets():
            # An unaltered Label is drawn in the plain text colour (an altered one uses the
            # "unmodified" colour instead).
            if (gt.class_name(w) == "Label" and w.isVisible() and w.isEnabled() and w.text()
                    and w.property("altered") is False):
                c = w.palette().color(QPalette.WindowText)
                return (c.red(), c.green(), c.blue())
        return None

    def icon_colour(button):
        return opaque_colour(button.icon().pixmap(QSize(ICON_PROBE_SIZE, ICON_PROBE_SIZE), QIcon.Normal, QIcon.Off).toImage())

    def label_pixmap_colour(label):
        # A label's pixmap is rendered at its small display size, where a stroke's core can stop
        # just short of fully opaque.
        pix = label.pixmap() if label is not None else None
        return opaque_colour(pix.toImage(), FULL_STRENGTH_ALPHA) if pix is not None and not pix.isNull() else None

    # ---- themed-icon probe ---------------------------------------------------------------------

    def theme_colours():
        settings = NatronEngine.natron.getSettings()
        colours = []
        for name in ("text", "selection"):
            param = settings.getParam(name) if settings is not None else None
            if param is not None:
                colours.append(tuple(int(round(param.getValue(i) * 255)) for i in range(3)))
        return colours or [(199, 199, 199)]

    def icon_image(icon):
        pix = icon.pixmap(QSize(ICON_PROBE_SIZE, ICON_PROBE_SIZE), QIcon.Normal, QIcon.Off)
        return pix, pix.toImage()

    def themed_fraction(img, colours):
        opaque = 0
        themed = 0
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                if c.alpha() < 160:
                    continue
                opaque += 1
                for r, g, b in colours:
                    if max(abs(c.red() - r), abs(c.green() - g), abs(c.blue() - b)) <= THEMED_COLOUR_TOLERANCE:
                        themed += 1
                        break
        return (themed / float(opaque)) if opaque else None, opaque

    def image_hash(img):
        data = QByteArray()
        buf = QBuffer(data)
        buf.open(QIODevice.WriteOnly)
        img.save(buf, "PNG")
        buf.close()
        return hashlib.md5(bytes(data)).hexdigest()

    def describe_icon_owner(owner):
        text = ""
        for getter in ("text", "toolTip"):
            try:
                text = getattr(owner, getter)() or ""
            except Exception:
                text = ""
            if text:
                break
        return re.sub(r"<[^>]*>", " ", text).replace("\n", " ").strip()[:40] or gt.class_name(owner)

    def icon_entries(owners):
        """(description, icon copy) for each button or action that has an icon."""
        entries = []
        for o in owners:
            try:
                ic = o.icon()
            except RuntimeError:
                continue
            if ic is not None and not ic.isNull():
                entries.append((describe_icon_owner(o), QIcon(ic)))
        return entries

    def check_icons(area, owners):
        """owners are buttons or actions. Every icon must be non-null, and most must be drawn in
        the theme's text or selection colour: the PNG icons were not."""
        return check_icon_entries(area, icon_entries(owners))

    def check_icon_entries(area, icons):
        colours = theme_colours()
        if not check(len(icons) > 0, "%s: found icons to probe (%d)" % (area, len(icons))):
            return 0, 0, []
        nulls = 0
        themed = 0
        probed = 0
        off = []
        hashes = []
        for owner, ic in icons:
            pix, img = icon_image(ic)
            if pix.isNull():
                nulls += 1
                continue
            hashes.append(image_hash(img))
            frac, opaque = themed_fraction(img, colours)
            if frac is None:
                continue
            probed += 1
            if frac >= THEMED_PIXEL_FRACTION:
                themed += 1
            else:
                off.append("%s (%.0f%%)" % (owner, frac * 100))
        check(nulls == 0, "%s: every icon pixmap is non-null (%d null of %d)" % (area, nulls, len(icons)))
        ratio = (themed / float(probed)) if probed else 0.0
        check(probed > 0 and ratio >= THEMED_ICON_FRACTION,
              "%s: %d of %d icons are drawn in the theme colour (need %.0f%%)"
              % (area, themed, probed, THEMED_ICON_FRACTION * 100))
        if off:
            report("NOTE %s: icons not in the theme colour: %s" % (area, ", ".join(off[:12])))
        _icon_hashes[area] = hashes
        compare_with_baseline(area, hashes)
        return themed, probed, off

    def compare_with_baseline(area, hashes):
        base_dir = os.environ.get("M56_BASELINE_DIR")
        if not base_dir:
            return
        path = os.path.join(base_dir, "icon-hashes.json")
        try:
            with open(path) as f:
                baseline = json.load(f).get(area)
        except (OSError, ValueError):
            baseline = None
        if not baseline:
            report("NOTE %s: no baseline icons in %s" % (area, path))
            return
        unchanged = len(set(hashes) & set(baseline))
        check(unchanged <= len(hashes) * (1.0 - THEMED_ICON_FRACTION),
              "%s: icons differ from the baseline run (%d of %d unchanged)" % (area, unchanged, len(hashes)))

    def compare_toolbox_with_baseline(shot_path):
        base_path = os.environ.get("M56_TOOLBOX_BASELINE")
        if not base_path:
            report("SKIP toolbox: pixel comparison with a baseline (set M56_TOOLBOX_BASELINE to an "
                   "icons-toolbox.png from an earlier run)")
            return
        if not check(os.path.isfile(base_path), "toolbox: baseline screenshot %s exists" % base_path):
            return
        base = QImage(base_path)
        if not check(not base.isNull(), "toolbox: baseline screenshot %s loads" % base_path):
            return
        current = QImage(shot_path)
        if not check(current.size() == base.size(),
                     "toolbox: same size as the baseline (%dx%d vs %dx%d)"
                     % (current.width(), current.height(), base.width(), base.height())):
            return
        differing = 0
        worst = 0
        for y in range(base.height()):
            for x in range(base.width()):
                a = current.pixelColor(x, y)
                b = base.pixelColor(x, y)
                d = max(abs(a.red() - b.red()), abs(a.green() - b.green()), abs(a.blue() - b.blue()))
                if d:
                    differing += 1
                    worst = max(worst, d)
        check(differing == 0, "toolbox: pixel-identical to the baseline (%d pixels differ, by up to %d)"
              % (differing, worst))

    def save_icon_hashes():
        with open(os.path.join(gt.OUT, "icon-hashes.json"), "w") as f:
            json.dump(_icon_hashes, f, indent=1, sort_keys=True)

    def buttons_in(root, exclude_ptrs=()):
        return [b for b in descendants(root, QAbstractButton) if ptr(b) not in exclude_ptrs]

    # ---- link cursor ---------------------------------------------------------------------------

    def send_move(widget, pos, modifiers):
        ev = QMouseEvent(QEvent.MouseMove, QPointF(pos), widget.mapToGlobal(QPointF(pos)),
                         Qt.NoButton, Qt.NoButton, modifiers)
        QApplication.sendEvent(widget, ev)
        QApplication.processEvents()

    def arrow_tip(pixels, width, height, colour):
        """The themed-colour pixel nearest the top-left corner: the tip of the cursor's arrow."""
        best = None
        r0, g0, b0 = colour
        for y in range(height):
            for x in range(width):
                a, r, g, b = pixels(x, y)
                if a < 160 or max(abs(r - r0), abs(g - g0), abs(b - b0)) > 40:
                    continue
                if best is None or x + y < best[0] + best[1]:
                    best = (x, y)
        return best

    def server_cursor():
        """The cursor the X server is showing, through XFixes: (width, height, xhot, yhot, pixel
        getter returning (a, r, g, b)), or None. This is what the user sees, after any scaling
        the platform plugin applies to the QCursor."""
        import ctypes
        import ctypes.util

        class XFixesCursorImage(ctypes.Structure):
            _fields_ = [("x", ctypes.c_short), ("y", ctypes.c_short),
                        ("width", ctypes.c_ushort), ("height", ctypes.c_ushort),
                        ("xhot", ctypes.c_ushort), ("yhot", ctypes.c_ushort),
                        ("cursor_serial", ctypes.c_ulong), ("pixels", ctypes.POINTER(ctypes.c_ulong))]
        try:
            x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
            xfixes = ctypes.CDLL(ctypes.util.find_library("Xfixes") or "libXfixes.so.3")
        except OSError as e:
            report("NOTE cursor: no libX11/libXfixes (%s)" % e)
            return None
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x11.XFree.argtypes = [ctypes.c_void_p]
        xfixes.XFixesGetCursorImage.restype = ctypes.POINTER(XFixesCursorImage)
        xfixes.XFixesGetCursorImage.argtypes = [ctypes.c_void_p]
        dpy = x11.XOpenDisplay(None)
        if not dpy:
            return None
        try:
            img = xfixes.XFixesGetCursorImage(dpy)
            if not img:
                return None
            c = img.contents
            w, h = c.width, c.height
            words = [c.pixels[i] & 0xffffffff for i in range(w * h)]
            result = (w, h, c.xhot, c.yhot)
            x11.XFree(img)
        finally:
            x11.XCloseDisplay(dpy)

        def pixel(x, y):
            v = words[y * w + x]
            a = (v >> 24) & 0xff
            if a == 0:
                return 0, 0, 0, 0
            # XFixes pixels are premultiplied.
            return (a, min(255, ((v >> 16) & 0xff) * 255 // a), min(255, ((v >> 8) & 0xff) * 255 // a),
                    min(255, (v & 0xff) * 255 // a))
        return result + (pixel,)

    def check_link_cursor(widget):
        cur = widget.cursor()
        if not check(cur.shape() == Qt.BitmapCursor, "Ctrl+hover on a knob shows the link cursor (shape %r)"
                     % cur.shape()):
            return
        colour = theme_colours()[0]
        pix = cur.pixmap()
        dpr = pix.devicePixelRatio()
        img = pix.toImage()
        hot = cur.hotSpot()
        pix.save(os.path.join(gt.OUT, "link-cursor.png"))

        def qt_pixel(x, y):
            c = img.pixelColor(x, y)
            return c.alpha(), c.red(), c.green(), c.blue()
        tip = arrow_tip(qt_pixel, img.width(), img.height(), colour)
        report("NOTE link cursor: pixmap %dx%d at dpr %.2f, hotspot (%d, %d), arrow tip at device (%r)"
               % (img.width(), img.height(), dpr, hot.x(), hot.y(), tip))
        if check(tip is not None, "the link cursor has a themed arrow"):
            dist = max(abs(tip[0] / dpr - hot.x()), abs(tip[1] / dpr - hot.y()))
            check(dist <= 2.0, "the link cursor's hotspot sits on the arrow tip (%.1f logical px off)" % dist)
        server = server_cursor()
        if server is None:
            report("NOTE link cursor: could not read the X server's cursor")
            return
        w, h, xhot, yhot, pixel = server
        stip = arrow_tip(pixel, w, h, colour)
        scale = QApplication.instance().devicePixelRatio()
        report("NOTE X server cursor: %dx%d, hotspot (%d, %d), arrow tip (%r)" % (w, h, xhot, yhot, stip))
        if check(stip is not None, "the X server shows the themed link cursor"):
            dist = max(abs(stip[0] - xhot), abs(stip[1] - yhot))
            check(dist <= 2.0 * scale, "the X server's hotspot sits on the arrow tip (%d device px off)" % dist)

    # ---- node label geometry -------------------------------------------------------------------

    def name_items(graph, nodes):
        """Each node's name item, keyed by label, matched on its text. Never call parentItem()
        here: the wrapper PySide builds for the NodeGui parent deletes the node's graphics item
        when it is collected, and the next Effect.getSize() then crashes."""
        items = {}
        for node in nodes:
            label = node.getLabel()
            x, y, w, h = node_rect(node)
            area = graph.mapFromScene(QRectF(x - 60, y - 60, w + 120, h + 120)).boundingRect()
            for it in graph.items(area):
                if not isinstance(it, QGraphicsTextItem):
                    continue
                if it.toPlainText().split("\n")[0].strip().startswith(label[:20]):
                    items[label] = it
                    break
        return items

    def name_rects(graph, nodes):
        """Each node's name-item scene rect, keyed by label."""
        return dict((label, QRectF(it.sceneBoundingRect())) for label, it in name_items(graph, nodes).items())

    def label_line_xs(item):
        """For each block of a name item's document, the x of each of its laid-out lines."""
        doc = item.document()
        doc.documentLayout().documentSize()
        blocks = []
        block = doc.begin()
        while block.isValid():
            lay = block.layout()
            blocks.append([lay.lineAt(i).x() for i in range(lay.lineCount())])
            block = block.next()
        return blocks

    def check_label_alignment(graph, rects, nodes, wrapped_node):
        """Every block's first line starts at the same x (left-aligned rather than centred), a
        wrapped block's later lines sit one hanging indent right of its first, and the text block
        as a whole is centred in the space beside the node's plugin-icon column."""
        items = name_items(graph, nodes)
        centre_offsets = []
        for node in nodes:
            label = node.getLabel()
            it = items.get(label)
            r = rects.get(label)
            if it is None or r is None:
                check(False, "label align: found the name item of %r" % label)
                continue
            blocks = label_line_xs(it)
            firsts = [b[0] for b in blocks if b]
            check(firsts and max(firsts) - min(firsts) <= 0.5,
                  "label align %r: every line break starts at the same x (%r)" % (label[:24], firsts))
            centre_offsets.append(r.center().x() - node_rect(node)[0])
        icon_column = px(PLUGIN_ICON_COLUMN)
        expected = [icon_column + (node_rect(n)[2] - icon_column) / 2.0 for n in nodes]
        report("NOTE label centre offsets from the node's left edge: %r (expected %r)"
               % (["%.1f" % o for o in centre_offsets], ["%.1f" % e for e in expected]))
        check(centre_offsets and all(abs(o - e) <= 2.0 for o, e in zip(centre_offsets, expected)),
              "label align: each label block is centred beside the plugin-icon column")
        it = items.get(wrapped_node.getLabel())
        if check(it is not None, "label align: found the wrapped label's item"):
            blocks = label_line_xs(it)
            wrapped = next((b for b in blocks if len(b) > 1), None)
            report("NOTE wrapped label line x per block: %r" % blocks)
            if check(wrapped is not None, "label align: the long label wraps onto several lines"):
                indents = [x - wrapped[0] for x in wrapped[1:]]
                check(all(abs(d - px(LABEL_WRAP_INDENT)) <= 0.5 for d in indents),
                      "label align: wrapped lines hang %d px right of the first (%r)" % (LABEL_WRAP_INDENT, indents))

    def frame_nodes(graph, nodes, max_zoom=1.5):
        xs, ys, xe, ye = [], [], [], []
        for n in nodes:
            x, y, w, h = node_rect(n)
            xs.append(x)
            ys.append(y)
            xe.append(x + w)
            ye.append(y + h)
        vp = graph.viewport()
        zoom = min(max_zoom, vp.width() / (max(xe) - min(xs) + 160.0), vp.height() / (max(ye) - min(ys) + 160.0))
        graph.resetTransform()
        graph.scale(zoom, zoom)
        graph.centerOn((max(xe) + min(xs)) / 2.0, (max(ye) + min(ys)) / 2.0)
        QApplication.processEvents()

    def node_rect(node):
        x, y = node.getPosition()
        w, h = node.getSize()
        return x, y, w, h

    def check_wrap(rects, node, case, ref_height=None):
        x, y, w, h = node_rect(node)
        r = rects.get(node.getLabel())
        report("NOTE wrap %s: node %.1fx%.1f at (%.1f, %.1f)" % (case, w, h, x, y))
        check(w <= px(NODE_MAX_WIDTH) + NODE_OUTLINE + 0.5,
              "%s: node width %.1f <= max %.1f" % (case, w, px(NODE_MAX_WIDTH)))
        if not check(r is not None, "%s: found the node's name item" % case):
            return
        report("NOTE wrap %s: label rect (%.1f, %.1f) %.1fx%.1f" % (case, r.x(), r.y(), r.width(), r.height()))
        tol = 1.0
        inside = (r.left() >= x - tol and r.right() <= x + w + tol and
                  r.top() >= y - tol and r.bottom() <= y + h + tol)
        check(inside, "%s: label rect lies inside the node rect" % case)
        if ref_height is not None:
            check(h > ref_height + 0.5, "%s: node grew taller than a default-named one (%.1f > %.1f)"
                  % (case, h, ref_height))

    # ---- knob widgets --------------------------------------------------------------------------

    def knob_widgets(panel, knob_name):
        tag = "<b>%s</b>" % knob_name
        labels = []
        fields = []
        for w in descendants(panel):
            try:
                tt = w.toolTip()
            except RuntimeError:
                continue
            if not tt or tag not in tt[:200]:
                continue
            if isinstance(w, QLabel):
                labels.append(w)
            else:
                fields.append(w)
        field_ptrs = set(ptr(f) for f in fields)
        outer = [f for f in fields if f.parentWidget() is None or not is_inside(f.parentWidget(), field_ptrs)]
        return labels, outer

    def left_in(panel, widget):
        return widget.mapTo(panel, QPoint(0, 0)).x()

    def top_in(panel, widget):
        return widget.mapTo(panel, QPoint(0, 0)).y()

    def check_picker_label(panel, plugin, knob, want_text):
        labels, fields = knob_widgets(panel, knob)
        texts = [l.text().strip() for l in labels]
        report("NOTE %s %s: labels %r, %d field widget(s)" % (plugin, knob, texts, len(fields)))
        check(len(fields) > 0, "%s: the %s picker is shown" % (plugin, knob))
        check(any(texts), "%s: the %s picker has a non-empty label" % (plugin, knob))
        if want_text is not None:
            check(want_text in texts, "%s: the %s picker is labelled %r" % (plugin, knob, want_text))
        return labels, fields

    def label_containers(panel):
        """The per-row label cells: each knob label sits in a QHBoxLayout'd container widget."""
        cells = []
        for w in descendants(panel, QLabel):
            tt = w.toolTip() or ""
            if "<b>" not in tt[:60]:
                continue
            parent = w.parentWidget()
            if parent is not None and parent.layout() is not None and gt.class_name(parent) == "QWidget":
                cells.append(parent)
        return cells

    def first_of_class(widgets, cls_name):
        for f in widgets:
            if gt.class_name(f) == cls_name:
                return f
            for w in descendants(f):
                if gt.class_name(w) == cls_name:
                    return w
        return None

    def right_in(panel, widget):
        return left_in(panel, widget) + widget.width()

    def image_of(widget):
        return widget.grab().toImage()

    def max_brightness(img):
        best = 0
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                best = max(best, (c.red() + c.green() + c.blue()) / 3.0)
        return best

    def dominant_colour(img):
        counts = {}
        for y in range(img.height()):
            for x in range(img.width()):
                c = img.pixelColor(x, y)
                key = (c.red(), c.green(), c.blue())
                counts[key] = counts.get(key, 0) + 1
        return max(counts.items(), key=lambda kv: kv[1])[0] if counts else None

    def chroma(rgb):
        return max(rgb) - min(rgb)

    def check_channel_buttons(panel, area):
        """Checked R/G/B toggles are filled with their channel colour blended toward the panel,
        not the raw channel colour, and still read as red, green and blue."""
        buttons = [b for b in buttons_in(panel) if b.property("channelColor") is not None and b.isChecked()]
        coloured = []
        for b in buttons:
            col = b.property("channelColor")
            raw = (col.red(), col.green(), col.blue())
            if chroma(raw) > 0:
                coloured.append((b, raw))
        if not check(len(coloured) >= 3, "%s: found the checked R, G and B channel buttons (%d)" % (area, len(coloured))):
            return
        for b, raw in coloured:
            shown = dominant_colour(image_of(b))
            report("NOTE %s channel button %s: channel colour %r, drawn %r" % (area, b.text(), raw, shown))
            check(shown is not None and chroma(shown) <= chroma(raw) * CHANNEL_BUTTON_CHROMA_RATIO,
                  "%s: the %s button is less saturated than its channel colour (chroma %r vs %d)"
                  % (area, b.text(), shown and chroma(shown), chroma(raw)))
            check(shown is not None and chroma(shown) >= CHANNEL_BUTTON_MIN_CHROMA and
                  shown.index(max(shown)) == raw.index(max(raw)),
                  "%s: the %s button still reads as its channel's hue (%r)" % (area, b.text(), shown))

    def check_add_layer_button(panel, area, field_knob="mix"):
        """The + button sits under the layer combo, and both share the same left x as every
        other field-column widget (the [-] remove button moved to the row's end, so it no longer
        pushes the combo out of the field column)."""
        rows = [w for w in descendants(panel) if gt.class_name(w) == "LayerChannelRow"]
        plus = [b for b in buttons_in(panel) if b.text() == "+" and "layer row" in (b.toolTip() or "")]
        if not check(rows and plus, "%s: found the channel picker's layer row and + button" % area):
            return
        combo = first_of_class([rows[0]], "ComboBox")
        if not check(combo is not None, "%s: found the layer row's combo" % area):
            return
        a, b = left_in(panel, plus[0]), left_in(panel, combo)
        check(abs(a - b) <= ALIGN_TOLERANCE, "%s: the + button sits under the layer combo (x %d vs %d)" % (area, a, b))
        check(top_in(panel, plus[0]) > top_in(panel, combo), "%s: the + button is below the combo" % area)
        _l, field_cells = knob_widgets(panel, field_knob)
        if check(bool(field_cells), "%s: found the %s field to compare against the field column" % (area, field_knob)):
            field_left = min(left_in(panel, f) for f in field_cells)
            check(abs(b - field_left) <= ALIGN_TOLERANCE,
                  "%s: the layer combo's left x matches the field column (%d vs %s %d)" % (area, b, field_knob, field_left))
            check(abs(a - field_left) <= ALIGN_TOLERANCE,
                  "%s: the + button shares the field column's left x (%d vs %s %d)" % (area, a, field_knob, field_left))

    def check_button_in_field_column(panel, area, text, field_knob):
        btns = [b for b in buttons_in(panel) if b.text().replace("&", "") == text]
        _l, fields = knob_widgets(panel, field_knob)
        if not check(btns and fields, "%s: found the %s button and the %s field" % (area, text, field_knob)):
            return
        a = left_in(panel, btns[0])
        b = min(left_in(panel, f) for f in fields)
        check(abs(a - b) <= ALIGN_TOLERANCE,
              "%s: the %s button lines up with the field column (x %d vs %s field %d)" % (area, text, a, field_knob, b))

    def check_uv_rows(panel):
        """UV Offset's expanded U and V boxes sit together, and its dimension switch lines up with
        UV Scale's at the row's end."""
        switches = {}
        for knob in ("uvOffset", "uvScale"):
            _l, fields = knob_widgets(panel, knob)
            spins = [f for f in fields if gt.class_name(f) == "SpinBox"]
            if not check(bool(spins), "IDistort: found the %s spin boxes" % knob):
                return
            mid_y = top_in(panel, spins[0]) + spins[0].height() // 2
            twos = [b for b in buttons_in(panel) if b.text() == "2" and
                    top_in(panel, b) <= mid_y <= top_in(panel, b) + b.height()]
            if not check(bool(twos), "IDistort: found the %s dimension switch" % knob):
                return
            switches[knob] = twos[0]
            if knob == "uvOffset" and len(spins) >= 2:
                spins.sort(key=lambda w: left_in(panel, w))
                gap = left_in(panel, spins[1]) - right_in(panel, spins[0])
                check(gap <= px(24), "IDistort: the U and V boxes sit together (gap %d px)" % gap)
        a, b = right_in(panel, switches["uvOffset"]), right_in(panel, switches["uvScale"])
        check(abs(a - b) <= ALIGN_TOLERANCE,
              "IDistort: the UV Offset and UV Scale dimension switches share a right edge (%d vs %d)" % (a, b))

    def check_merge_rows(panel):
        """The A/B/Output row labels are drawn in the normal label colour, in the label column,
        and each row's first channel toggle starts at the same x."""
        _l, op_fields = knob_widgets(panel, "operation")
        op_labels = [l for l in _l if l.text().strip()]
        reference = max_brightness(image_of(op_labels[0])) if op_labels else None
        lefts = []
        for knob in ("AChannels", "BChannels", "OutputChannels"):
            labels, _f = knob_widgets(panel, knob)
            labels = [l for l in labels if l.text().strip()]
            if not check(bool(labels), "Merge: found the %s row label" % knob):
                continue
            bright = max_brightness(image_of(labels[0]))
            check(reference is not None and bright >= reference * ROW_LABEL_BRIGHTNESS_RATIO,
                  "Merge: the %s row label is drawn in the normal text colour (brightness %.0f vs %r)"
                  % (knob, bright, reference))
            if op_labels:
                a, b = right_in(panel, labels[0]), right_in(panel, op_labels[0])
                check(abs(a - b) <= ALIGN_TOLERANCE,
                      "Merge: the %s row label is right-aligned in the label column (%d vs %d)" % (knob, a, b))
            _l2, toggles = knob_widgets(panel, knob + "R")
            if check(bool(toggles), "Merge: found the %sR toggle" % knob):
                lefts.append(min(left_in(panel, t) for t in toggles))
        report("NOTE Merge first toggle left x per row: %r" % lefts)
        check(len(lefts) == 3 and max(lefts) - min(lefts) <= ALIGN_TOLERANCE,
              "Merge: the A, B and Output toggles start at the same x (%r)" % lefts)

    # ---- steps ---------------------------------------------------------------------------------

    def steps():
        ok = yield ("until", lambda: root_graph() is not None, STARTUP_TIMEOUT_S)
        gt.require(ok, "node graph widget is up")
        record_binary()
        main = gui_widget()
        if main is not None:
            main.window().setGeometry(QGuiApplication.primaryScreen().availableGeometry())
            yield ("sleep", SETTLE_MS)
            report("NOTE main window %dx%d" % (main.window().width(), main.window().height()))
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # ---- wrap
        gt.section("wrap")
        graph = root_graph()
        reference = create("net.sf.cimg.CImgBlur", -300, 0)
        short = create("net.sf.cimg.CImgBlur", 0, 0, "Blur_abc")
        mid = create("net.sf.cimg.CImgBlur", 300, 0, "Blur with a thirty char label!")
        long_name = "Blur_" + "".join("%d" % (i % 10) for i in range(75))
        longn = create("net.sf.cimg.CImgBlur", 700, 0, long_name)
        grade = create("net.sf.openfx.GradePlugin", 0, 300, "GradeWithUserLabel")
        user_text = grade.getParam("userTextArea")
        if check(user_text is not None, "Grade has a user label (userTextArea) param"):
            user_text.set("first line of a user label\nsecond, somewhat longer line of the user label")
        icon_node = create("IDistort", 300, 300, "IDistort with plugin icon label")
        preview = create("net.sf.cimg.CImgBlur", 700, 300, "Blur with a preview, long label")
        prev_param = preview.getParam("enablePreview")
        if check(prev_param is not None, "Blur has the enablePreview param"):
            prev_param.set(True)
        backdrop = create("fr.inria.built-in.BackDrop", 0, 700)
        dot = create("fr.inria.built-in.Dot", 600, 700)
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)
        backdrop_size = backdrop.getSize()
        dot_size = dot.getSize()
        backdrop.setLabel("A backdrop whose label is far longer than its frame")
        dot.setLabel("A dot with a long label that must not resize it")
        yield ("sleep", SETTLE_MS)

        _rx, _ry, rw, rh = node_rect(reference)
        _sx, _sy, sw, sh = node_rect(short)
        check(abs(sw - rw) <= 1.5 and abs(sh - rh) <= 1.5,
              "8 chars: a short name keeps the size of a default-named Blur (%.1fx%.1f vs %.1fx%.1f)"
              % (sw, sh, rw, rh))
        app.clearSelection()
        frame_nodes(graph, [short, mid, longn, grade, icon_node, preview])
        yield ("sleep", SETTLE_MS)
        rects = name_rects(graph, [short, mid, longn, grade, icon_node, preview])
        check_wrap(rects, short, "8 chars")
        check_wrap(rects, mid, "30 chars")
        check_wrap(rects, longn, "80 chars, no spaces", rh)
        check_wrap(rects, grade, "Grade with a multi-line user label")
        check_wrap(rects, icon_node, "30 chars with a plugin icon")
        check_wrap(rects, preview, "30 chars with a preview")
        check_label_alignment(graph, rects, [short, mid, longn, grade, icon_node, preview], longn)
        check(all(abs(a - b) <= 1.5 for a, b in zip(backdrop.getSize(), backdrop_size)),
              "Backdrop keeps its size with a long label (%r -> %r)" % (backdrop_size, backdrop.getSize()))
        check(all(abs(a - b) <= 1.5 for a, b in zip(dot.getSize(), dot_size)),
              "Dot keeps its size with a long label (%r -> %r)" % (dot_size, dot.getSize()))
        crop_shot(graph.viewport(), "wrap-nodes")
        for n in [reference, backdrop, dot, short, mid, longn, grade, icon_node, preview]:
            n.destroy()
        yield ("sleep", SETTLE_MS)

        # ---- labels
        gt.section("labels")
        for plugin_id, short_name, expected in (("net.sf.openfx.Premult", "Premult", "Premult by"),
                                                ("net.sf.openfx.Unpremult", "Unpremult", "Unpremult by")):
            gt.close_all_panels()
            node = create(plugin_id, 0, 0)
            yield ("sleep", PANEL_MS)
            panel = panel_for(node)
            if check(panel is not None, "%s: settings panel is open" % short_name):
                _labels, pick_fields = check_picker_label(panel, short_name, "hostUnPremultByChannel", expected)
                _l, layer_fields = knob_widgets(panel, "hostInputLayer")
                if pick_fields and layer_fields:
                    check(abs(top_in(panel, pick_fields[0]) - top_in(panel, layer_fields[0])) > 2,
                          "%s: the picker sits on its own row, not the layer row" % short_name)
                    pick_combo = first_of_class(pick_fields, "ComboBox")
                    layer_combo = first_of_class(layer_fields, "ComboBox")
                    if check(pick_combo is not None and layer_combo is not None,
                             "%s: found the layer and picker combos" % short_name):
                        a, b = left_in(panel, layer_combo), left_in(panel, pick_combo)
                        check(abs(a - b) <= ALIGN_TOLERANCE,
                              "%s: the layer and %s combos share a left x (%d vs %d)" % (short_name, expected, a, b))
                crop_shot(panel, "labels-%s" % short_name.lower())
            node.destroy()

        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # ---- icons
        gt.section("icons")
        toolbars = [t for t in QApplication.allWidgets() if isinstance(t, QToolBar) and t.isVisible()]
        toolbox = toolbars[0] if toolbars else None
        if check(toolbox is not None, "found the left toolbox"):
            themed, probed, off = check_icons("toolbox", buttons_in(toolbox))
            check(probed > 0 and themed == probed,
                  "toolbox: every grouping icon is drawn in the theme colour (%d of %d; off: %s)"
                  % (themed, probed, ", ".join(off) or "none"))
            crop_shot(toolbox, "icons-toolbox")
            compare_toolbox_with_baseline(os.path.join(gt.OUT, "icons-toolbox.png"))

        viewer = create("fr.inria.built-in.Viewer", 0, 400)
        gt.close_all_panels()
        make_tab_current("pane1", "viewer")
        ok = yield ("until", lambda: any(w.isVisible() for w in gt.widgets_of("ViewerTab")), 15)
        viewer_tab = next((w for w in gt.widgets_of("ViewerTab") if w.isVisible()), None)
        if check(ok and viewer_tab is not None, "a viewer tab is shown"):
            yield ("sleep", SETTLE_MS)
            viewer_buttons = buttons_in(viewer_tab)
            check_icons("viewer toolbar and timeline", viewer_buttons)
            crop_shot(viewer_tab, "icons-viewer")
        viewer_ptrs = set(ptr(b) for b in (buttons_in(viewer_tab) if viewer_tab is not None else []))

        grade = create("net.sf.openfx.GradePlugin", 300, 0)
        yield ("sleep", PANEL_MS)
        panel = panel_for(grade)
        if check(panel is not None, "Grade: settings panel is open"):
            header = [b for b in buttons_in(panel)
                      if top_in(panel, b) < px(40) and gt.class_name(b) != "NodeColorButton"]
            check_icons("Grade panel header", header)
            crop_shot(panel, "icons-grade-panel")
        grade.destroy()
        gt.close_all_panels()

        for plugin_id, area in (("fr.inria.built-in.RotoPaint", "RotoPaint viewer toolbar"),
                                ("fr.inria.built-in.Tracker", "Tracker viewer toolbar")):
            gt.close_all_panels()
            node = create(plugin_id, 600, 0)
            viewer.connectInput(0, node)
            app.clearSelection()
            app.selectNode(node, True)
            yield ("sleep", PANEL_MS)
            if viewer_tab is not None:
                extra = buttons_in(viewer_tab, viewer_ptrs)
                check_icons(area, extra)
                crop_shot(viewer_tab, "icons-" + area.split()[0].lower() + "-viewer")
            node.destroy()
            yield ("sleep", SETTLE_MS)
        gt.close_all_panels()

        anim = create("net.sf.cimg.CImgBlur", 0, 200, "AnimatedBlur")
        size = anim.getParam("size")
        if check(size is not None, "Blur has a size param to animate"):
            for frame, value in ((1, 0.0), (10, 8.0), (20, 3.0), (30, 12.0)):
                size.setValueAtTime(value, frame, 0)
                size.setValueAtTime(value * 0.5, frame, 1)
            kinds = NatronEngine.Natron.KeyframeTypeEnum
            # One key per marker glyph, so the dope-sheet shots show C, H, R and Z the right way up.
            for frame, dim, kind in ((1, 0, "eKeyframeTypeConstant"), (10, 0, "eKeyframeTypeLinear"),
                                     (20, 0, "eKeyframeTypeCatmullRom"), (30, 0, "eKeyframeTypeSmooth"),
                                     (1, 1, "eKeyframeTypeCubic"), (10, 1, "eKeyframeTypeHorizontal"),
                                     (20, 1, "eKeyframeTypeFree")):
                size.setInterpolationAtTime(frame, getattr(kinds, kind), dim)
        yield ("sleep", SETTLE_MS)

        def interp_menu_handler(area, shot_name, seen):
            def on_menu(menu):
                seen["menu"] = menu is not None
                if menu is None:
                    return
                interp = next((a for a in menu.actions()
                               if a.menu() is not None and "interpolation" in a.text().lower()), None)
                seen["interp"] = interp is not None
                if interp is None:
                    close_popups()
                    return
                sub = interp.menu()
                entries = [a for a in sub.actions() if not a.isSeparator()]
                seen["actions"] = [a.text() for a in entries]
                seen["icons"] = icon_entries(entries)
                geo = menu.actionGeometry(interp)
                sub.popup(menu.mapToGlobal(geo.topRight()))

                def finish_menu():
                    try:
                        gt.shot(shot_name + ".png")
                    finally:
                        close_popups()
                QTimer.singleShot(400, finish_menu)
            return on_menu

        for tab_label, view_class, area, shot_name in (
                ("dope", "DopeSheetView", "dope-sheet interpolation menu", "icons-dopesheet-menu"),
                ("curve", "CurveWidget", "curve-editor interpolation menu", "icons-curve-editor-menu")):
            make_tab_current("pane3", tab_label)
            yield ("sleep", PANEL_MS)
            view = next((w for w in gt.widgets_of(view_class) if w.isVisible()), None)
            if not check(view is not None, "%s: the %s is shown" % (area, view_class)):
                continue
            if view_class == "DopeSheetView":
                crop_shot(pane("pane3"), "icons-dopesheet-keyframes")
                QMetaObject.invokeMethod(view, "onSelectedAllTriggered")
                yield ("sleep", SETTLE_MS)
                crop_shot(pane("pane3"), "icons-dopesheet-keyframes-selected")
            else:
                crop_shot(pane("pane3"), "icons-curve-editor")
            seen = {}
            right_click(view, QPoint(view.width() // 2, view.height() // 2),
                        interp_menu_handler(area, shot_name, seen))
            yield ("sleep", SETTLE_MS)
            check(seen.get("menu", False), "%s: the right-click menu opened" % area)
            check(seen.get("interp", False), "%s: it has an Interpolation submenu" % area)
            actions = seen.get("actions", [])
            report("NOTE %s entries: %r" % (area, actions))
            if seen.get("icons") or view_class == "DopeSheetView":
                check_icon_entries(area, seen.get("icons", []))
            else:
                report("NOTE %s: its entries have no icons" % area)
            check(any(t.replace("&", "").lower().startswith("free") for t in actions),
                  "%s: has a Free entry" % area)
        make_tab_current("pane3", "node graph")
        anim.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        split = app.getTabWidget("pane1").splitHorizontally()
        yield ("sleep", SETTLE_MS)
        split_pane = pane(split.getScriptName()) if split is not None else None
        if check(split_pane is not None, "split the viewer pane"):
            QMetaObject.invokeMethod(split_pane, "moveScriptEditorHere")
            yield ("sleep", PANEL_MS)
            editor = next((w for w in gt.widgets_of("ScriptEditor") if w.isVisible()), None)
            if check(editor is not None, "the Script Editor is shown in the new pane"):
                check_icons("Script Editor toolbar", buttons_in(editor))
                editor_ptrs = set(ptr(w) for w in [editor] + editor.findChildren(QWidget))
                header_buttons = [b for b in buttons_in(split_pane) if ptr(b) not in editor_ptrs]
                check_icons("split pane header", header_buttons)
            crop_shot(split_pane, "icons-split-pane-script-editor")

        grade = create("net.sf.openfx.GradePlugin", 300, 0)
        yield ("sleep", PANEL_MS)
        scroll = next((w for w in QApplication.allWidgets()
                       if isinstance(w, QScrollArea) and w.objectName() == "Properties" and w.isVisible()), None)
        bin_widget = scroll.parentWidget() if scroll is not None else None
        if check(bin_widget is not None, "the properties bin is shown"):
            panel_ptrs = set()
            for p in visible_panels():
                panel_ptrs.update(ptr(w) for w in [p] + p.findChildren(QWidget))
            bin_buttons = [b for b in buttons_in(bin_widget) if ptr(b) not in panel_ptrs]
            check_icons("properties bin buttons", bin_buttons)
            if bin_buttons:
                crop_shot(bin_buttons[0].parentWidget(), "icons-properties-bin-buttons", pad=4)
            crop_shot(bin_widget, "icons-properties-bin")
        grade.destroy()
        gt.close_all_panels()
        save_icon_hashes()
        yield ("sleep", SETTLE_MS)

        # ---- UAT round: icon sizes, bin row, combos, icon colour, live recolour, legacy icons
        gt.section("uat")
        headers = [h for h in gt.widgets_of("TabWidgetHeader") if h.isVisible()]
        if check(bool(headers), "found a pane header"):
            check_header_buttons("pane header", [b for h in headers for b in h.findChildren(QAbstractButton)])

        grade = create("net.sf.openfx.GradePlugin", 300, 0)
        yield ("sleep", PANEL_MS)
        scroll = next((w for w in QApplication.allWidgets()
                       if isinstance(w, QScrollArea) and w.objectName() == "Properties" and w.isVisible()), None)
        bin_widget = scroll.parentWidget() if scroll is not None else None
        bin_buttons = []
        if check(bin_widget is not None, "the properties bin is shown"):
            panel_ptrs = set()
            for p in visible_panels():
                panel_ptrs.update(ptr(w) for w in [p] + p.findChildren(QWidget))
            bin_buttons = [b for b in buttons_in(bin_widget) if ptr(b) not in panel_ptrs and not b.icon().isNull()]
            check_header_buttons("properties bin", bin_buttons)

            spins = [w for w in descendants(bin_widget) if gt.class_name(w) == "SpinBox" and ptr(w) not in panel_ptrs]
            if check(bool(spins) and bool(bin_buttons), "found the bin's max-panels box and buttons"):
                spin = spins[0]
                fm = QFontMetrics(spin.font())
                check(spin.height() >= fm.height() + 2,
                      "max-panels box is tall enough for its text (%d px for a %d px line)" % (spin.height(), fm.height()))
                check(spin.width() >= fm.horizontalAdvance("99") + 4,
                      "max-panels box is wide enough for two digits (%d px)" % spin.width())
                spin_mid = spin.mapTo(bin_widget, QPoint(0, spin.height() // 2)).y()
                button_mid = bin_buttons[0].mapTo(bin_widget, QPoint(0, bin_buttons[0].height() // 2)).y()
                check(abs(spin_mid - button_mid) <= 1,
                      "max-panels box is centred on the bin buttons (%d vs %d)" % (spin_mid, button_mid))
                crop_shot(spin.parentWidget(), "uat-properties-bin-row", pad=4)

        if bin_buttons:
            want = text_colour()
            got = icon_colour(bin_buttons[0])
            rendered = label_text_colour()
            check(colour_near(got, want, ICON_TEXT_TOLERANCE),
                  "an icon's strokes are the Preferences text colour (%s vs %s)" % (fmt_colour(got), fmt_colour(want)))
            check(colour_near(got, rendered, ICON_TEXT_TOLERANCE),
                  "an icon's strokes match the rendered label text colour (%s vs %s)" % (fmt_colour(got), fmt_colour(rendered)))

        viewer_tab = next((w for w in gt.widgets_of("ViewerTab") if w.isVisible()), None)
        if viewer_tab is None:
            make_tab_current("pane1", "viewer")
            yield ("sleep", SETTLE_MS)
            viewer_tab = next((w for w in gt.widgets_of("ViewerTab") if w.isVisible()), None)
        if check(viewer_tab is not None, "a viewer tab is shown"):
            formats = [w for w in descendants(viewer_tab)
                       if gt.class_name(w) == "ComboBox" and "time display format" in (w.toolTip() or "").lower()]
            if check(bool(formats), "found the time display format menu"):
                combo = formats[0]
                fm = QFontMetrics(combo.font())
                need = (max(fm.horizontalAdvance("TC"), fm.horizontalAdvance("TF")) + fm.horizontalAdvance("x") // 2
                        + 2 * DROP_DOWN_ICON + 2)
                check(combo.width() >= need,
                      "time display format menu fits its text and arrow (%d px, needs %d)" % (combo.width(), need))
                crop_shot(combo.parentWidget(), "uat-time-format")

        if bin_buttons:
            settings = NatronEngine.natron.getSettings()
            text_param = settings.getParam("text")
            old = [text_param.getValue(i) for i in range(3)]
            start = time.time()
            for i, v in enumerate(RECOLOUR_TEST_COLOUR):
                text_param.setValue(v, i)
            elapsed = time.time() - start
            check(elapsed < 0.5, "changing the text colour returns at once (%.2f s for three channels)" % elapsed)
            yield ("sleep", THEME_RELOAD_MS)
            want = text_colour()
            check(colour_near(icon_colour(bin_buttons[0]), want, ICON_TEXT_TOLERANCE),
                  "a text colour change recolours a button icon (%s vs %s)"
                  % (fmt_colour(icon_colour(bin_buttons[0])), fmt_colour(want)))
            label = themed_label()
            if check(label is not None, "found a label showing a themed icon"):
                got = label_pixmap_colour(label)
                check(colour_near(got, want, ICON_TEXT_TOLERANCE),
                      "a text colour change recolours a label's icon (%s vs %s)" % (fmt_colour(got), fmt_colour(want)))
            check(colour_near(label_text_colour(), want, ICON_TEXT_TOLERANCE),
                  "a text colour change restyles the label text (%s)" % fmt_colour(label_text_colour()))
            gt.shot("uat-recoloured.png")
            for i, v in enumerate(old):
                text_param.setValue(v, i)
            yield ("sleep", THEME_RELOAD_MS)
            check(colour_near(icon_colour(bin_buttons[0]), text_colour(), ICON_TEXT_TOLERANCE),
                  "restoring the text colour restores the icon colour")
        grade.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gui = gui_widget()
        if check(gui is not None, "found the main window"):
            menu_actions = []
            for top in gui.menuBar().actions():
                if top.menu() is not None:
                    menu_actions.extend(a for a in top.menu().actions() if not a.icon().isNull())
            check_icon_entries("main menu", icon_entries(menu_actions))

            seen = {}

            def inspect_dialog():
                dialogs = [d for d in gt.widgets_of("SequenceFileDialog") if d.isVisible()]
                if not dialogs:
                    if seen.get("tries", 0) < 40:
                        seen["tries"] = seen.get("tries", 0) + 1
                        QTimer.singleShot(200, inspect_dialog)
                    return
                dialog = dialogs[0]
                seen["dialog"] = True
                entries = icon_entries(buttons_in(dialog))
                for view in dialog.findChildren(QWidget):
                    model = view.model() if hasattr(view, "model") and callable(view.model) else None
                    if model is None or gt.class_name(view) != "FavoriteView":
                        continue
                    for row in range(model.rowCount()):
                        deco = model.index(row, 0).data(Qt.DecorationRole)
                        if isinstance(deco, QIcon) and not deco.isNull():
                            entries.append(("favourite %d" % row, deco))
                seen["entries"] = entries
                crop_shot(dialog, "uat-file-dialog")
                dialog.reject()

            QTimer.singleShot(500, inspect_dialog)
            # openProject() runs the modal file dialog; inspect_dialog() rejects it, which returns here.
            QMetaObject.invokeMethod(gui, "openProject")
            QApplication.processEvents()
            if check(seen.get("dialog", False), "the open-project file dialog opened"):
                check_icon_entries("file dialog", seen.get("entries", []))
        yield ("sleep", SETTLE_MS)

        # ---- link cursor
        gt.section("cursor")
        blur = create("net.sf.cimg.CImgBlur", 0, 0, "CursorBlur")
        yield ("sleep", PANEL_MS)
        panel = panel_for(blur)
        if check(panel is not None, "Blur: settings panel is open"):
            # KnobSpinBox has no Q_OBJECT, so its meta class name is its base's.
            spins = [w for w in descendants(panel) if gt.class_name(w) == "SpinBox"
                     and "<b>size</b>" in (w.toolTip() or "")[:200]]
            if check(bool(spins), "found the Blur size spin box"):
                spin = spins[0]
                centre = QPoint(spin.width() // 2, spin.height() // 2)
                # The warp makes X send a motion event without Ctrl, which unsets the link
                # cursor; let it arrive before the Ctrl move.
                QCursor.setPos(spin.mapToGlobal(centre))
                yield ("sleep", SETTLE_MS)
                send_move(spin, centre, Qt.ControlModifier)
                yield ("sleep", 100)
                check_link_cursor(spin)
                send_move(spin, centre, Qt.NoModifier)
        blur.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        # ---- density
        gt.section("density")
        grade = create("net.sf.openfx.GradePlugin", 0, 0)
        merge = create("net.sf.openfx.MergePlugin", 300, 0)
        yield ("sleep", PANEL_MS)
        gt.shot("density-main-window.png")
        panel = panel_for(grade)
        if check(panel is not None, "Grade: settings panel is open"):
            crop_shot(panel, "density-grade-panel")
            cells = label_containers(panel)
            bottoms = [c.layout().contentsMargins().bottom() for c in cells]
            report("NOTE Grade label cell bottom margins: %r" % sorted(set(bottoms)))
            check(bottoms and sum(1 for b in bottoms if near(b, ROW_GAP)) * 2 > len(bottoms),
                  "Grade: most knob rows use the %d px row gap" % ROW_GAP)
            margins = []
            for w in [panel] + panel.findChildren(QWidget):
                lay = w.layout()
                if lay is not None:
                    m = lay.contentsMargins()
                    margins.append((m.left(), m.top(), m.right(), m.bottom()))
            check(any(all(near(v, PANEL_MARGIN) for v in m) for m in margins),
                  "Grade: the panel body has a %d px margin on every side" % PANEL_MARGIN)
            check_channel_buttons(panel, "Grade")
            check_add_layer_button(panel, "Grade")
            check_button_in_field_column(panel, "Grade", "Normalize", "mix")
        merge_panel = panel_for(merge)
        if check(merge_panel is not None, "Merge: settings panel is open"):
            crop_shot(merge_panel, "density-merge-panel")
            check_merge_rows(merge_panel)
        tab_pane = pane("pane3")
        if check(tab_pane is not None and tab_pane.layout() is not None, "found the node-graph pane"):
            top = tab_pane.layout().contentsMargins().top()
            check(near(top, PANE_TOP_MARGIN), "pane tab bar sits %d px below the pane top (got %d)"
                  % (PANE_TOP_MARGIN, top))
            bars = [b for b in tab_pane.findChildren(QTabBar) if b.isVisible() and b.count() > 0]
            if check(bool(bars), "found the node-graph pane's tab bar"):
                h = bars[0].tabRect(0).height()
                check(h <= px(TAB_HEIGHT) + 0.5, "pane tabs are at most %d px tall (got %d)" % (TAB_HEIGHT, h))
        grade.destroy()
        merge.destroy()
        gt.close_all_panels()
        yield ("sleep", SETTLE_MS)

        gui = gui_widget()
        if check(gui is not None, "found the main window"):
            QMetaObject.invokeMethod(gui, "showSettings")
            ok = yield ("until", lambda: any(w.isVisible() for w in gt.widgets_of("PreferencesPanel")), 10)
            prefs = next((w for w in gt.widgets_of("PreferencesPanel") if w.isVisible()), None)
            if check(ok and prefs is not None, "the Preferences window opened"):
                prefs.raise_()
                yield ("sleep", PANEL_MS)
                crop_shot(prefs, "density-preferences")
                prefs.close()
                yield ("sleep", SETTLE_MS)

        graph = root_graph()
        seen = {}

        def on_graph_menu(menu):
            seen["menu"] = menu is not None
            if menu is None:
                return
            acts = [a for a in menu.actions() if not a.isSeparator() and a.isVisible()]
            seen["top"] = menu.actionGeometry(acts[0]).top() if acts else None
            seen["count"] = len(acts)
            gt.shot("density-node-graph-menu.png")
            close_popups()

        vp = graph.viewport()
        right_click(vp, QPoint(vp.width() // 2, vp.height() // 2), on_graph_menu)
        yield ("sleep", SETTLE_MS)
        if check(seen.get("menu", False), "the node-graph right-click menu opened"):
            top = seen.get("top")
            report("NOTE node-graph menu: %r items, first item top y %r" % (seen.get("count"), top))
            check(top is not None and top >= px(MENU_TOP_PADDING) - 0.5,
                  "the node-graph menu has at least %d px above its first item (got %r)" % (MENU_TOP_PADDING, top))

        gt.close_all_panels()
        props = {"CreateNodeArgsPropSettingsOpened": NatronEngine.BoolNodeCreationProperty(True)}
        writer = app.createWriter(os.path.join(gt.OUT, "density.####.exr"), None, props)
        if check(writer is not None, "created a Write node"):
            writer.setPosition(0, 300)
            groups = [p for p in writer.getParams() if isinstance(p, NatronEngine.GroupParam)]
            report("NOTE Write groups: %r" % [p.getScriptName() for p in groups])
            for g in groups:
                g.setOpened(True)
            yield ("sleep", PANEL_MS)
            panel = panel_for(writer)
            if check(panel is not None, "Write: settings panel is open"):
                lefts = sorted(set(c.layout().contentsMargins().left() for c in label_containers(panel)))
                report("NOTE Write label cell left margins: %r" % lefts)
                check(any(near(v, GROUP_INDENT) for v in lefts),
                      "Write: a group's child rows are indented %d px" % GROUP_INDENT)
                crop_shot(panel, "density-write-panel-group")

        faulthandler.cancel_dump_traceback_later()

    gt.run(steps())
except Exception:
    try:
        gt.check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
