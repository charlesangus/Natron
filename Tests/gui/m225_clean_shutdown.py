"""Driver for m225_clean_shutdown.sh: sets up one scenario, then closes Natron one way.

The harness checks what is left once Natron is gone, so this script never exits Natron itself the
way guitest.finish() does: the close path under test has to do it.

Environment:
  CLEAN_SHUTDOWN_SCENARIO  idle | playback | write_fg | write_bg | modal | url | warn_fg | warn_bg
  CLEAN_SHUTDOWN_ACTION    menu (File > Quit) | window (close the main window) | signal (the
                           harness signals the process once this script reports ready)
  CLEAN_SHUTDOWN_OUT       directory for driver.log, the ready marker and rendered frames

Every quit while a render runs asks "N renders are still running. Quitting will stop them."; the
driver answers Quit. The warn_fg and warn_bg scenarios (menu or window action) first answer Cancel
and check that Natron and the render survive, then close again and answer Quit; their ready
marker holds the warning text and what survived the Cancel.

The ready marker holds "ready" plus key=value lines the harness reads, such as render_child=<pid>
when a background render runs in its own process.
"""
import os
import time
import traceback

from PySide6.QtCore import QMetaObject, QTimer
from PySide6.QtGui import QAction
from PySide6.QtWidgets import QApplication, QCheckBox, QDialogButtonBox, QMessageBox, QPushButton

SCENARIO = os.environ.get("CLEAN_SHUTDOWN_SCENARIO", "idle")
ACTION = os.environ.get("CLEAN_SHUTDOWN_ACTION", "menu")
OUT = os.environ["CLEAN_SHUTDOWN_OUT"]
READY = os.path.join(OUT, "ready")
STARTUP_TIMEOUT_S = 60.0
RENDER_START_TIMEOUT_S = 30.0

_log = open(os.path.join(OUT, "driver.log"), "a")
# PySide wrappers of Qt objects die with the Python reference; the timers and actions used later
# have to stay referenced.
state = {"keep": []}


def say(msg):
    _log.write("%.2f %s\n" % (time.time(), msg))
    _log.flush()


def class_name(w):
    return w.metaObject().className().split("::")[-1]


def visible_widgets(name):
    out = []
    for w in QApplication.allWidgets():
        try:
            if class_name(w) == name and w.isVisible():
                out.append(w)
        except RuntimeError:
            pass
    return out


def main_window():
    for w in QApplication.topLevelWidgets():
        try:
            if class_name(w) == "Gui" and w.isVisible():
                return w
        except RuntimeError:
            pass
    return None


def find_action(*texts):
    win = main_window()
    state["keep"].append(win)
    for a in win.findChildren(QAction):
        if a.text().replace("&", "").strip().lower() in texts:
            state["keep"].append(a)
            return a
    return None


def children_of(pid):
    """Direct child processes of pid, from /proc."""
    kids = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % entry) as f:
                fields = f.read().rsplit(")", 1)[1].split()
            if int(fields[1]) == pid:
                with open("/proc/%s/cmdline" % entry, "rb") as f:
                    kids.append((int(entry), f.read().replace(b"\0", b" ").decode(errors="replace")))
        except (OSError, IndexError, ValueError):
            pass
    return kids


def render_child():
    for pid, cmd in children_of(os.getpid()):
        if " -b " in cmd and "--IPCpipe" in cmd:
            return pid
    return None


def build_graph():
    os.makedirs(os.path.join(OUT, "frames"), exist_ok=True)
    constant = app.createNode("net.sf.openfx.ConstantPlugin")
    constant.getParam("color").set(0.2, 0.7, 0.3, 1.0)
    # A slow node keeps every render running when the close arrives.
    blur = app.createNode("net.sf.cimg.CImgBlur")
    blur.connectInput(0, constant)
    blur.getParam("size").set(300, 300)
    write = app.createWriter(os.path.join(OUT, "frames", "f.####.png"))
    write.connectInput(0, blur)
    write.getParam("frameRange").setValue(2)
    write.getParam("firstFrame").setValue(1)
    write.getParam("lastFrame").setValue(400)
    viewer = app.getNode("Viewer1") or app.createNode("fr.inria.built-in.Viewer")
    viewer.connectInput(0, blur)
    state.update(write=write)


def drive_render_dialog(background):
    timer = QTimer()
    timer.setInterval(100)

    def poll():
        dialogs = visible_widgets("RenderDialog")
        if not dialogs:
            return
        timer.stop()
        dialog = dialogs[0]
        for box in dialog.findChildren(QCheckBox):
            if "background" in box.text().lower():
                box.setChecked(background)
        boxes = [b for b in dialog.findChildren(QDialogButtonBox) if b.isVisible()]
        boxes[0].button(QDialogButtonBox.Ok).click()
    timer.timeout.connect(poll)
    timer.start()
    state["render_dialog"] = timer


