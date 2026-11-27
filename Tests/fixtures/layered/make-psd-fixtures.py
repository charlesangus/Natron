#!/usr/bin/env python3
# Regenerates the Photoshop fixtures NativeReadPsd_Test.cpp reads:
#
#   python3 Tests/fixtures/layered/make-psd-fixtures.py Tests/fixtures/layered
#
# Only the standard library is used, so the files are hand-built to Adobe's file format
# specification rather than saved by Photoshop; the tests pin the exact values written here.
#
# groups8.psd, 8-bit RGB 8x6, layers bottom to top: Background (8x6 at 64, raw), the group
#   "Group A" (opacity 128, pass through) holding Inner (2x2 green at 2,1, RLE), Empty (no
#   pixels) and Masked (4x2 red at 4,3, ZIP, with a 2x2 user mask of 128 at 4,3 whose outside is
#   0), Top (Pascal name "Top", Unicode name "Top Layer"; 4x3 at 6,4 so cropped to 2x2; blue
#   50/110/170/230 across each row; ZIP with prediction; opacity 128, multiply, clipped), a
#   Levels adjustment layer and Hidden (Unicode name with a non-ASCII letter, hidden). The merged
#   image has transparency: RGBA 200/210/220/128 matted against white, RLE.
# deep16.psd, 16-bit Grayscale 4x2, layers in an Lr16 block: Shade, whose rows are 0x1000,
#   0xF000, 0x0100, 0x8000 (ZIP with prediction). The merged image is a raw 0x8000 with no alpha.
# float32.psd, 32-bit RGB 4x2, layers in an Lr32 block: Glow (4x2, R 2.0 + 0.5x, G 0.5,
#   B -0.25, alpha 0.5; ZIP with prediction) and, in the hidden group "Off", Ghost (2x2 of 1.0 at
#   2,0, raw). Its version info says it has no real merged image.
# layers.psb, PSB 8-bit RGB 8x6: Base (8x6 at 100, RLE with 4-byte row counts) and, in the
#   closed hidden group "Hidden Group", Spot (2x2 yellow at 2,2, RLE) with an empty user mask and
#   a real user mask of 0 over its top row whose outside is 255. The merged image is 1/2/3, RLE.
# wide.psb, PSB 8-bit RGB 300000x4: Strip (4x2 red at 299996,0, raw) and Band (300000x4 at 0,0,
#   RLE, only a red channel, whose rows are 40, 80, 120, 160); the merged image is black,
#   ZIP-compressed.
# cmyk.psd, a 2x2 CMYK document.
import os
import struct
import sys
import zlib

SECTION_OPEN = 1
SECTION_CLOSED = 2
SECTION_END = 3
WIDE_KEYS = {b"LMsk", b"Lr16", b"Lr32", b"Layr", b"Mt16", b"Mt32", b"Mtrn", b"Alph", b"FMsk", b"lnk2", b"FEid", b"FXid", b"PxSD"}


def be(fmt, *values):
    return struct.pack(">" + fmt, *values)


def length(value, wide):
    return be("Q" if wide else "I", value)


def pad(data, multiple):
    return data + b"\0" * (-len(data) % multiple)


def pascal(name, multiple):
    raw = name.encode("latin-1")
    return pad(bytes([len(raw)]) + raw, multiple)


