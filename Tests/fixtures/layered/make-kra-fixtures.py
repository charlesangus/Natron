#!/usr/bin/env python3
# Regenerates the Krita fixtures NativeReadKra_Test.cpp reads:
#
#   python3 Tests/fixtures/layered/make-kra-fixtures.py Tests/fixtures/layered
#
# Only the standard library is used, so the files are hand-built to Krita's maindoc.xml and
# version 2 tile layout rather than saved by Krita; the tests pin the exact values written here.
# Tiles are 64x64 as Krita writes them; 8- and 16-bit RGB pixels are stored B, G, R, A, float ones
# R, G, B, A, all with unassociated alpha.
#
# layers8.kra, 8-bit RGBA 8x6, layers top to bottom: Hidden (white, hidden), Top Layer (at 6,4 so
#   cropped to 2x2; R 10 + 10x, G 20y, B 200; opacity 128, multiply, inherit alpha), a filter
#   layer Levels, the group "Group A" (opacity 128) holding Masked (red at 4,0, an uncompressed
#   tile, with a transparency mask at 4,0 of 128 over its first two columns and 0 elsewhere, a filter
#   mask and a hidden transparency mask), Empty (no tiles), Inner (green with alpha 128 at 1,1)
#   and a transparency mask on the group itself, a fill layer Fill, a vector layer Vector and
#   Background, which stores no tiles and whose default pixel is an opaque 64. Its
#   mergedimage.png is a flat 10/20/30.
# deep16.kra, 16-bit Grayscale 4x2: Tint (an RGBA16 layer of 0x3333/0x2222/0x1111, uncompressed)
#   over Shade (columns 0x1000, 0xF000, 0x0100, 0x8000); mergedimage.png is a 16-bit gray 0x8000.
# float.kra, 32-bit float RGBA 4x2 with no mergedimage.png: Glow (R 2 + 0.5x, G 0.5, B -0.25,
#   alpha 0.5) and, in the hidden group "Off", Half (an RGBAF16 layer of 1.0 at 2,0).
# tiles.kra, 8-bit RGBA 256x64: Strip, whose tiles at columns 0, 64 and 192 are red 40, 80 and
#   160, with nothing stored at 128; mergedimage.png is black.
# cmyk.kra, a 2x2 CMYK document.
import os
import struct
import sys
import zipfile
import zlib

TILE = 64


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


def lzf(data):
    out = bytearray()
    last = {}
    literal = bytearray()

    def flush():
        while literal:
            run = literal[:32]
            out.append(len(run) - 1)
            out.extend(run)
            del literal[:32]

    i = 0
    while i < len(data):
        key = bytes(data[i:i + 3])
        candidate = last.get(key) if len(key) == 3 else None
        if len(key) == 3:
            last[key] = i
        if candidate is not None and i - candidate <= 8192:
            length = 3
            while length < 264 and i + length < len(data) and data[candidate + length] == data[i + length]:
                length += 1
            flush()
            code = length - 2
            distance = i - candidate - 1
            if code < 7:
                out.append((code << 5) | (distance >> 8))
            else:
                out.append((7 << 5) | (distance >> 8))
                out.append(code - 7)
            out.append(distance & 0xFF)
            i += length
            continue
        literal.append(data[i])
        i += 1
    flush()
    return bytes(out)


def tile_pixels(fmt, pixel_at):
    return b"".join(struct.pack(fmt, *pixel_at(x, y)) for y in range(TILE) for x in range(TILE))


def compressed(pixels, pixel_size):
    count = len(pixels) // pixel_size
    planes = bytes(pixels[p * pixel_size + b] for b in range(pixel_size) for p in range(count))
    return b"\x01" + lzf(planes)


def device(pixel_size, tiles):
    """tiles: a list of (left, top, data, packed) with `data` the tile's pixels."""
    out = b"VERSION 2\nTILEWIDTH %d\nTILEHEIGHT %d\nPIXELSIZE %d\nDATA %d\n" % (TILE, TILE, pixel_size, len(tiles))
    for left, top, data, packed in tiles:
        body = compressed(data, pixel_size) if packed else b"\x00" + data
        out += b"%d,%d,LZF,%d\n" % (left, top, len(body)) + body
    return out


def write_kra(path, name, colorspace, width, height, layers_xml, members, mimetype="application/x-krita"):
    maindoc = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE DOC PUBLIC '-//KDE//DTD krita 2.0//EN' 'http://www.calligra.org/DTD/krita-2.0.dtd'>
<DOC xmlns="http://www.calligra.org/DTD/krita" syntaxVersion="2.0" editor="Krita" kritaVersion="5.2.2">
 <IMAGE width="%d" height="%d" colorspacename="%s" name="%s" mime="application/x-kra" x-res="72" y-res="72">
  <layers>
%s
  </layers>
 </IMAGE>
