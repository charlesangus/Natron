"""Run by Tests/gui/run-gl-refusal-test.sh, which forces an OpenGL context the GUI must refuse.
Natron should exit before any startup script runs, so reaching this file is the failure."""
import os

ROOT = os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd())
OUT = os.environ.get("NATRON_GUI_TEST_OUT", os.path.join(ROOT, "build", "gui-test-out"))
if not os.path.isdir(OUT):
    os.makedirs(OUT)
with open(os.path.join(OUT, "results.txt"), "w") as results:
    results.write("FAIL the GUI started on a context it should have refused\n")
    results.write("SUMMARY 1 failure(s)\n")
os._exit(1)
