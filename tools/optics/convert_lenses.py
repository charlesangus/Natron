#!/usr/bin/env python3
"""Convert the OpticsDatabase photographic lenses into Gui/Resources/Optics/lenses.zmx.gz.

usage: convert_lenses.py <OpticsDatabase checkout> <commit> --checker <check_lenses binary>
                         [--output <lenses.zmx.gz>]

The checkout must be at <commit> of https://github.com/amegahed/OpticsDatabase (MIT). Every
Optics/Photography/<Vendor>/**/*.zmx becomes one lens with the id "<Vendor>/<file stem>": the
file is decoded (UTF-16 or UTF-8) and cut down to the lines the .zmx parser in
Engine/Optics/ZemaxParser.cpp reads, NOTE included for attribution, under "#LENSID <id>" and
"#SOURCE <path in the checkout>" lines. The archive's first line is
"#OPTICSDATABASE <commit> <url>".

The checker (tools/optics/check_lenses.cpp, see its header for the build line) loads the archive
through the same code Natron uses and names every lens that fails to parse, has no paraxial focus
or parses differently from its original file; those are left out with a warning and the archive
is checked again. Lenses are sorted by id and the gzip header carries no name or time, so a rerun
on the same checkout is byte-identical.

The checkout is only read as data: nothing in it is executed or imported.
"""
import argparse
import gzip
import io
import os
import subprocess
import sys
import tempfile

SOURCE_URL = "https://github.com/amegahed/OpticsDatabase"
PINNED_COMMIT = "1b007269eda014b9e4d23670d5d5d7dd2dfe6de5"
PHOTOGRAPHY = os.path.join("Optics", "Photography")

# The keys ZemaxParser.cpp reads before the first surface, and inside one.
TOP_KEYS = {b"UNIT", b"NAME", b"NOTE", b"FNUM"}
SURFACE_KEYS = {b"TYPE", b"STOP", b"CURV", b"DISZ", b"GLAS", b"DIAM", b"CONI", b"PARM", b"XDAT"}


def decode(data):
    """The file as UTF-8 bytes, decoded the way ZemaxParser.cpp's decodeText() does."""
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:]
    if data.startswith(b"\xff\xfe"):
        return data[2:].decode("utf-16-le", "surrogatepass").encode("utf-8", "surrogatepass")
    if data.startswith(b"\xfe\xff"):
        return data[2:].decode("utf-16-be", "surrogatepass").encode("utf-8", "surrogatepass")
    if len(data) >= 2 and data[0] != 0 and data[1] == 0:
        return data[:len(data) // 2 * 2].decode("utf-16-le", "surrogatepass").encode("utf-8", "surrogatepass")
    if len(data) >= 2 and data[0] == 0 and data[1] != 0:
        return data[:len(data) // 2 * 2].decode("utf-16-be", "surrogatepass").encode("utf-8", "surrogatepass")
    return data


def strip(text):
    """The lines of a decoded .zmx the parser reads, in order.

    The parser ends a surface at the first unindented line, so the first unindented line after a
    surface that is otherwise dropped becomes an "END" line, which ends the surface at the same place.
    """
    kept = []
    in_surface = False
    for raw in text.split(b"\n"):
        line = raw[:-1] if raw.endswith(b"\r") else raw
        body = line.lstrip(b" \t")
        if not body:
            continue
        indented = len(body) < len(line)
        end = len(body)
        for sep in (b" ", b"\t"):
            at = body.find(sep)
            if at != -1:
                end = min(end, at)
        key = body[:end]
        if key == b"SURF":
            kept.append(line)
            in_surface = True
        elif not indented and in_surface:
            kept.append(line if key in TOP_KEYS else b"END")
            in_surface = False
        elif in_surface:
            if key in SURFACE_KEYS:
                kept.append(line)
        elif key in TOP_KEYS:
            kept.append(line)
    return kept


def head_commit(checkout):
    try:
        out = subprocess.run(["git", "-C", checkout, "rev-parse", "HEAD"],
                             check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip()


def collect(checkout):
    root = os.path.join(checkout, PHOTOGRAPHY)
    lenses = {}
    duplicates = []
    for vendor in sorted(os.listdir(root)):
        vendor_dir = os.path.join(root, vendor)
        if not os.path.isdir(vendor_dir) or os.path.islink(vendor_dir):
            continue
        for directory, subdirs, files in os.walk(vendor_dir):
            subdirs.sort()
            for filename in sorted(files):
                if not filename.lower().endswith(".zmx"):
                    continue
                path = os.path.join(directory, filename)
                if os.path.islink(path):
                    continue
                lens_id = "%s/%s" % (vendor, filename[:-len(".zmx")])
                if lens_id in lenses:
                    duplicates.append(lens_id)
                    continue
                with open(path, "rb") as f:
                    lines = strip(decode(f.read()))
                lenses[lens_id] = (os.path.relpath(path, checkout), lines)
    return lenses, duplicates


def archive(lenses, commit):
    out = [("#OPTICSDATABASE %s %s" % (commit, SOURCE_URL)).encode("utf-8")]
    for lens_id in sorted(lenses):
        source, lines = lenses[lens_id]
        out.append(("#LENSID %s" % lens_id).encode("utf-8"))
        out.append(("#SOURCE %s" % source.replace(os.sep, "/")).encode("utf-8"))
        out.extend(lines)
    raw = b"\n".join(out) + b"\n"
    buffer = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buffer, compresslevel=9, mtime=0) as f:
        f.write(raw)
    return buffer.getvalue()


def check(checker, data, checkout):
    with tempfile.TemporaryDirectory() as scratch:
        path = os.path.join(scratch, "lenses.zmx.gz")
        with open(path, "wb") as f:
            f.write(data)
        result = subprocess.run([checker, path, checkout], capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit("convert_lenses.py: the checker failed")
    excluded = {}
    for line in result.stdout.splitlines():
        parts = line.split("\t", 2)
        if len(parts) == 3 and parts[0] == "EXCLUDE":
            excluded[parts[1]] = parts[2]
    return excluded


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("checkout")
    parser.add_argument("commit")
    parser.add_argument("--checker", required=True)
    parser.add_argument("--output", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "Gui", "Resources", "Optics", "lenses.zmx.gz"))
    args = parser.parse_args()

    commit = head_commit(args.checkout)
    if commit != args.commit:
        sys.stderr.write("convert_lenses.py: %s is at %s, expected %s\n" % (args.checkout, commit, args.commit))
        return 1
    if commit != PINNED_COMMIT:
        sys.stderr.write("convert_lenses.py: warning: %s is not the pinned commit %s; update PINNED_COMMIT "
                         "and Gui/Resources/Optics/README.md\n" % (commit, PINNED_COMMIT))

    lenses, duplicates = collect(args.checkout)
    for lens_id in duplicates:
        sys.stderr.write("convert_lenses.py: warning: excluded %s: a second file with the same vendor and stem\n" % lens_id)
    found = len(lenses) + len(duplicates)

    excluded = check(args.checker, archive(lenses, commit), args.checkout)
    for lens_id in sorted(excluded):
        sys.stderr.write("convert_lenses.py: warning: excluded %s: %s\n" % (lens_id, excluded[lens_id]))
        lenses.pop(lens_id, None)
    data = archive(lenses, commit)
    again = check(args.checker, data, args.checkout)
    if again:
        for lens_id in sorted(again):
            sys.stderr.write("convert_lenses.py: %s still fails: %s\n" % (lens_id, again[lens_id]))
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(data)
    sys.stdout.write("%d of %d lenses written to %s (%d bytes)\n" % (
        len(lenses), found, os.path.normpath(args.output), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