def unicode_name(name):
    units = name.encode("utf-16-be")
    return be("I", len(units) // 2) + units


def tagged(key, data, psb=False):
    data = pad(data, 4)
    return b"8BIM" + key + length(len(data), psb and key in WIDE_KEYS) + data


def section(kind, blend=None):
    data = be("I", kind)
    if blend:
        data += b"8BIM" + blend
    return tagged(b"lsct", data)


def sample_bytes(value, depth):
    if depth == 8:
        return bytes([value])
    if depth == 16:
        return be("H", value)
    return be("f", value)


def packbits(row):
    out = bytearray()
    i = 0
    while i < len(row):
        run = 1
        while i + run < len(row) and run < 128 and row[i + run] == row[i]:
            run += 1
        if run >= 2:
            out += bytes([257 - run, row[i]])
            i += run
            continue
        start = i
        while i < len(row) and i - start < 128 and (i + 1 >= len(row) or row[i + 1] != row[i]):
            i += 1
        if i == start:
            i += 1
        out += bytes([i - start - 1]) + row[start:i]
    return bytes(out)


def predicted(row, width, depth):
    if depth == 8:
        return bytes([row[0]] + [(row[x] - row[x - 1]) & 0xFF for x in range(1, width)])
    if depth == 16:
        values = struct.unpack(">%dH" % width, row)
        return be("%dH" % width, values[0], *[(values[x] - values[x - 1]) & 0xFFFF for x in range(1, width)])
    planes = bytes(row[x * 4 + k] for k in range(4) for x in range(width))
    return bytes([planes[0]] + [(planes[x] - planes[x - 1]) & 0xFF for x in range(1, len(planes))])


def encode_planes(planes, width, height, depth, compression, psb):
    """`planes` are lists of rows of sample values; returns the compression word and data."""
    rows = [b"".join(sample_bytes(v, depth) for v in row) for plane in planes for row in plane]
    if compression == 0:
        body = b"".join(rows)
    elif compression == 1:
        packed = [packbits(row) for row in rows]
        body = b"".join(length(len(p), psb)[-(4 if psb else 2):] for p in packed) + b"".join(packed)
    elif compression == 2:
        body = zlib.compress(b"".join(rows), 9)
    else:
        body = zlib.compress(b"".join(predicted(row, width, depth) for row in rows), 9)
    return be("H", compression) + body


def constant(width, height, value):
    return [[value] * width for _ in range(height)]


class Layer:
    def __init__(self, name, rect=(0, 0, 0, 0), channels=(), compression=0, blend=b"norm", opacity=255,
                 clipping=0, hidden=False, mask=b"", tags=b""):
        self.name = name
        self.rect = rect
        self.channels = channels
        self.compression = compression
        self.blend = blend
        self.opacity = opacity
        self.clipping = clipping
        self.hidden = hidden
        self.mask = mask
        self.tags = tags

    def channel_data(self, depth, psb):
        top, left, bottom, right = self.rect
        out = []
        for cid, plane, rect, compression in self.channels:
            t, l, b, r = rect if rect else self.rect
            if plane is None:
                out.append((cid, be("H", 0)))
            else:
                out.append((cid, encode_planes([plane], r - l, b - t, depth, self.compression if compression is None else compression, psb)))
        return out


def channel(cid, plane, rect=None, compression=None):
    return (cid, plane, rect, compression)


def empty_channels(*ids):
    return tuple(channel(cid, None) for cid in ids)


def user_mask(rect, default):
    return be("I", 20) + be("4i", *rect) + bytes([default, 0]) + b"\0\0"


def real_mask(rect, default, real_rect, real_default):
    return be("I", 36) + be("4i", *rect) + bytes([default, 0]) + bytes([0, real_default]) + be("4i", *real_rect)


def layer_info(layers, merged_alpha, depth, psb):
    records = b""
    data = b""
    for layer in layers:
        channels = layer.channel_data(depth, psb)
        records += be("4i", *layer.rect) + be("H", len(channels))
        for cid, payload in channels:
            records += be("h", cid) + length(len(payload), psb)
        records += b"8BIM" + layer.blend + bytes([layer.opacity, layer.clipping, 0x0A if layer.hidden else 0x08, 0])
        extra = (layer.mask or be("I", 0)) + be("I", 0) + pascal(layer.name, 4) + layer.tags
        records += be("I", len(extra)) + extra
        data += b"".join(payload for _, payload in channels)
    count = -len(layers) if merged_alpha else len(layers)
    return pad(be("h", count) + records + data, 2)


def version_info(real_merged):
    data = be("I", 1) + bytes([1 if real_merged else 0]) + unicode_name("Natron") + unicode_name("Natron") + be("I", 1)
    return b"8BIM" + be("H", 1057) + b"\0\0" + be("I", len(data)) + pad(data, 2)


def document(width, height, depth, mode, channels, layers, merged, merged_alpha=False, psb=False,
             real_merged=True, layer_block=None):
    header = b"8BPS" + be("H", 2 if psb else 1) + b"\0" * 6 + be("H", channels) + be("II", height, width) + be("HH", depth, mode)
    resources = version_info(real_merged)
    info = layer_info(layers, merged_alpha, depth, psb) if layers else b""
    if layer_block:
        section_data = length(0, psb) + be("I", 0) + tagged(layer_block, info, psb)
    else:
        section_data = length(len(info), psb) + info + be("I", 0)
    return header + be("I", 0) + be("I", len(resources)) + resources + length(len(section_data), psb) + section_data + merged


def groups8():
    green = [channel(-1, constant(2, 2, 255)), channel(0, constant(2, 2, 0)), channel(1, constant(2, 2, 255)), channel(2, constant(2, 2, 0))]
    red = [channel(-1, constant(4, 2, 255)), channel(0, constant(4, 2, 255)), channel(1, constant(4, 2, 0)), channel(2, constant(4, 2, 0)),
           channel(-2, constant(2, 2, 128), rect=(3, 4, 5, 6), compression=0)]
    blue_row = [50, 110, 170, 230]
    blue = [channel(-1, constant(4, 3, 255)), channel(0, constant(4, 3, 0)), channel(1, constant(4, 3, 0)), channel(2, [blue_row] * 3)]
    white = [channel(c, constant(2, 2, 255)) for c in (-1, 0, 1, 2)]
    layers = [
        Layer("Background", (0, 0, 6, 8), [channel(c, constant(8, 6, 64)) for c in (0, 1, 2)], compression=0),
        Layer("</Layer group>", channels=empty_channels(-1, 0, 1, 2), tags=section(SECTION_END)),
        Layer("Inner", (1, 2, 3, 4), green, compression=1),
        Layer("Empty", channels=empty_channels(-1, 0, 1, 2)),
        Layer("Masked", (3, 4, 5, 8), red, compression=2, mask=user_mask((3, 4, 5, 6), 0)),
        Layer("Group A", channels=empty_channels(-1, 0, 1, 2), opacity=128, blend=b"pass", tags=section(SECTION_OPEN, b"pass")),
        Layer("Top", (4, 6, 7, 10), blue, compression=3, blend=b"mul ", opacity=128, clipping=1, tags=tagged(b"luni", unicode_name("Top Layer"))),
        Layer("Levels 1", channels=empty_channels(-1, 0, 1, 2), tags=tagged(b"levl", be("H", 2) + b"\0" * 8)),
        Layer("Hidden", (0, 0, 2, 2), white, compression=0, hidden=True, tags=tagged(b"luni", unicode_name("Hïdden"))),
    ]
    merged = encode_planes([constant(8, 6, v) for v in (200, 210, 220, 128)], 8, 6, 8, 1, False)
    return document(8, 6, 8, 3, 4, layers, merged, merged_alpha=True)


def deep16():
    row = [0x1000, 0xF000, 0x0100, 0x8000]
    layers = [Layer("Shade", (0, 0, 2, 4), [channel(0, [row, row])], compression=3)]
    merged = encode_planes([constant(4, 2, 0x8000)], 4, 2, 16, 0, False)
    return document(4, 2, 16, 1, 1, layers, merged, layer_block=b"Lr16")


def float32():
    glow = [channel(-1, constant(4, 2, 0.5)), channel(0, [[2.0 + 0.5 * x for x in range(4)]] * 2),
            channel(1, constant(4, 2, 0.5)), channel(2, constant(4, 2, -0.25))]
    ghost = [channel(c, constant(2, 2, 1.0)) for c in (-1, 0, 1, 2)]
    layers = [
        Layer("Glow", (0, 0, 2, 4), glow, compression=3),
        Layer("</Layer group>", channels=empty_channels(-1, 0, 1, 2), tags=section(SECTION_END)),
        Layer("Ghost", (0, 2, 2, 4), ghost, compression=0),
        Layer("Off", channels=empty_channels(-1, 0, 1, 2), hidden=True, tags=section(SECTION_OPEN, b"pass")),
    ]
    merged = encode_planes([constant(4, 2, 0.0) for _ in range(3)], 4, 2, 32, 0, False)
    return document(4, 2, 32, 3, 3, layers, merged, merged_alpha=True, real_merged=False, layer_block=b"Lr32")


def layers_psb():
    yellow = [channel(-1, constant(2, 2, 255)), channel(0, constant(2, 2, 255)), channel(1, constant(2, 2, 255)),
              channel(2, constant(2, 2, 0)), channel(-2, None), channel(-3, constant(2, 1, 0), rect=(2, 2, 3, 4))]
    layers = [
        Layer("Base", (0, 0, 6, 8), [channel(c, constant(8, 6, 100)) for c in (0, 1, 2)], compression=1),
        Layer("</Layer group>", channels=empty_channels(-1, 0, 1, 2), tags=section(SECTION_END)),
        Layer("Spot", (2, 2, 4, 4), yellow, compression=1, mask=real_mask((0, 0, 0, 0), 0, (2, 2, 3, 4), 255)),
        Layer("Hidden Group", channels=empty_channels(-1, 0, 1, 2), hidden=True, tags=section(SECTION_CLOSED, b"norm")),
    ]
    merged = encode_planes([constant(8, 6, v) for v in (1, 2, 3)], 8, 6, 8, 1, True)
    return document(8, 6, 8, 3, 3, layers, merged, psb=True)


def wide_psb():
    width = 300000
    height = 4
    strip = [channel(c, constant(4, 2, 255 if c == 0 else 0)) for c in (0, 1, 2)]
    band = [channel(0, [[40 * (row + 1)] * width for row in range(height)])]
    layers = [
        Layer("Strip", (0, width - 4, 2, width), strip, compression=0),
        Layer("Band", (0, 0, height, width), band, compression=1),
    ]
    merged = be("H", 2) + zlib.compress(b"\0" * (width * height * 3), 9)
    return document(width, height, 8, 3, 3, layers, merged, psb=True)


def cmyk():
    merged = encode_planes([constant(2, 2, 0) for _ in range(4)], 2, 2, 8, 0, False)
    return document(2, 2, 8, 4, 4, [], merged)


def main(out):
    files = {
        "groups8.psd": groups8(),
        "deep16.psd": deep16(),
        "float32.psd": float32(),
        "layers.psb": layers_psb(),
        "wide.psb": wide_psb(),
        "cmyk.psd": cmyk(),
    }
    for name, data in files.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
