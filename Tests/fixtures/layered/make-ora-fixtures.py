#!/usr/bin/env python3
# Regenerates the OpenRaster fixtures NativeReadOra_Test.cpp and NativeReadLayeredDocs_Test.cpp
# read:
#
#   python3 Tests/fixtures/layered/make-ora-fixtures.py Tests/fixtures/layered
#
# Only the standard library is used, so the files are hand-built rather than saved by a painting
# application; the tests pin the exact values written here.
#
# nested.ora, 8x6: Background (8x6, 64/64/64), Color (4x4 yellow at 6,4, so cropped to 2x2), the
#   group "Group A" (offset 1,1, opacity 0.5) holding Inner (2x2 green at 1,0) and the hidden
#   group "Deep" holding Leaf (1x1 blue), Top Layer (4x3 red at 2,1, opacity 0.5, multiply), an
#   Empty layer with no pixels and a text element. Its mergedimage.png is a flat 10/20/30, which
#   no flattening of the layers gives.
# flatten.ora, 4x4, with no mergedimage.png: Background (opaque blue), Hidden (white, hidden) and
#   Top (2x2 red with alpha 128 at 1,1).
# gray16.ora, 4x2: a 16-bit gray mergedimage.png of 0x8000 and one 16-bit gray layer, Shade, of
#   0x4000.
# wrongmime.ora: a well-formed stack whose mimetype is not OpenRaster's.
import os
import struct
import sys
import zipfile
import zlib


def png(width, height, channels, pixel, depth=8):
    color_type = {1: 0, 2: 4, 3: 2, 4: 6}[channels]
    fmt = ">" + ("H" if depth == 16 else "B") * channels
    row = b"".join(struct.pack(fmt, *pixel) for _ in range(width))
    raw = b"".join(b"\x00" + row for _ in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def write_ora(path, stack, members, mimetype="image/openraster"):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), mimetype, compress_type=zipfile.ZIP_STORED)
        z.writestr("stack.xml", stack, compress_type=zipfile.ZIP_DEFLATED)
        for name, data in members:
            z.writestr(name, data, compress_type=zipfile.ZIP_DEFLATED)


def main(out):
    write_ora(
        os.path.join(out, "nested.ora"),
        """<?xml version="1.0" encoding="UTF-8"?>
<image version="0.0.3" w="8" h="6">
 <stack>
  <layer name="Top Layer" src="data/top.png" x="2" y="1" opacity="0.5" composite-op="svg:multiply"/>
  <stack name="Group A" x="1" y="1" opacity="0.5">
   <layer name="Inner" src="data/inner.png" x="1" y="0"/>
   <stack name="Deep" visibility="hidden">
    <layer name="Leaf" src="data/leaf.png"/>
   </stack>
  </stack>
  <layer name="Empty"/>
  <text name="Caption"/>
  <layer name="Color" src="data/color.png" x="6" y="4"/>
  <layer name="Background" src="data/bg.png"/>
 </stack>
</image>
""",
        [
            ("data/top.png", png(4, 3, 4, (255, 0, 0, 255))),
            ("data/inner.png", png(2, 2, 4, (0, 255, 0, 255))),
            ("data/leaf.png", png(1, 1, 4, (0, 0, 255, 255))),
            ("data/color.png", png(4, 4, 4, (255, 255, 0, 255))),
            ("data/bg.png", png(8, 6, 3, (64, 64, 64))),
            ("mergedimage.png", png(8, 6, 4, (10, 20, 30, 255))),
        ],
    )
    write_ora(
        os.path.join(out, "flatten.ora"),
        """<?xml version="1.0" encoding="UTF-8"?>
<image version="0.0.3" w="4" h="4">
 <stack>
  <layer name="Top" src="data/top.png" x="1" y="1"/>
  <layer name="Hidden" src="data/hidden.png" visibility="hidden"/>
  <layer name="Background" src="data/bg.png"/>
 </stack>
</image>
""",
        [
            ("data/top.png", png(2, 2, 4, (255, 0, 0, 128))),
            ("data/hidden.png", png(4, 4, 4, (255, 255, 255, 255))),
            ("data/bg.png", png(4, 4, 4, (0, 0, 255, 255))),
        ],
    )
    write_ora(
        os.path.join(out, "gray16.ora"),
        """<?xml version="1.0" encoding="UTF-8"?>
<image version="0.0.3" w="4" h="2">
 <stack>
  <layer name="Shade" src="data/shade.png"/>
 </stack>
</image>
""",
        [
            ("data/shade.png", png(4, 2, 1, (0x4000,), depth=16)),
            ("mergedimage.png", png(4, 2, 1, (0x8000,), depth=16)),
        ],
    )
    write_ora(
        os.path.join(out, "wrongmime.ora"),
        """<?xml version="1.0" encoding="UTF-8"?>
<image version="0.0.3" w="2" h="2">
 <stack>
  <layer name="Background" src="data/bg.png"/>
 </stack>
</image>
""",
        [("data/bg.png", png(2, 2, 4, (0, 0, 0, 255)))],
        mimetype="image/png",
    )


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
