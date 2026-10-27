"""Plumbing shared by the Natron GUI test scripts that Tests/gui/run-gui-test.sh runs.

Results go to $NATRON_GUI_TEST_OUT/results.txt (default build/gui-test-out/) as PASS/FAIL/SHOT/
NOTE lines, one SECTION line per section when sections are used, and a final SUMMARY line; the
process exits 0 only if every check passed.

Steps run as a generator on the Qt event loop (see run()), so Natron keeps rendering between them:
    yield ("sleep", ms)
    ok = yield ("until", predicate, timeout_seconds)
"""
import ctypes
import ctypes.util
import os
import time
import traceback

import NatronEngine
from PySide6.QtCore import QTimer
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import QApplication

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
OUT = os.environ.get("NATRON_GUI_TEST_OUT", os.path.join(ROOT, "build", "gui-test-out"))
FIXTURES = os.path.join(ROOT, "Tests", "fixtures")

if not os.path.isdir(OUT):
    os.makedirs(OUT)
_results = open(os.path.join(OUT, "results.txt"), "w")
_failures = []
_sections = []


def report(line):
    _results.write(line + "\n")
    _results.flush()


def section(name):
    """Later checks count towards, and are prefixed with, this section until the next one."""
    _sections.append([name, 0, 0])


def check(ok, label):
    prefix = ("[%s] " % _sections[-1][0]) if _sections else ""
    report(("PASS " if ok else "FAIL ") + prefix + label)
    if _sections:
        _sections[-1][1 if ok else 2] += 1
    if not ok:
        _failures.append(prefix + label)
    return ok


def finish():
    for name, passed, failed in _sections:
        report("SECTION %s: %d passed, %d failed" % (name, passed, failed))
    report("SUMMARY %d failure(s)" % len(_failures))
    _results.close()
    os._exit(1 if _failures else 0)


def require(ok, label, detail=""):
    """A failed setup step ends the run at once: nothing after it could mean anything."""
    check(ok, label + (" (" + detail + ")" if detail and not ok else ""))
    if not ok:
        finish()


_plugin_ids = set(NatronEngine.natron.getPluginIDs())


def plugin_problem(plugin_id):
    return "plugin %s; OFX_PLUGIN_PATH=%r" % (
        "registered but createNode returned None" if plugin_id in _plugin_ids else "not registered",
        os.environ.get("OFX_PLUGIN_PATH"))


def class_name(w):
    return w.metaObject().className().split("::")[-1]


_menu_owners = {}


def menu_of(owner):
    """owner.menu() for a QAction or QToolButton, usable for the rest of the run.

    PySide makes the returned menu's wrapper a child of owner's wrapper, and marks it deleted once
    owner's wrapper is collected, although the C++ menu lives on; keeping owner's wrapper alive
    keeps the menu's. Every call must go through here: a plain .menu() on a temporary wrapper
    re-parents the menu's wrapper to that temporary."""
    menu = owner.menu()
    if menu is not None:
        _menu_owners[id(owner)] = owner
    return menu


def widgets_of(name):
    found = []
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == name:
                found.append(w)
        except RuntimeError:
            continue
    return found


def close_all_panels():
    from PySide6.QtCore import QMetaObject
    for w in QApplication.allWidgets():
        try:
            if w.metaObject().indexOfMethod("closePanel()") >= 0 and w.isVisible():
                QMetaObject.invokeMethod(w, "closePanel")
        except RuntimeError:
            continue
    QApplication.processEvents()


def any_panel_open():
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == "NodeSettingsPanel" and w.isVisible():
                return True
        except RuntimeError:
            continue
    return False


def viewer_up():
    return any(w.isVisible() for w in widgets_of("ViewerGL"))


def viewer_messages():
    """The persistent messages the viewer is drawing, one string per message."""
    messages = []
    for w in widgets_of("ViewerGL"):
        value = w.property("natronPersistentMessages")
        if value:
            messages.extend(str(m) for m in value)
    return messages


def viewer_centre_colour():
    """The colour on screen at the centre of the first visible viewer, or None if there is none.
    Read from a screen grab: a widget grab of the GL viewer can come out blank under llvmpipe."""
    visible = [w for w in widgets_of("ViewerGL") if w.isVisible()]
    if not visible:
        return None
    w = visible[0]
    centre = w.mapToGlobal(w.rect().center())
    image = QGuiApplication.primaryScreen().grabWindow(0).toImage()
    c = image.pixelColor(centre.x(), centre.y())
    return (c.red(), c.green(), c.blue())


def shot(name):
    QApplication.processEvents()
    path = os.path.join(OUT, name)
    if QGuiApplication.primaryScreen().grabWindow(0).save(path):
        report("SHOT " + path)
    else:
        report("NOTE could not save screenshot " + path)
    return path


HOST_DISPLAY = bool(os.environ.get("GUI_TEST_HOST_DISPLAY"))
HOST_WINDOW_SIZE = tuple(int(n) for n in os.environ.get("GUI_TEST_SCREEN_SIZE", "1920x1080").split("x"))
HOST_ON_SCREEN_TIMEOUT_S = 60.0
_host_window = []


