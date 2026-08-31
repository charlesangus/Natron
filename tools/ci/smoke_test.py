# ***** BEGIN LICENSE BLOCK *****
# This file is part of Natron <https://natrongithub.github.io/>,
# (C) 2018-2023 The Natron developers
#
# Natron is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# Natron is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
# ***** END LICENSE BLOCK *****
#
# CI smoke test for the NatronEngine Python bindings (see M2.P3.T1a).
#
# This is *not* a script to be run under a system/standalone Python
# interpreter: NatronEngine/NatronGui are built as static libraries baked
# directly into the Natron/NatronRenderer executables (see
# Engine/CMakeLists.txt, Gui/CMakeLists.txt) -- there is no importable
# NatronEngine .so/.pyd to `import` from the outside. Instead this file is
# meant to be passed as the background script argument to NatronRenderer:
#
#     NatronRenderer /path/to/tools/ci/smoke_test.py
#
# NatronRenderer runs in background mode implicitly (Engine/CLArgs.cpp), so
# no -b/--background flag is required, and -t/--interpreter must NOT be
# used here: -t (with or without a script) drops into an interactive
# stdin prompt after sourcing the script and never returns, which would
# hang CI indefinitely.
#
# Deliberately, this script does NOT define a top-level
#     def createInstance(app, group):
# function. If it did, Natron would `import` it as a module instead of
# executing it directly (see AppInstance::loadPythonScript() in
# Engine/AppInstance.cpp), and an imported module does not have access to
# the "app" variable that Natron pre-declares for directly-executed
# scripts (see Documentation/source/devel/natronexecution.rst). This
# script relies on that pre-declared `app` variable below.
#
# It exercises two aspects of the PySide6/Shiboken6 binding surface
# implicated in https://github.com/NatronGitHub/Natron/issues/854 (see
# PLAN/MILESTONES/M2-qt6-migration.md, task M2.P3.T1):
#
#   1. The Qt binding a user script actually gets from inside Natron's
#      embedded interpreter is a working PySide6 -- importable, and with
#      enum/QFlags semantics that behave like the ints the surrounding
#      code expects. That is the precise shape of #854 ("Qt.AlignmentFlag
#      object cannot be interpreted as an integer"): user scripts were
#      handed a different Qt binding from the one NatronEngine/NatronGui
#      are compiled against. Natron used to route this through a
#      third-party Qt-binding abstraction layer (PyQt4/PyQt5/PySide/
#      PySide2/PySide6), steered by an environment variable that had to be
#      exported before Py_Initialize(); this fork targets exactly one Qt
#      (6.8) and one Python (3.13), so both the shim and the env var are
#      gone and AppManager::loadPythonGroups() now imports PySide6
#      directly (see M2.P3.T1g in PLAN/).
#
#      Be honest about what check_pyside6_bindings() is therefore worth.
#      With no binding-selection layer left, #854's mechanism is gone by
#      construction, and every property that check asserts is a property
#      of the PySide6 wheel the container ships rather than of anything in
#      this repo. Its `import PySide6` cannot fail here either:
#      NatronEngine's generated module init already calls
#      Shiboken::Module::import("PySide6.QtCore") (see
#      build/*/Engine/Qt6/NatronEngine/natronengine_module_wrapper.cpp),
#      and AppManager::initPython() throws if importing NatronEngine
#      fails, so Natron aborts long before this script runs. Treat it as a
#      cheap environment tripwire, not a regression guard. The coverage
#      that does bite lives elsewhere in this file:
#      check_app_render_with_task_list() drives NatronEngine's PySide6
#      QString converters on every string-taking call, and the one
#      QFlags-taking bound API (PyGuiApplication::addMenuCommand) stays
#      uncovered, for the GUI-only reason spelled out in main().
#   2. The <replace-type modified-type="PySequence"/> on App.render()'s
#      (and GuiApp.renderBlocking()'s) first argument in
#      Engine/typesystem_engine.xml / Gui/typesystem_natronGui.xml, which
#      is what makes `app.render([(writeNode, first, last)])` actually
#      accept a plain Python list of tuples. Without it, Shiboken emits a
#      std::list<Effect*> element-type check for that overload and rejects
#      the list of tuples with a TypeError at the call site.
#
# NOTE on how a Python-level failure here becomes a non-zero *process*
# exit code (this matters, so a future editor doesn't "simplify" it away):
# Since this script has no createInstance(app, group) function, Natron
# executes it with CPython's PyRun_SimpleString(), and neither
# PyRun_SimpleString()'s return value nor loadPythonScript()'s return
# value is checked anywhere in Natron's caller chain in background-autorun
# mode. A plain uncaught exception (e.g. AssertionError) would just be
# printed by Natron's own PyErr_Print() call and swallowed -- the process
# would still exit 0. The one exception CPython treats specially is
# SystemExit: PyErr_Print() -> PyErr_PrintEx() calls handle_system_exit(),
# which calls Py_Exit(code), which calls the C library exit(code)
# immediately, terminating the whole process right there with that exit
# code. So every failure path below must end in sys.exit(1) (raised via
# SystemExit), not just an uncaught exception.
from __future__ import print_function

