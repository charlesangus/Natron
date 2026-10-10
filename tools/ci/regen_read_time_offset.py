"""Regenerate Tests/fixtures/read-time-offset.ntp.

Run through NatronRenderer, which supplies the `app` global (the same way
tools/ci/smoke_test.py is run), with the output path in the environment
because a script run this way has no __file__ or argv to take it from:

    READ_TIME_OFFSET_FIXTURE_PATH=$PWD/Tests/fixtures/read-time-offset.ntp \
    OFX_PLUGIN_PATH=$PWD/build/assets/Plugins \
    xvfb-run --auto-servernum NatronRenderer tools/ci/regen_read_time_offset.py

The fixture is a native Read (timeOffset 50, in time-offset frame mode, the
only mode that honours it) feeding a Write whose output directory is the
placeholder smoke_test.py substitutes before rendering.
"""
import os

# Must match READ_TIME_OFFSET_FIXTURE_OUTPUT_TOKEN in tools/ci/smoke_test.py.
OUTPUT_DIR_TOKEN = "TIME_OFFSET_FIXTURE_OUTPUT_DIR"
TIME_OFFSET = 50
FRAME_MODE_TIME_OFFSET = 1


def _require(value, what):
    if value is None:
        raise AssertionError("could not create or find %s" % what)
    return value


def main():
    out_path = os.environ.get("READ_TIME_OFFSET_FIXTURE_PATH")
    if not out_path:
        raise AssertionError("READ_TIME_OFFSET_FIXTURE_PATH is not set")

    reader = _require(app.createNode("fr.inria.built-in.Read"), "the native Read")
    if reader.getScriptName() != "Read1":
        raise AssertionError("expected the Read to be named Read1, got %r"
                             % (reader.getScriptName(),))
    _require(reader.getParam("frameMode"), "Read.frameMode").set(FRAME_MODE_TIME_OFFSET)
    _require(reader.getParam("timeOffset"), "Read.timeOffset").set(TIME_OFFSET)

    writer = _require(app.createNode("fr.inria.openfx.WriteOIIO"), "WriteOIIO")
    if writer.getScriptName() != "Write1":
        raise AssertionError("expected the Write to be named Write1, got %r"
                             % (writer.getScriptName(),))
    _require(writer.getParam("filename"), "Write.filename").set(
        OUTPUT_DIR_TOKEN + "/out.####.exr")
    if not writer.connectInput(0, reader):
        raise AssertionError("could not connect Read1 to Write1")

    _require(app.getProjectParam("frameRange"), "the project frame range").set(1, 3)

    if not app.saveProject(out_path):
        raise AssertionError("could not save the project to %r" % (out_path,))


main()
