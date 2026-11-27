#!/usr/bin/env python3
# Regenerates the GIMP fixtures NativeReadXcf_Test.cpp reads:
#
#   python3 Tests/fixtures/layered/make-xcf-fixtures.py Tests/fixtures/layered
#
# Only the standard library is used, so the files are hand-built to the XCF specification rather
# than saved by GIMP; the tests pin the exact values written here. Tiles are 64x64, smaller on the
# right and bottom edges, components big-endian with unassociated alpha. Layers are listed top
# to bottom; a layer inside a group carries the item path of its index at each level.
#
# legacy8.xcf, version 3 (4-byte pointers), 8-bit RGB 8x6, RLE tiles: Floating (a floating
#   selection), Top Layer (4x3 at 6,4 so cropped to 2x2; R 10 + 10x, G 20y, B 200; opacity 128,
#   legacy multiply), Hidden (white, hidden), the group "Group A" (opacity 128) holding Masked
#   (red 4x6 at 4,0 with a mask of 128 over its first two columns and 0 elsewhere, no apply-mask
#   property), Unmasked (blue 2x2 at 0,4 with a mask of 0 and apply-mask off), Empty (0x0) and
#   Inner (green with alpha 128, 7x5 at 1,1), then Background (RGB without alpha, an opaque 64).
# float32.xcf, version 14 (8-byte pointers), linear float RGB 4x2, zlib tiles: Glow (R 2 + 0.5x,
#   G 0.5, B -0.25, alpha 0.5; opacity 191 overridden by a float opacity of 0.75, normal) and the
#   hidden group "Off", which has a mask of its own and holds Child (1.0 at 2,0, 2x2).
# gray16.xcf, version 20, 16-bit perceptual Grayscale 4x2, RLE tiles: Tint (gray 0x3333 with
#   alpha, a mask of 0x8000) over Shade (no alpha; columns 0x1000, 0xF000, 0x0100, 0x8000).
# tiles.xcf, version 11 (8-byte pointers), 8-bit RGB 256x64, RLE tiles: Strip, whose four tiles
#   are red 40, 80, 120 and 160 with G (x + 3y) mod 256 on the top two rows and 0 below, B 0
#   and an opaque alpha.
# indexed.xcf, a version 3 Indexed 2x2 document.
import os
import struct
import sys
import zlib

TILE = 64

PROP_END = 0
PROP_COLORMAP = 1
PROP_FLOATING_SELECTION = 5
PROP_OPACITY = 6
PROP_MODE = 7
PROP_VISIBLE = 8
PROP_APPLY_MASK = 11
PROP_OFFSETS = 15
PROP_COMPRESSION = 17
PROP_GROUP_ITEM = 29
PROP_ITEM_PATH = 30
PROP_FLOAT_OPACITY = 33

RAW, RLE, ZLIB = 0, 1, 2


def prop(kind, payload=b""):
    return struct.pack(">II", kind, len(payload)) + payload


def u32(value):
    return struct.pack(">I", value)


def string(text):
    data = text.encode("utf-8") + b"\x00"
    return u32(len(data)) + data


def rle(stream):
    """Every opcode is used: short and long repeats, short and long literal copies."""
    out = bytearray()
    i = 0
    n = len(stream)
    while i < n:
        run = 1
        while i + run < n and stream[i + run] == stream[i] and run < 0xFFFF:
            run += 1
        if run >= 3:
            if run <= 127:
                out.append(run - 1)
            else:
                out += bytes((127, run >> 8, run & 0xFF))
            out.append(stream[i])
            i += run
            continue
        start = i
        while i < n and i - start < 0xFFFF:
            if i + 2 < n and stream[i] == stream[i + 1] == stream[i + 2]:
                break
            i += 1
        literal = stream[start:i]
        if len(literal) <= 127:
            out.append(256 - len(literal))
        else:
            out += bytes((128, len(literal) >> 8, len(literal) & 0xFF))
        out += literal
    return bytes(out)


