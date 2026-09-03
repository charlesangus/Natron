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
# Deliberately, this script does NOT define a top-level createInstance()
# function -- and, just as deliberately, does not spell that name with a
# preceding "def " anywhere in its text, not even inside a comment.
# AppInstance::loadPythonScript() (Engine/AppInstance.cpp) chooses how to
# run this file with a plain content.contains() search for the text "def"
# followed by "createInstance", over the whole file, comments included, so
# a passing mention is indistinguishable from a real definition. On a
# match, Natron
# `import`s the file as a module instead of executing it directly, and an
# imported module does not have access to the "app" variable that Natron
# pre-declares for directly-executed scripts (see
# Documentation/source/devel/natronexecution.rst). This script relies on
# that pre-declared `app` variable below, and -- see the NOTE further down
# -- the two branches also differ in whether a SystemExit from this file
# can reach the process exit status at all.
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
# raising is not enough, and neither is sys.exit(). Neither
# PyRun_SimpleString()'s return value nor loadPythonScript()'s return
# value is checked anywhere in Natron's caller chain in background-autorun
# mode, so a plain uncaught exception (e.g. AssertionError) is printed and
# swallowed and the process still exits 0. SystemExit is the one exception
# CPython treats specially -- PyErr_Print() -> PyErr_PrintEx() calls
# handle_system_exit(), which calls Py_Exit(code) -- but that chain is
# only reached on the direct-execution branch described above. On the
# module-import branch, Natron runs `import <module>` through
# NATRON_PYTHON_NAMESPACE::interpretPythonScript() (Engine/AppManager.cpp),
# which calls PyRun_String() and then PyErr_Fetch()es the pending
# exception in order to format it into a std::string -- and PyErr_Fetch()
# clears the error indicator. PyErr_Print() never runs, the SystemExit is
# reported as an ordinary printed traceback, and Natron carries on to
# render the project's write nodes, so the process exit status ends up
# describing that unrelated render rather than this script's verdict.
# That is not a hypothetical: this file used to spell that definition out
# in the comment above, took the import branch because of it, printed
# "SMOKE TEST FAILED", and exited 0.
#
# Keeping the substring out of this file restores the direct branch and
# with it sys.exit(), but that would leave CI's only pass/fail signal at
# the mercy of a comment. So both exits below go through _exit() ->
# os._exit() instead, which sets the process exit status unconditionally
# and identically on either branch.
from __future__ import print_function

import os
import struct
import subprocess
import sys
import tempfile
import traceback
import zlib

# Scene-linear 0.18 -- the standard 18% mid-grey -- and the 8-bit code
# value it lands on once encoded into an sRGB PNG. SRGB_GREY_CODE is
# measured through the plug-in bundle under test, not derived from the
# sRGB formula: it is what the reader's decode and the writer's encode
# actually produce end to end.
SCENE_LINEAR_GREY = 0.18
SRGB_GREY_CODE = 118
# Loose enough to absorb a rounding wobble in either transform, far too
# tight for the failure this exists to catch: drop the sRGB encode (or
# substitute a scene-linear role for a colorspace that failed to resolve)
# and the same pixel lands on 46, a 2.5x error.
SRGB_GREY_TOLERANCE = 2

# The OCIO config Natron picks for itself when nothing in the environment
# picks one for it (Engine/Settings.cpp's NATRON_DEFAULT_OCIO_CONFIG_NAME).
# Natron exports it as an "ocio://" URI; OpenColorIO reports the config it
# resolved to under the same name minus the scheme.
DEFAULT_OCIO_CONFIG = "studio-config-v4.0.0_aces-v2.0_ocio-v2.5"


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