def start_write(background):
    app.selectNode(state["write"], True)
    QMetaObject.invokeMethod(visible_widgets("NodeGraph")[0], "showSelectedNodeSettingsPanel")
    deadline = time.time() + STARTUP_TIMEOUT_S
    button = None
    while time.time() < deadline and button is None:
        QApplication.processEvents()
        for w in QApplication.allWidgets():
            try:
                if isinstance(w, QPushButton) and w.isVisible() and "<b>startRender</b>" in w.toolTip():
                    button = w
            except RuntimeError:
                pass
    say("render button found=%r" % (button is not None))
    drive_render_dialog(background)
    button.click()


def click_button_named(box, name):
    for b in box.buttons():
        if b.text().replace("&", "").strip().lower() == name:
            b.click()
            return True
    return False


def answer_dialogs():
    modal = QApplication.activeModalWidget()
    if not isinstance(modal, QMessageBox):
        return
    if "still running" in modal.text():
        answer = state.get("renders_answer", "quit")
        state.setdefault("warnings", []).append(modal.text())
        say("render warning %r, answering %s" % (modal.text(), answer))
        click_button_named(modal, answer)
        return
    # A quit with unsaved changes asks whether to save; the scenario discards them.
    discard = modal.button(QMessageBox.Discard)
    if discard is not None:
        say("discard changes")
        discard.click()


def write_ready(extra):
    with open(READY + ".tmp", "w") as f:
        f.write("ready\n")
        for k, v in extra.items():
            f.write("%s=%s\n" % (k, v))
    os.rename(READY + ".tmp", READY)


def start_answerer():
    if "answerer" in state:
        return
    answerer = QTimer()
    answerer.setInterval(200)
    answerer.timeout.connect(answer_dialogs)
    answerer.start()
    state["answerer"] = answerer


def request_close():
    if ACTION == "menu":
        find_action("quit").trigger()
    elif ACTION == "window":
        main_window().close()


def close_now(extra):
    start_answerer()
    say("closing via %s" % ACTION)
    write_ready(extra)
    request_close()
    say("close call returned")


def frame_count():
    return len(os.listdir(os.path.join(OUT, "frames")))


def cancel_then_close(extra):
    """Answers Cancel to the render warning, checks the render went on, then quits for real."""
    start_answerer()
    state["renders_answer"] = "cancel"
    say("closing via %s, to cancel" % ACTION)
    request_close()
    warnings = list(state.get("warnings", []))
    frames_at_cancel = frame_count()
    child = extra.get("render_child")

    def check():
        extra["warning_count"] = len(warnings)
        extra["warning_text"] = warnings[0] if warnings else ""
        extra["survived_window"] = int(main_window() is not None)
        extra["survived_render"] = int(frame_count() > frames_at_cancel
                                       and (child is None or os.path.exists("/proc/%d" % child)))
        say("after cancel %r" % extra)
        state["renders_answer"] = "quit"
        state["warnings"] = []
        close_now(extra)
    QTimer.singleShot(3000, check)


def wait_for_render_then_close():
    """Closes once the render is under way: its process exists or a frame is written."""
    deadline = state.setdefault("render_deadline", time.time() + RENDER_START_TIMEOUT_S)
    child = render_child()
    frames = os.listdir(os.path.join(OUT, "frames"))
    if child is None and not frames and time.time() < deadline:
        QTimer.singleShot(200, wait_for_render_then_close)
        return
    extra = {"frames_before_close": len(frames)}
    if child is not None:
        extra["render_child"] = child
    say("render state %r" % extra)
    # Let the render run on a little so the close interrupts it mid-frame.
    if SCENARIO.startswith("warn_"):
        QTimer.singleShot(1500, lambda: cancel_then_close(extra))
    else:
        QTimer.singleShot(1500, lambda: close_now(extra))


def run_scenario():
    try:
        if SCENARIO == "playback":
            app.getViewer("Viewer1").startForward()
            QTimer.singleShot(3000, lambda: close_now({}))
        elif SCENARIO in ("write_fg", "write_bg", "warn_fg", "warn_bg"):
            start_write(SCENARIO.endswith("_bg"))
            QTimer.singleShot(500, wait_for_render_then_close)
        elif SCENARIO == "url":
            find_action("website").trigger()
            QTimer.singleShot(3000, lambda: close_now({}))
        elif SCENARIO == "modal":
            # The file dialog's exec() is still on the stack when the close arrives.
            QTimer.singleShot(3000, lambda: close_now({}))
            find_action("open project...", "open project").trigger()
        else:
            QTimer.singleShot(1000, lambda: close_now({}))
    except Exception:
        say(traceback.format_exc())


def wait_for_main_window():
    if main_window() is None:
        if time.time() - state.setdefault("t0", time.time()) < STARTUP_TIMEOUT_S:
            QTimer.singleShot(500, wait_for_main_window)
        else:
            say("main window never showed")
        return
    try:
        build_graph()
    except Exception:
        say(traceback.format_exc())
    QTimer.singleShot(1500, run_scenario)


say("start scenario=%s action=%s pid=%d" % (SCENARIO, ACTION, os.getpid()))
QTimer.singleShot(500, wait_for_main_window)