def encode_tile(data, pixel_size, compression):
    if compression == RAW:
        return data
    if compression == ZLIB:
        return zlib.compress(data, 9)
    count = len(data) // pixel_size
    return b"".join(rle(data[b::pixel_size]) for b in range(pixel_size)) if count else b""


class Writer:
    def __init__(self, version, compression, fmt):
        self.wide = version >= 11
        self.compression = compression
        self.fmt = fmt
        self.data = bytearray()

    def pointer(self, value=0):
        return struct.pack(">Q" if self.wide else ">I", value)

    def patch(self, at, value):
        packed = self.pointer(value)
        self.data[at:at + len(packed)] = packed

    def append(self, blob):
        at = len(self.data)
        self.data += blob
        return at

    def hierarchy(self, width, height, channels, pixel_at):
        """pixel_at(x, y) gives the components of a pixel; returns the hierarchy's offset."""
        pixel_size = channels * struct.calcsize(">" + self.fmt)
        columns = (width + TILE - 1) // TILE
        rows = (height + TILE - 1) // TILE
        at = self.append(struct.pack(">III", width, height, pixel_size))
        level_slot = self.append(self.pointer())
        self.append(self.pointer())
        level = self.append(struct.pack(">II", width, height))
        self.patch(level_slot, level)
        slots = [self.append(self.pointer()) for _ in range(columns * rows)]
        self.append(self.pointer())
        for row in range(rows):
            for column in range(columns):
                tw = min(TILE, width - column * TILE)
                th = min(TILE, height - row * TILE)
                pixels = b"".join(
                    struct.pack(">" + self.fmt * channels, *pixel_at(column * TILE + x, row * TILE + y))
                    for y in range(th) for x in range(tw))
                self.patch(slots[row * columns + column], self.append(encode_tile(pixels, pixel_size, self.compression)))
        return at

    def channel(self, name, width, height, value_at):
        at = self.append(struct.pack(">II", width, height) + string(name) + prop(PROP_END))
        slot = self.append(self.pointer())
        self.patch(slot, self.hierarchy(width, height, 1, lambda x, y: (value_at(x, y),)))
        return at

    def layer(self, layer):
        width, height = layer["size"]
        kind = layer.get("type", 1)
        props = b""
        if layer.get("floating"):
            props += prop(PROP_FLOATING_SELECTION, self.pointer())
        if "opacity" in layer:
            props += prop(PROP_OPACITY, u32(layer["opacity"]))
        if "float_opacity" in layer:
            props += prop(PROP_FLOAT_OPACITY, struct.pack(">f", layer["float_opacity"]))
        if "mode" in layer:
            props += prop(PROP_MODE, u32(layer["mode"]))
        props += prop(PROP_VISIBLE, u32(0 if layer.get("hidden") else 1))
        if "apply_mask" in layer:
            props += prop(PROP_APPLY_MASK, u32(layer["apply_mask"]))
        props += prop(PROP_OFFSETS, struct.pack(">ii", *layer.get("offset", (0, 0))))
        if layer.get("group"):
            props += prop(PROP_GROUP_ITEM)
        if "path" in layer:
            props += prop(PROP_ITEM_PATH, b"".join(u32(i) for i in layer["path"]))
        at = self.append(struct.pack(">III", width, height, kind) + string(layer["name"]) + props + prop(PROP_END))
        hierarchy_slot = self.append(self.pointer())
        mask_slot = self.append(self.pointer())
        channels = {0: 3, 1: 4, 2: 1, 3: 2}[kind]
        pixel = layer.get("pixel", lambda x, y: (0,) * channels)
        if width and height:
            self.patch(hierarchy_slot, self.hierarchy(width, height, channels, pixel))
        if "mask" in layer:
            self.patch(mask_slot, self.channel(layer.get("mask_name", "Mask"), width, height, layer["mask"]))
        return at