def _write_solid_exr(path, width, height, value):
    """Write a solid scene-linear half-float EXR, via oiiotool.

    Generated at test time rather than committed next to this script: a
    binary fixture would have to be taken on trust, whereas this states
    the pixel value the assertion is about in the same file as the
    assertion. oiiotool ships in the CI container alongside the openfx-io
    plug-ins under test, so its absence is an environment failure and is
    reported as one -- this case must never quietly skip itself, since a
    skipped colour check looks exactly like a passing one.
    """
    size = "%dx%d" % (width, height)
    cmd = ["oiiotool",
           "--create", size, "3",
           "--fill:color=%r,%r,%r" % (value, value, value), size,
           "-d", "half",
           "-o", path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0 or not os.path.exists(path):
        raise AssertionError(
            "%s exited %d and did not produce a usable EXR at %r: %s"
            % (" ".join(cmd), proc.returncode, path,
               proc.stdout.decode("utf-8", "replace").strip())
        )


EXPECTED_OFX_BUNDLES = ("Arena", "CImg", "IO", "Misc")


def check_ofx_plugin_bundle_set():
    """Assert OFX_PLUGIN_PATH holds exactly the bundle set Natron ships.

    Not a plugin-ID check (Tests/BaseTest.cpp already asserts specific IDs
    at the C++ level) -- this exists to catch a *skipped*
    tools/ci/local/fetch-assets.sh run. CI caches build/assets/ keyed on
    that script's hash, so a build where the cache step was silently
    skipped (or restored partially) still has OFX_PLUGIN_PATH pointing at
    *something*: every check that doesn't happen to need the missing
    bundle passes normally, and the gap stays invisible until a user hits
    the missing plug-in.
    """
    plugin_path = os.environ.get("OFX_PLUGIN_PATH")
    if not plugin_path or not os.path.isdir(plugin_path):
        raise AssertionError(
            "OFX_PLUGIN_PATH (%r) is not set to a directory -- "
            "tools/ci/local/fetch-assets.sh has not run." % (plugin_path,)
        )

    found = set()
    for entry in os.listdir(plugin_path):
        if entry.endswith(".ofx.bundle") and os.path.isdir(os.path.join(plugin_path, entry)):
            found.add(entry[: -len(".ofx.bundle")])

    expected = set(EXPECTED_OFX_BUNDLES)
    if found != expected:
        raise AssertionError(
            "OFX_PLUGIN_PATH=%r holds bundle set %r, expected exactly %r "
            "(missing=%r extra=%r). A missing bundle here means "
            "fetch-assets.sh did not run, or did not complete, before this "
            "test did." % (plugin_path, sorted(found), sorted(expected),
                            sorted(expected - found), sorted(found - expected))
        )
    _mark("[smoke] OK: OFX_PLUGIN_PATH bundle set is %r" % (sorted(found),))

    # Reuse the dlopen + OfxGetNumberOfPlugins/OfxGetPlugin probe
    # fetch-assets.sh already builds and runs against these same bundles
    # (tools/ci/verify_plugin_loads.cpp), rather than re-implementing a
    # second loader here: a directory that merely exists is not the same
    # guarantee as a bundle that actually dlopen()s.
    assets_dir = os.path.dirname(os.path.normpath(plugin_path))
    verify_loader = os.path.join(assets_dir, "plugin-src", "verify_plugin_loads")
    if not os.path.isfile(verify_loader):
        raise AssertionError(
            "verify_plugin_loads probe not found at %r -- fetch-assets.sh "
            "builds it, so its absence means that step didn't complete "
            "either." % (verify_loader,)
        )

    for name in EXPECTED_OFX_BUNDLES:
        ofx_bin = os.path.join(plugin_path, "%s.ofx.bundle" % name,
                                "Contents", "Linux-x86-64", "%s.ofx" % name)
        proc = subprocess.run([verify_loader, ofx_bin],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # IO.ofx's dlopen is informational-only in fetch-assets.sh too (see
        # the comment above its own probe call): it depends on the
        # container having already indexed OCIO/OIIO/OpenEXR's SONAMEs via
        # ldconfig, which is a property of the environment, not the bundle.
        if name == "IO":
            continue
        if proc.returncode != 0:
            raise AssertionError(
                "verify_plugin_loads %r failed (exit %d): %s"
                % (ofx_bin, proc.returncode,
                   proc.stdout.decode("utf-8", "replace").strip())
            )
    _mark("[smoke] OK: verify_plugin_loads probed %r" % (EXPECTED_OFX_BUNDLES,))


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


def check_default_ocio_config():
    """Check the colour checks below run on Natron's own default OCIO config.

    tools/ci/local/test.sh leaves OCIO unset so that Settings.cpp's default
    resolution is what gets exercised. Nothing else would notice if that
    resolution broke: with OCIO unset, OpenColorIO falls back to a built-in
    config of its own that encodes sRGB just as correctly, so the code value
    asserted below would still come out right.

    os.environ cannot answer this. CPython snapshots the environment when the
    interpreter starts, which is before Natron's qputenv(), so os.environ still
    reports OCIO as unset here. CreateFromEnv() reads it through getenv(), the
    same way the plug-ins do.
    """
    import PyOpenColorIO as ocio

    name = ocio.Config.CreateFromEnv().getName()
    _mark("[smoke] active OCIO config: %r" % (name,))
    if name != DEFAULT_OCIO_CONFIG:
        raise AssertionError(
            "the active OpenColorIO config is %r, expected %r. Either "
            "Settings::tryLoadOpenColorIOConfig() no longer resolves Natron's "
            "default config, or something in the environment overrode it -- "
            "in both cases the colour check below is no longer testing what "
            "ships." % (name, DEFAULT_OCIO_CONFIG)
        )


def check_exr_to_png_colorspace():
    """Render scene-linear EXR -> 8-bit sRGB PNG and check the code value.

    The PNG -> PNG render above cannot see a colour bug at all: reader and
    writer sit on opposite ends of the same transform there, so getting
    that transform wrong cancels out and the output is byte-identical to
    the input either way. Crossing formats breaks the symmetry -- the
    reader is asked to decode a linear EXR and the writer to encode an
    8-bit sRGB PNG, so only one of the two can be silently wrong -- which
    is what makes the single code value asserted here meaningful. This is
    the check that would have caught substituting OCIO's scene_linear role
    for every colorspace that failed to resolve: the render still
    succeeds, every image just comes out 2.5x too dark.
    """
    from PySide6 import QtGui

    tmpdir = tempfile.mkdtemp(prefix="natron-ci-smoke-exr-")
    in_path = os.path.join(tmpdir, "linear.exr")
    out_path = os.path.join(tmpdir, "encoded.png")
    _write_solid_exr(in_path, 16, 16, SCENE_LINEAR_GREY)
    _mark("[smoke] wrote %r scene-linear input EXR at %r"
          % (SCENE_LINEAR_GREY, in_path))

    reader = app.createReader(in_path)
    if reader is None:
        raise AssertionError("app.createReader(%r) returned None" % (in_path,))
    writer = app.createWriter(out_path)
    if writer is None:
        raise AssertionError("app.createWriter(%r) returned None" % (out_path,))
    if not writer.connectInput(0, reader):
        raise AssertionError(
            "Effect.connectInput(0, reader) failed for the EXR -> PNG case")

    _mark("[smoke] calling app.render([(writer, 1, 1)]) for EXR -> PNG...")
    app.render([(writer, 1, 1)])

    img = QtGui.QImage(out_path)
    if img.isNull():
        raise AssertionError(
            "app.render() produced no decodable PNG at %r" % (out_path,))
    code = img.pixelColor(img.width() // 2, img.height() // 2).red()
    _mark("[smoke] scene-linear %r -> PNG code value %d (expected %d +/- %d)"
          % (SCENE_LINEAR_GREY, code, SRGB_GREY_CODE, SRGB_GREY_TOLERANCE))
    if abs(code - SRGB_GREY_CODE) > SRGB_GREY_TOLERANCE:
        raise AssertionError(
            "scene-linear %r rendered from EXR to 8-bit PNG landed on code "
            "value %d, expected %d +/- %d. The colour pipeline is not doing "
            "what it should: %d is what an unencoded linear value quantised "
            "straight into an 8-bit container looks like, and is the "
            "signature of a colorspace that silently resolved to a linear "
            "role instead of to a display encoding."
            % (SCENE_LINEAR_GREY, code, SRGB_GREY_CODE, SRGB_GREY_TOLERANCE,
               round(SCENE_LINEAR_GREY * 255))
        )
    _mark("[smoke] OK: EXR -> PNG colour transform intact, rendered %r"
          % (out_path,))


READ_TIME_OFFSET_FIXTURE_OUTPUT_TOKEN = "TIME_OFFSET_FIXTURE_OUTPUT_DIR"


def _repo_root():
    """Locate the repo root the same way check_ofx_plugin_bundle_set() locates
    verify_plugin_loads: OFX_PLUGIN_PATH is always <repo>/build/assets/Plugins
    under tools/ci/local/test.sh, which is the only launcher for this script
    and always sets it -- there is no __file__ to fall back on, since Natron
    runs this script's text through PyRun_SimpleString() rather than
    executing it as a file (see the module docstring).
    """
    plugin_path = os.environ.get("OFX_PLUGIN_PATH")
    if not plugin_path:
        raise AssertionError(
            "OFX_PLUGIN_PATH is not set -- cannot locate the repository "
            "root to find Tests/fixtures/read-time-offset.ntp."
        )
    assets_dir = os.path.dirname(os.path.normpath(plugin_path))
    build_dir = os.path.dirname(assets_dir)
    return os.path.dirname(build_dir)


def _write_time_offset_fixture_copy(dir_path, output_dir):
    fixture_path = os.path.join(_repo_root(), "Tests", "fixtures",
                                 "read-time-offset.ntp")
    with open(fixture_path, "r") as f:
        content = f.read()

    # Read1's timeOffset knob in this fixture is 50, not 0. A first attempt
    # at this check used the default offset of 0 and passed green against
    # the broken -i code path -- i.e. it tested nothing, since with
    # timeOffset==0 there is nothing stale for GenericReaderPlugin's
    # sequenceTime = t - timeOffset to get wrong. Do not "simplify" this
    # fixture to a zero offset.
    content = content.replace(READ_TIME_OFFSET_FIXTURE_OUTPUT_TOKEN, output_dir)

    copy_path = os.path.join(dir_path, "read-time-offset.ntp")
    with open(copy_path, "w") as f:
        f.write(content)
    return copy_path


def check_reader_cli_time_offset_regression():
    """Assert `NatronRenderer -i <read> <file>` keeps frames 1-3 distinct.

    Engine/AppInstance.cpp applies -i's filename with a bare
    KnobFile::setValue(): the reader's own filename handler refreshes
    originalFrameRange/firstFrame/lastFrame/startingTime from the new file,
    but never timeOffset. Tests/fixtures/read-time-offset.ntp's Read1 node
    carries a non-zero timeOffset for exactly this reason (see
    _write_time_offset_fixture_copy() above): with the stale offset, every
    requested time maps outside the sequence domain and the reader's
    before/after hold collapses frames 1-3 onto one held input frame, so
    they render byte-identical despite the 3 input frames differing.

    This drives a *second*, freshly spawned NatronRenderer as a subprocess
    rather than exercising -i in-process: the defect is specifically on
    AppInstance::loadInternal()'s CLI path, which is one-shot per
    AppInstance, and this script already runs inside one such instance.
    """
    natron_renderer = os.readlink("/proc/self/exe")

    tmpdir = tempfile.mkdtemp(prefix="natron-ci-smoke-timeoffset-")
    in_dir = os.path.join(tmpdir, "in")
    out_dir = os.path.join(tmpdir, "out")
    os.mkdir(in_dir)
    os.mkdir(out_dir)

    for i in (1, 2, 3):
        _write_solid_exr(os.path.join(in_dir, "input.%04d.exr" % i), 4, 4, 0.1 * i)
    _mark("[smoke] wrote 3 distinct input frames at %r" % (in_dir,))

    project_path = _write_time_offset_fixture_copy(tmpdir, out_dir)
    read_pattern = os.path.join(in_dir, "input.####.exr")
    cmd = [natron_renderer, "-i", "Read1", read_pattern, project_path, "1-3"]
    _mark("[smoke] running %r" % (cmd,))
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        raise AssertionError(
            "%s exited %d:\n%s" % (" ".join(cmd), proc.returncode, output)
        )

    frame_paths = [os.path.join(out_dir, "out.%04d.exr" % i) for i in (1, 2, 3)]
    contents = []
    for p in frame_paths:
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            raise AssertionError(
                "-i Read1 <file> did not produce a non-empty output frame "
                "at %r:\n%s" % (p, output)
            )
        with open(p, "rb") as f:
            contents.append(f.read())

    matches = [(frame_paths[i], frame_paths[j])
               for i in range(len(contents))
               for j in range(i + 1, len(contents))
               if contents[i] == contents[j]]
    if matches:
        raise AssertionError(
            "rendering frames 1-3 through `%s` produced byte-identical "
            "output for %r even though the 3 input frames at %r differ. "
            "Read1's timeOffset (50, non-zero by design -- see "
            "Tests/fixtures/read-time-offset.ntp) was not refreshed when -i "
            "set a new filename, so every requested frame time maps "
            "outside the sequence domain and the reader's before/after "
            "hold collapses frames 1-3 onto a single held input frame."
            % (" ".join(cmd), matches, in_dir)
        )
    _mark("[smoke] OK: -i Read1 <file> rendered 3 distinct frames %r "
          "despite a stale non-zero timeOffset" % (frame_paths,))


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

    check_ofx_plugin_bundle_set()
    check_pyside6_bindings()
    check_app_render_with_task_list()
    check_default_ocio_config()
    check_exr_to_png_colorspace()
    check_reader_cli_time_offset_regression()

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


def _exit(code):
    """End the process with `code`, immediately and unconditionally.

    See the NOTE in the module docstring for why sys.exit() cannot be
    trusted to do this from inside Natron's embedded interpreter.
    os._exit() does not raise anything for Natron to catch, format and
    discard: it goes straight to the C library's _exit(), so the status it
    is handed is the status CI sees, and nothing Natron would otherwise go
    on to do -- notably rendering the write nodes this script left in the
    project -- gets a chance to overwrite the verdict. Both streams are
    flushed first, since _exit() does not flush.
    """
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)


# Natron sources this file directly with PyRun_SimpleString() (see module
# docstring above) -- it is never imported as a module -- so just run
# unconditionally rather than gating on `if __name__ == "__main__"`.
#
# BaseException rather than Exception: anything at all that escapes main()
# has to end up red, including a SystemExit raised out of code this script
# calls, which under `except Exception` would sail past this handler and
# be swallowed by Natron exactly as described in the NOTE.
try:
    main()
except BaseException:
    traceback.print_exc()
    sys.stderr.flush()
    _mark("\n[smoke] SMOKE TEST FAILED")
    _exit(1)
else:
    _mark("\n[smoke] SMOKE TEST PASSED")
    _exit(0)