</DOC>
""" % (width, height, colorspace, name, layers_xml)
    # ZipInfo's fixed 1980 timestamp keeps the files byte for byte the same on every run.
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), mimetype, compress_type=zipfile.ZIP_STORED)
        z.writestr(zipfile.ZipInfo("maindoc.xml"), maindoc, compress_type=zipfile.ZIP_DEFLATED)
        for member, data in members:
            z.writestr(zipfile.ZipInfo(member if member == "mergedimage.png" else name + "/layers/" + member), data, compress_type=zipfile.ZIP_DEFLATED)


def solid(fmt, pixel):
    return tile_pixels(fmt, lambda x, y: pixel)


def main(out):
    bgra8 = "<BBBB"
    write_kra(
        os.path.join(out, "layers8.kra"),
        "layers8",
        "RGBA",
        8,
        6,
        """   <layer name="Hidden" nodetype="paintlayer" filename="layer9" x="0" y="0" opacity="255" visible="0" compositeop="normal" colorspacename="RGBA"/>
   <layer name="Top Layer" nodetype="paintlayer" filename="layer8" x="6" y="4" opacity="128" visible="1" compositeop="multiply" channelflags="1110" colorspacename="RGBA"/>
   <layer name="Levels" nodetype="adjustmentlayer" filename="layer7" filtername="levels" x="0" y="0" opacity="255" visible="1" compositeop="normal"/>
   <layer name="Group A" nodetype="grouplayer" filename="layer6" x="0" y="0" opacity="128" visible="1" compositeop="normal" passthrough="0">
    <masks>
     <mask name="Group Mask" nodetype="transparencymask" filename="mask4" x="0" y="0" visible="1"/>
    </masks>
    <layers>
     <layer name="Masked" nodetype="paintlayer" filename="layer5" x="4" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="RGBA">
      <masks>
       <mask name="Off Mask" nodetype="transparencymask" filename="mask3" x="0" y="0" visible="0"/>
       <mask name="Blur Mask" nodetype="filtermask" filename="mask2" filtername="blur" x="0" y="0" visible="1"/>
       <mask name="Mask" nodetype="transparencymask" filename="mask1" x="4" y="0" visible="1"/>
      </masks>
     </layer>
     <layer name="Empty" nodetype="paintlayer" filename="layer4" x="0" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="RGBA"/>
     <layer name="Inner" nodetype="paintlayer" filename="layer3" x="1" y="1" opacity="255" visible="1" compositeop="normal"/>
    </layers>
   </layer>
   <layer name="Fill" nodetype="generatorlayer" filename="layer2" generatorname="color" x="0" y="0" opacity="255" visible="1" compositeop="normal"/>
   <layer name="Vector" nodetype="shapelayer" filename="layer10" x="0" y="0" opacity="255" visible="1" compositeop="normal"/>
   <layer name="Background" nodetype="paintlayer" filename="layer1" x="0" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="RGBA"/>""",
        [
            ("layer9", device(4, [(0, 0, solid(bgra8, (255, 255, 255, 255)), True)])),
            ("layer8", device(4, [(0, 0, tile_pixels(bgra8, lambda x, y: (200, min(20 * y, 255), min(10 + 10 * x, 255), 255)), True)])),
            ("layer5", device(4, [(0, 0, solid(bgra8, (0, 0, 255, 255)), False)])),
            ("mask1.pixelselection", device(1, [(0, 0, tile_pixels("<B", lambda x, y: (128 if x < 2 else 0,)), True)])),
            ("mask1.pixelselection.defaultpixel", b"\x00"),
            ("mask3.pixelselection", device(1, [(0, 0, solid("<B", (0,)), True)])),
            ("mask4.pixelselection", device(1, [(0, 0, solid("<B", (0,)), True)])),
            ("layer4", device(4, [])),
            ("layer3", device(4, [(0, 0, solid(bgra8, (0, 255, 0, 128)), True)])),
            ("layer1", device(4, [])),
            ("layer1.defaultpixel", bytes((64, 64, 64, 255))),
            ("mergedimage.png", png(8, 6, 4, (10, 20, 30, 255))),
        ],
    )
    write_kra(
        os.path.join(out, "deep16.kra"),
        "deep16",
        "GRAYAU16",
        4,
        2,
        """   <layer name="Tint" nodetype="paintlayer" filename="layer2" x="0" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="RGBA16"/>
   <layer name="Shade" nodetype="paintlayer" filename="layer1" x="0" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="GRAYAU16"/>""",
        [
            ("layer2", device(8, [(0, 0, solid("<HHHH", (0x1111, 0x2222, 0x3333, 0xFFFF)), False)])),
            ("layer1", device(4, [(0, 0, tile_pixels("<HH", lambda x, y: ((0x1000, 0xF000, 0x0100, 0x8000)[x % 4], 0xFFFF)), True)])),
            ("mergedimage.png", png(4, 2, 1, (0x8000,), depth=16)),
        ],
    )
    write_kra(
        os.path.join(out, "float.kra"),
        "float",
        "RGBAF32",
        4,
        2,
        """   <layer name="Off" nodetype="grouplayer" filename="layer3" x="0" y="0" opacity="255" visible="0" compositeop="normal">
    <layers>
     <layer name="Half" nodetype="paintlayer" filename="layer2" x="2" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="RGBAF16"/>
    </layers>
   </layer>
   <layer name="Glow" nodetype="paintlayer" filename="layer1" x="0" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="RGBAF32"/>""",
        [
            ("layer2", device(8, [(0, 0, solid("<eeee", (1.0, 1.0, 1.0, 1.0)), True)])),
            ("layer1", device(16, [(0, 0, tile_pixels("<ffff", lambda x, y: (2.0 + 0.5 * x, 0.5, -0.25, 0.5)), True)])),
        ],
    )
    write_kra(
        os.path.join(out, "tiles.kra"),
        "tiles",
        "RGBA",
        256,
        64,
        """   <layer name="Strip" nodetype="paintlayer" filename="layer1" x="0" y="0" opacity="255" visible="1" compositeop="normal" colorspacename="RGBA"/>""",
        [
            ("layer1", device(4, [(left, 0, solid(bgra8, (0, 0, red, 255)), True) for left, red in ((0, 40), (64, 80), (192, 160))])),
            ("mergedimage.png", png(256, 64, 3, (0, 0, 0))),
        ],
    )
    write_kra(os.path.join(out, "cmyk.kra"), "cmyk", "CMYK", 2, 2, "", [])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