import os
import struct
import sys
import tempfile
import traceback
import zlib


def _mark(msg):
    # Print with an eager, unbuffered flush. Natron does not redirect
    # sys.stdout/sys.stderr to a StreamCatcher in background mode (see
    # AppManager::initPython()'s `if (!isBackground())` guard), so this is
    # plain C stdio; when NatronRenderer's output is piped to a file rather
    # than a tty, CPython block-buffers it, and any unflushed output would
    # be lost if the process were ever killed by a signal (e.g. a native
    # crash unrelated to this script, elsewhere in Natron) instead of
    # exiting normally. Flushing after every step keeps this script's
    # progress visible in CI logs even in that case.
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


def _write_solid_png(path, width, height, rgb):
    """Write a tiny, dependency-free solid-color PNG file.

    Used as the smoke test's input image so this script does not depend on
    any downloaded test assets or a generator OpenFX plug-in (only
    openfx-io's ReadOIIO/WriteOIIO are available in CI, see
    ".github/workflows/ci.yml"'s "Download Plugins" step; the generator
    plug-ins from openfx-misc, e.g. CheckerBoard/ColorBars/Constant, are
    not).
    """
    def chunk(tag, data):
        c = tag + data
        return struct.pack("!I", len(data)) + c + struct.pack("!I", zlib.crc32(c) & 0xffffffff)

    row = bytes(bytearray(rgb)) * width
    raw = b"".join(b"\x00" + row for _ in range(height))
    ihdr = struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit depth, RGB color type
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def check_pyside6_bindings():
    """Assert the embedded interpreter sees a working PySide6.

    An environment tripwire, not a regression guard -- see item 1 of the
    module docstring for why nothing in this repo can make it fail. It is
    deliberately more than `import PySide6` (it converts an enum to an
    int, round-trips a QFlags value, and instantiates a real QObject, so
    the C++/shiboken6 half is exercised and not just the Python package's
    presence on sys.path), but note that under the PySide6 6.8 that
    the container ships, Qt.AlignmentFlag is an enum.IntFlag -- an int
    subclass -- so those conversions cannot realistically fail against any
    released PySide6 either.
    """
    _mark("[smoke] importing PySide6...")
    import PySide6
    import shiboken6
    from PySide6 import QtCore
    _mark("[smoke] PySide6 %s imported from %r"
          % (PySide6.__version__, PySide6.__file__))

    # A major-version check, not an exact pin: this fork builds against
    # exactly one Qt (6.8), and anything that is not PySide6 here means the
    # bindings the user script sees are not the ones NatronEngine/NatronGui
    # were generated against.
    if not PySide6.__version__.startswith("6."):
        raise AssertionError(
            "PySide6.__version__ is %r, expected a 6.x -- the Qt bindings "
            "visible to user scripts do not match the PySide6/Qt6 that "
            "NatronEngine/NatronGui are bound against."
            % (PySide6.__version__,)
        )

    # Enum -> int. This is literally the #854 symptom: under a mismatched
    # binding, Qt.AlignmentFlag members are not int-like and int() (or any
    # implicit integer coercion in user code) raises
    # "TypeError: 'Qt.AlignmentFlag' object cannot be interpreted as an
    # integer".
    align_left = QtCore.Qt.AlignmentFlag.AlignLeft
    align_top = QtCore.Qt.AlignmentFlag.AlignTop
    try:
        left_int = int(align_left)
        top_int = int(align_top)
    except TypeError as e:
        raise AssertionError(
            "int(QtCore.Qt.AlignmentFlag.AlignLeft) raised a TypeError -- "
            "this is exactly the enum/QFlags regression from "
            "NatronGitHub/Natron#854 that this smoke test guards "
            "against: %s" % (e,)
        )
    _mark("[smoke] int(Qt.AlignmentFlag.AlignLeft)=%r int(AlignTop)=%r"
          % (left_int, top_int))

    # QFlags round trip: OR two flags together, and check both the combined
    # integer value and membership testing. A binding whose QFlags support
    # is broken typically fails one or the other rather than the import.
    flags = align_left | align_top
    if int(flags) != (left_int | top_int):
        raise AssertionError(
            "int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop) is "
            "%r, expected %r -- PySide6's QFlags arithmetic is not behaving "
            "as an integer flag set." % (int(flags), left_int | top_int)
        )
    if not (flags & align_left) or (flags & QtCore.Qt.AlignmentFlag.AlignRight):
        raise AssertionError(
            "QFlags membership test failed on %r: expected AlignLeft set "
            "and AlignRight unset." % (flags,)
        )
    _mark("[smoke] OK: QFlags round trip, int(AlignLeft|AlignTop)=%r"
          % (int(flags),))

    # Finally, prove the native half of the bindings actually works: create
    # a real QObject (a C++ instance behind a Python wrapper), round-trip a
    # property through it, and ask shiboken6 whether the wrapper still owns
    # a valid C++ object. An importable-but-broken PySide6 (mismatched Qt
    # libs, half-installed package) gets this far and then fails here.
    obj = QtCore.QObject()
    obj.setObjectName("natron-ci-smoke")
    if not shiboken6.isValid(obj):
        raise AssertionError(
            "shiboken6.isValid(QtCore.QObject()) is False -- the PySide6 "
            "Python package imported but its underlying C++ object could "
            "not be created."
        )
    if obj.objectName() != "natron-ci-smoke":
        raise AssertionError(
            "QObject.objectName() round trip returned %r, expected "
            "'natron-ci-smoke'." % (obj.objectName(),)
        )
    _mark("[smoke] OK: PySide6/shiboken6 %s bindings are live "
          "(QObject created and valid)" % (shiboken6.__version__,))


