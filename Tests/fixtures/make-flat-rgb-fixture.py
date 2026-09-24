#!/usr/bin/env python3
# Regenerates the RGB-only EXR fixture ShuffleRender_Test.cpp reads:
#
#   python3 Tests/fixtures/make-flat-rgb-fixture.py Tests/fixtures
#
# Writes flat-rgb-only.exr: an 8x8, single-part, scanline, uncompressed half-float EXR whose
# only layer is a three-channel Color (red), so a Read of it presents Color without an alpha.
# Written with the standard library alone, so it runs anywhere and regenerates byte-identically.
import os
import struct
import sys

WIDTH = 8
HEIGHT = 8
VALUES = {"R": 1.0, "G": 0.0, "B": 0.0}

HALF = 1
NO_COMPRESSION = 0
INCREASING_Y = 0


def attribute(name, type_name, value):
    return name.encode() + b"\0" + type_name.encode() + b"\0" + struct.pack("<i", len(value)) + value


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    path = "%s/flat-rgb-only.exr" % outdir

    # EXR stores channels, and each scanline's samples, in channel-name order.
    channels = sorted(VALUES)
    chlist = b"".join(name.encode() + b"\0" + struct.pack("<iB3xii", HALF, 0, 1, 1) for name in channels) + b"\0"
    window = struct.pack("<4i", 0, 0, WIDTH - 1, HEIGHT - 1)

    header = b"\x76\x2f\x31\x01" + struct.pack("<i", 2)
    header += attribute("channels", "chlist", chlist)
    header += attribute("compression", "compression", struct.pack("<B", NO_COMPRESSION))
    header += attribute("dataWindow", "box2i", window)
    header += attribute("displayWindow", "box2i", window)
    header += attribute("lineOrder", "lineOrder", struct.pack("<B", INCREASING_Y))
    header += attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += attribute("screenWindowCenter", "v2f", struct.pack("<2f", 0.0, 0.0))
    header += attribute("screenWindowWidth", "float", struct.pack("<f", 1.0))
    header += b"\0"

    line = b"".join(struct.pack("<e", VALUES[name]) * WIDTH for name in channels)
    chunks = [struct.pack("<ii", y, len(line)) + line for y in range(HEIGHT)]

    # Uncompressed EXR holds one scanline per chunk, each located through the offset table.
    offset = len(header) + 8 * HEIGHT
    table = b""
    for chunk in chunks:
        table += struct.pack("<Q", offset)
        offset += len(chunk)

    with open(path, "wb") as out:
        out.write(header + table + b"".join(chunks))
    print("wrote %s" % path)


if __name__ == "__main__":
    main()
