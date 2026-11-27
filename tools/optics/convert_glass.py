#!/usr/bin/env python3
"""Convert the OpticsDatabase glass catalogues into Gui/Resources/Optics/glass.json.

usage: convert_glass.py <OpticsDatabase checkout> [--output <glass.json>]

The checkout must be at PINNED_COMMIT of https://github.com/amegahed/OpticsDatabase
(MIT; its .mtrl files come from the refractiveindex.info database, CC0). For each
vendor directory in VENDORS every .mtrl file with nd, Vd and a dispersion formula 1,
2 or 3 becomes one entry: vendor, name (the file stem), nd, Vd, formula and its
coefficients (wavelength in micrometres). Output is sorted and its numbers are
written from their parsed values, so a rerun on the same checkout is byte-identical.
"""
import argparse
import json
import os
import subprocess
import sys

PINNED_COMMIT = "1b007269eda014b9e4d23670d5d5d7dd2dfe6de5"
SOURCE_URL = "https://github.com/amegahed/OpticsDatabase"

# Output vendor name -> directory under Materials/.
VENDORS = {
    "CDGM": "Cdgm",
    "Hikari": "Hikari",
    "Hoya": "Hoya",
    "Ohara": "Ohara",
    "Schott": "Schott",
    "Sumita": "Sumita",
}


def parse_mtrl(path):
    """Return (nd, vd, formula, coefficients) or None.

    The files are YAML, but only three keys are needed and their layout is
    regular, so a line scan avoids a PyYAML dependency.
    """
    nd = vd = None
    formula = None
    coefficients = None
    pending_formula = None
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("- type:"):
                kind = line[len("- type:"):].strip()
                pending_formula = None
                if formula is None and kind in ("formula 1", "formula 2", "formula 3"):
                    pending_formula = int(kind.split()[1])
            elif line.startswith("coefficients:") and pending_formula is not None:
                values = [float(v) for v in line[len("coefficients:"):].split()]
                formula = pending_formula
                coefficients = values
                pending_formula = None
            elif line.startswith("nd:"):
                nd = float(line[3:].split()[0])
            elif line.startswith("Vd:"):
                vd = float(line[3:].split()[0])
    if nd is None or vd is None or formula is None or not coefficients:
        return None
    return nd, vd, formula, coefficients


def head_commit(checkout):
    try:
        out = subprocess.run(["git", "-C", checkout, "rev-parse", "HEAD"],
                             check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip()


def number(value):
    text = repr(float(value))
    return text[:-2] if text.endswith(".0") else text


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("checkout")
    parser.add_argument("--output", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "Gui", "Resources", "Optics", "glass.json"))
    args = parser.parse_args()

    commit = head_commit(args.checkout)
    if commit != PINNED_COMMIT:
        sys.stderr.write("convert_glass.py: %s is at %s, expected %s\n" % (args.checkout, commit, PINNED_COMMIT))
        return 1

    entries = []
    for vendor, directory in sorted(VENDORS.items()):
        root = os.path.join(args.checkout, "Materials", directory)
        for filename in sorted(os.listdir(root)):
            if not filename.endswith(".mtrl"):
                continue
            parsed = parse_mtrl(os.path.join(root, filename))
            if parsed is None:
                continue
            nd, vd, formula, coefficients = parsed
            entries.append((vendor, filename[:-len(".mtrl")], nd, vd, formula, coefficients))

    lines = [
        "{",
        '"source": %s,' % json.dumps("%s at %s (MIT), data from refractiveindex.info (CC0)" % (SOURCE_URL, PINNED_COMMIT)),
        '"glasses": [',
    ]
    for i, (vendor, name, nd, vd, formula, coefficients) in enumerate(entries):
        lines.append('{"vendor": %s, "name": %s, "nd": %s, "vd": %s, "formula": %d, "coefficients": [%s]}%s' % (
            json.dumps(vendor), json.dumps(name), number(nd), number(vd), formula,
            ", ".join(number(c) for c in coefficients), "," if i + 1 < len(entries) else ""))
    lines += ["]", "}", ""]

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    sys.stdout.write("%d glasses written to %s\n" % (len(entries), os.path.normpath(args.output)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
