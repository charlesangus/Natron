"""Checks that a Natron started under software GL reaches an exposed main window.

A Debug build arms the floating-point traps for the whole process, and a software rasteriser such
as Mesa's llvmpipe raises FE_INVALID while it compiles its shaders for the first GL context, so a
build that does not disarm them around the driver dies of SIGFPE before this script ever runs.
Reaching this file at all is therefore most of the check; the script then waits for the main window
to be exposed and for the node graph to exist.

Usage: Tests/gui/run-gui-test.sh Tests/gui/debug_startup.py build/debug/App/Natron
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0

try:
    import guitest as gt
    from guitest import check, require

    def main_window():
        return gt._find_main_window()

    def steps():
        gt.section("startup")
        exposed = yield ("until", lambda: main_window() is not None, STARTUP_TIMEOUT_S)
        require(exposed, "the main window is exposed within %ds" % int(STARTUP_TIMEOUT_S))
        check(len(gt.widgets_of("NodeGraph")) > 0, "the node graph exists")
        yield ("sleep", 1000)

    gt.run(steps())
except Exception:
    import traceback
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", os.getcwd()), "results.txt"), "a") as results:
        results.write("FAIL script error: " + traceback.format_exc() + "\n")
    os._exit(1)
