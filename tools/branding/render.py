#!/usr/bin/env python3
"""Render branding SVGs to PNG.

  render.py svg <in.svg> --sizes 16,32,256 --bg '#1e1e1e' --out <dir>
  render.py concepts [--out build/branding/concepts]

Output is deterministic: PNGs carry no timestamps or metadata.
"""

import argparse
import io
import sys
from pathlib import Path

import cairosvg
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
CONCEPT_DIR = REPO / "Gui" / "Resources" / "Branding" / "concepts"
DEFAULT_CONCEPT_OUT = REPO / "build" / "branding" / "concepts"

BACKGROUNDS = {"dark": "#1e1e1e", "light": "#f0f0f0"}
LABEL_COLOUR = (136, 136, 136, 255)


def parse_colour(text):
    if text is None or text.lower() == "transparent":
        return None
    text = text.lstrip("#")
    if len(text) != 6:
        raise argparse.ArgumentTypeError("colour must be #rrggbb or 'transparent'")
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


def render_svg(svg_path, width=None, height=None, bg=None):
    png = cairosvg.svg2png(
        url=str(svg_path), output_width=width, output_height=height
    )
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    if bg is None:
        return img
    base = Image.new("RGBA", img.size, bg)
    base.alpha_composite(img)
    return base


def save_png(img, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    # Explicit settings and no pnginfo keep the bytes identical across runs.
    img.save(path, format="PNG", optimize=False, compress_level=9)


def cmd_svg(args):
    src = Path(args.input)
    bg = args.bg
    out = Path(args.out)
    for size in args.sizes:
        img = render_svg(src, width=size, height=size, bg=bg)
        save_png(img, out / f"{src.stem}-{size}.png")
    return 0


def label(draw, xy, text):
    draw.text(xy, text, fill=LABEL_COLOUR, font=ImageFont.load_default())


def make_sheet(concept, bg):
    pad, gap, label_h = 24, 32, 18
    lockup_w = 1024
    mark = CONCEPT_DIR / f"{concept}.svg"
    lockups = [(p.stem[len(concept) + 1:], render_svg(p, width=lockup_w, bg=bg))
               for p in list_lockups(concept)]

    tiles = []  # (caption, image)
    for size in (16, 32):
        true = render_svg(mark, size, size, bg)
        tiles.append((f"{size}px", true))
        tiles.append((f"{size}px x4", true.resize((size * 4, size * 4), Image.NEAREST)))
    tiles.append(("256px", render_svg(mark, 256, 256, bg)))

    row_h = max(t.height for _, t in tiles)
    row_w = sum(t.width for _, t in tiles) + gap * (len(tiles) - 1)
    width = max(lockup_w, row_w) + 2 * pad
    height = (pad + label_h + row_h
              + sum(gap + label_h + img.height for _, img in lockups) + pad)

    sheet = Image.new("RGBA", (width, height), bg)
    draw = ImageDraw.Draw(sheet)
    x = pad
    for caption, tile in tiles:
        label(draw, (x, pad), caption)
        sheet.alpha_composite(tile, (x, pad + label_h + (row_h - tile.height) // 2))
        x += tile.width + gap
    y = pad + label_h + row_h
    for caption, img in lockups:
        y += gap
        label(draw, (pad, y), f"{caption} {lockup_w}px")
        sheet.alpha_composite(img, (pad, y + label_h))
        y += label_h + img.height
    return sheet


def list_concepts():
    return sorted(p.stem for p in CONCEPT_DIR.glob("*.svg") if "-lockup" not in p.stem)


def list_lockups(concept):
    return sorted(CONCEPT_DIR.glob(f"{concept}-lockup*.svg"))


def cmd_concepts(args):
    out = Path(args.out)
    for concept in list_concepts():
        for name, hexcol in BACKGROUNDS.items():
            sheet = make_sheet(concept, parse_colour(hexcol))
            save_png(sheet, out / f"{concept}-{name}.png")
    return 0


def sizes_arg(text):
    try:
        return [int(s) for s in text.split(",") if s]
    except ValueError:
        raise argparse.ArgumentTypeError("sizes must be comma-separated integers")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("svg", help="render one SVG at several sizes")
    p.add_argument("input")
    p.add_argument("--sizes", type=sizes_arg, required=True)
    p.add_argument("--bg", type=parse_colour, default=None,
                   help="#rrggbb, or omit for transparent")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_svg)

    p = sub.add_parser("concepts", help="render concept comparison sheets")
    p.add_argument("--out", default=str(DEFAULT_CONCEPT_OUT))
    p.set_defaults(func=cmd_concepts)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