def pin_host_window():
    """On a shared desktop, pins the main window above others at the primary screen's top left:
    screen grabs read the root window, so a covering window or another monitor would be grabbed
    instead of the viewer.

    Returns True the first time, when the caller must wait for host_window_on_screen(). The
    keep-above request is repeated on every step: the window manager drops it for a window it has
    not mapped yet, and other windows get raised over ours later."""
    if not HOST_DISPLAY:
        return False
    if _host_window:
        w = _host_window[0]
    else:
        w = _find_main_window()
        if w is None:
            return False
        w.showNormal()
        area = QGuiApplication.primaryScreen().availableGeometry()
        w.setGeometry(area.x(), area.y(), min(HOST_WINDOW_SIZE[0], area.width()), min(HOST_WINDOW_SIZE[1], area.height()))
        w.activateWindow()
    w.raise_()
    above = _ask_window_manager_keep_above(int(w.winId()))
    QApplication.processEvents()
    if _host_window:
        return False
    _host_window.append(w)
    report("NOTE host display: main window pinned, kept above: %s" % above)
    return True


def _find_main_window():
    for w in QApplication.topLevelWidgets():
        if class_name(w) == "Gui" and w.isVisible() and w.windowHandle() is not None and w.windowHandle().isExposed():
            return w
    return None


def host_window_on_screen():
    """Whether a root-window grab shows the main window's panes painted. A compositor can take
    seconds to show a newly mapped window, and the panes can stay unpainted after the menu bar is
    drawn."""
    from PySide6.QtCore import QRect
    from PySide6.QtGui import QImage
    QApplication.processEvents()
    w = _host_window[0]
    area = QRect(w.width() // 20, w.height() // 10, w.width() * 9 // 10, w.height() * 17 // 20)
    own = w.grab(area).toImage().convertToFormat(QImage.Format_RGBX8888)
    origin = w.mapToGlobal(area.topLeft())
    seen = QGuiApplication.primaryScreen().grabWindow(0, origin.x(), origin.y(), area.width(), area.height())
    seen = seen.toImage().convertToFormat(QImage.Format_RGBX8888)
    if own.size() != seen.size():
        return False
    a = bytes(own.constBits())[:own.sizeInBytes()]
    b = bytes(seen.constBits())[:seen.sizeInBytes()]
    samples = range(0, len(a) - 3, 4 * 97)
    if len(set(a[i:i + 3] for i in samples)) < 16:
        return False
    same = sum(1 for i in samples if a[i:i + 3] == b[i:i + 3])
    return same >= 0.9 * len(samples)


class _ClientMessage(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong),
                ("format", ctypes.c_int), ("data", ctypes.c_long * 5), ("pad", ctypes.c_long * 12)]


_x11 = []
_x11_error = []
_x11_display = []


def _load_x11():
    if _x11:
        return _x11[0]
    if _x11_error:
        return None
    try:
        x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
    except OSError as e:
        _x11_error.append("no libX11 (%s)" % e)
        return None
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    _x11.append(x11)
    return x11


def _ask_window_manager_keep_above(window_id):
    """Sends the EWMH _NET_WM_STATE_ABOVE request for window_id. Qt's WindowStaysOnTopHint would
    recreate the native window, and the viewer's GL context with it."""
    x11 = _load_x11()
    if x11 is None:
        return _x11_error[0]
    if not _x11_display:
        dpy = x11.XOpenDisplay(None)
        if not dpy:
            return "cannot open the display"
        _x11_display.append(dpy)
    dpy = _x11_display[0]
    ev = _ClientMessage()
    ev.type = 33
    ev.send_event = 1
    ev.display = dpy
    ev.window = window_id
    ev.message_type = x11.XInternAtom(dpy, b"_NET_WM_STATE", 0)
    ev.format = 32
    ev.data[0] = 1
    ev.data[1] = x11.XInternAtom(dpy, b"_NET_WM_STATE_ABOVE", 0)
    ev.data[3] = 1
    substructure_notify_and_redirect = (1 << 19) | (1 << 20)
    sent = x11.XSendEvent(dpy, x11.XDefaultRootWindow(dpy), 0, substructure_notify_and_redirect, ctypes.byref(ev))
    x11.XFlush(dpy)
    return "requested" if sent else "XSendEvent failed"


def run(gen):
    """Drives gen on the Qt event loop and calls finish() when it ends or raises."""
    window_deadline = time.time() + HOST_ON_SCREEN_TIMEOUT_S

    def advance(value):
        if HOST_DISPLAY and not _host_window and _find_main_window() is None:
            if time.time() > window_deadline:
                check(False, "host display: the main window is exposed within %ds" % HOST_ON_SCREEN_TIMEOUT_S)
            else:
                QTimer.singleShot(200, lambda: advance(value))
                return
        if pin_host_window():
            started = time.time()
            deadline = started + HOST_ON_SCREEN_TIMEOUT_S

            def wait_on_screen():
                if host_window_on_screen():
                    g = _host_window[0].geometry()
                    report("NOTE host display: main window on screen at %d,%d %dx%d after %.1fs" %
                           (g.x(), g.y(), g.width(), g.height(), time.time() - started))
                    advance(value)
                elif time.time() > deadline:
                    check(False, "host display: the main window appears on screen within %ds" % HOST_ON_SCREEN_TIMEOUT_S)
                    advance(value)
                else:
                    QTimer.singleShot(200, wait_on_screen)

            QTimer.singleShot(200, wait_on_screen)
            return
        try:
            cmd = gen.send(value)
        except StopIteration:
            finish()
            return
        except Exception:
            check(False, "script error: " + traceback.format_exc())
            finish()
            return
        if cmd[0] == "sleep":
            QTimer.singleShot(cmd[1], lambda: advance(None))
            return
        predicate, timeout = cmd[1], cmd[2]
        deadline = time.time() + timeout

        def poll():
            try:
                ok = bool(predicate())
            except Exception:
                ok = False
            if ok:
                advance(True)
            elif time.time() > deadline:
                advance(False)
            else:
                QTimer.singleShot(50, poll)

        poll()

    QTimer.singleShot(0, lambda: advance(None))