def check_app_render_with_task_list():
    tmpdir = tempfile.mkdtemp(prefix="natron-ci-smoke-")
    in_path = os.path.join(tmpdir, "in.png")
    out_path = os.path.join(tmpdir, "out.png")
    _write_solid_png(in_path, 4, 4, (220, 20, 60))
    _mark("[smoke] wrote input PNG at %r" % (in_path,))

    reader = app.createReader(in_path)  # app: pre-declared by Natron, see module docstring above
    _mark("[smoke] app.createReader() returned %r" % (reader,))
    if reader is None:
        raise AssertionError("app.createReader(%r) returned None" % (in_path,))

    writer = app.createWriter(out_path)
    _mark("[smoke] app.createWriter() returned %r" % (writer,))
    if writer is None:
        raise AssertionError("app.createWriter(%r) returned None" % (out_path,))

    connect_ok = writer.connectInput(0, reader)
    _mark("[smoke] writer.connectInput(0, reader) returned %r" % (connect_ok,))
    if not connect_ok:
        raise AssertionError("Effect.connectInput(0, reader) failed")

    # This is the call that exercises the PySequence replace-type fix: a
    # plain Python list of (Effect, int, int) tuples must be accepted by
    # the render(tasks) overload. Before the fix, argument 1 kept its
    # declared std::list<Effect*> C++ type, so Shiboken's generated
    # overload-dispatch code checked that every list element converts to
    # an Effect* and rejected our list of tuples with a TypeError.
    _mark("[smoke] calling app.render([(writer, 1, 1)])...")
    try:
        app.render([(writer, 1, 1)])
    except TypeError as e:
        raise AssertionError(
            "app.render([(writer, 1, 1)]) raised a TypeError -- this is "
            "exactly the App.render() PySequence/replace-type regression "
            "this smoke test guards against (see "
            "Engine/typesystem_engine.xml's render() modify-function): %s" % (e,)
        )
    _mark("[smoke] app.render([(writer, 1, 1)]) returned")

    if not os.path.exists(out_path) or os.path.getsize(out_path) == 0:
        raise AssertionError(
            "app.render([(writer, 1, 1)]) did not raise, but no non-empty "
            "output file was produced at %r" % (out_path,)
        )
    _mark("[smoke] OK: app.render([(writer, 1, 1)]) rendered %r" % (out_path,))


def main():
    _mark("[smoke] script started")

    global app
    try:
        app
    except NameError:
        _mark("[smoke] WARNING: 'app' was not pre-declared by Natron; "
              "obtaining via NatronEngine.natron.getInstance(0)")
        import NatronEngine
        app = NatronEngine.natron.getInstance(0)

    check_pyside6_bindings()
    check_app_render_with_task_list()

    # NOTE: not covered here -- PyGuiApplication::addMenuCommand()
    # (Gui/PyGlobalGui.h, bound in Gui/typesystem_natronGui.xml), which
    # takes a Qt::KeyboardModifiers QFlags argument, is the one other
    # QFlags-taking API on the bound surface and would exercise a second,
    # independent instance of the enum/QFlags class of bug from issue
    # #854. It is intentionally not exercised by this script: the `natron`
    # variable it hangs off is only bound to a PyGuiApplication (as
    # opposed to a background-mode PyCoreApplication) when
    # AppManager::isBackground() is false (see the `if (!isBackground())`
    # branch in AppManager::initPython(), Engine/AppManager.cpp), which
    # rules out NatronRenderer entirely (always background) as well as
    # `Natron -b`/`Natron -t` (both force background mode too, see
    # Engine/CLArgs.cpp). The only way to reach it is `Natron
    # <script>.py` with no -b/-t flag, which runs this script from inside
    # GuiApplicationManager::initGui() *after* qApp->exec() has already
    # been entered (a splash screen is shown and an async fontconfig
    # cache build is kicked off first) rather than synchronously before
    # any event loop starts, unlike every background-mode entry point
    # above. That is meaningfully more involved to make reliable in CI
    # than the two checks above, so it is left as a known gap rather than
    # added speculatively -- see M2.P3.T1a's report for details.


# Natron sources this file directly with PyRun_SimpleString() (see module
# docstring above) -- it is never imported as a module -- so just run
# unconditionally rather than gating on `if __name__ == "__main__"`.
try:
    main()
except Exception:
    traceback.print_exc()
    sys.stderr.flush()
    _mark("\n[smoke] SMOKE TEST FAILED")
    sys.exit(1)
else:
    _mark("\n[smoke] SMOKE TEST PASSED")
    sys.exit(0)