def write_xcf(path, version, base_type, precision, compression, fmt, width, height, layers, image_props=b""):
    w = Writer(version, compression, fmt)
    tag = b"file" if version == 0 else b"v%03d" % version
    header = b"gimp xcf " + tag + b"\x00" + struct.pack(">III", width, height, base_type)
    if version >= 4:
        header += u32(precision)
    header += image_props + prop(PROP_COMPRESSION, bytes((compression,))) + prop(PROP_END)
    w.append(header)
    slots = [w.append(w.pointer()) for _ in layers]
    w.append(w.pointer())
    w.append(w.pointer())
    for slot, layer in zip(slots, layers):
        w.patch(slot, w.layer(layer))
    with open(path, "wb") as f:
        f.write(bytes(w.data))


def main(out):
    write_xcf(
        os.path.join(out, "legacy8.xcf"), 3, 0, 0, RLE, "B", 8, 6,
        [
            {"name": "Floating", "size": (1, 1), "floating": True, "pixel": lambda x, y: (255, 0, 255, 255)},
            {"name": "Top Layer", "size": (4, 3), "offset": (6, 4), "opacity": 128, "mode": 3,
             "pixel": lambda x, y: (10 + 10 * x, 20 * y, 200, 255)},
            {"name": "Hidden", "size": (8, 6), "hidden": True, "opacity": 255, "pixel": lambda x, y: (255, 255, 255, 255)},
            {"name": "Group A", "size": (8, 6), "group": True, "opacity": 128},
            {"name": "Masked", "size": (4, 6), "offset": (4, 0), "path": [3, 0], "opacity": 255,
             "pixel": lambda x, y: (255, 0, 0, 255), "mask": lambda x, y: 128 if x < 2 else 0},
            {"name": "Unmasked", "size": (2, 2), "offset": (0, 4), "path": [3, 1], "opacity": 255, "apply_mask": 0,
             "pixel": lambda x, y: (0, 0, 255, 255), "mask": lambda x, y: 0, "mask_name": "Unused"},
            {"name": "Empty", "size": (0, 0), "path": [3, 2]},
            {"name": "Inner", "size": (7, 5), "offset": (1, 1), "path": [3, 3], "opacity": 255,
             "pixel": lambda x, y: (0, 255, 0, 128)},
            {"name": "Background", "size": (8, 6), "type": 0, "opacity": 255, "pixel": lambda x, y: (64, 64, 64)},
        ])
    write_xcf(
        os.path.join(out, "float32.xcf"), 14, 0, 600, ZLIB, "f", 4, 2,
        [
            {"name": "Glow", "size": (4, 2), "opacity": 191, "float_opacity": 0.75, "mode": 28,
             "pixel": lambda x, y: (2 + 0.5 * x, 0.5, -0.25, 0.5)},
            {"name": "Off", "size": (4, 2), "group": True, "hidden": True, "mode": 61,
             "mask": lambda x, y: 1.0, "mask_name": "Off Mask"},
            {"name": "Child", "size": (2, 2), "offset": (2, 0), "path": [1, 0], "mode": 28,
             "pixel": lambda x, y: (1.0, 1.0, 1.0, 1.0)},
        ])
    shade = (0x1000, 0xF000, 0x0100, 0x8000)
    write_xcf(
        os.path.join(out, "gray16.xcf"), 20, 1, 275, RLE, "H", 4, 2,
        [
            {"name": "Tint", "size": (4, 2), "type": 3, "mode": 28, "pixel": lambda x, y: (0x3333, 0xFFFF),
             "mask": lambda x, y: 0x8000},
            {"name": "Shade", "size": (4, 2), "type": 2, "mode": 28, "pixel": lambda x, y: (shade[x],)},
        ])
    write_xcf(
        os.path.join(out, "tiles.xcf"), 11, 0, 150, RLE, "B", 256, 64,
        [
            {"name": "Strip", "size": (256, 64), "mode": 28,
             "pixel": lambda x, y: (40 * (x // TILE + 1), (x + 3 * y) % 256 if y < 2 else 0, 0, 255)},
        ])
    colormap = u32(2) + bytes((0, 0, 0, 255, 255, 255))
    write_xcf(os.path.join(out, "indexed.xcf"), 3, 2, 0, RLE, "B", 2, 2, [],
              image_props=prop(PROP_COLORMAP, colormap))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
