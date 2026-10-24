#!/usr/bin/env python3
"""Audit the native RAW nodes against darktable's modules.

Every params field and described enum value of the darktable modules the RAW nodes port, colorin's
input-profile list, and the hand list of GUI-only controls in darktable-parity.json is an item.
darktable-parity.json maps each item to a node knob or option with darktable's default, to N/A with
a reason, or to DEFERRED. An item without an entry is MISSING.

Usage: python3 -I tools/raw/parity-audit.py [--darktable-ref DIR] [--mapping FILE]

DARKTABLE_REF (environment variable or --darktable-ref) defaults to /tmp/dt-src-562, then
build/darktable-src. The exit status is non-zero on any MISSING row and on any mapped knob or option
that the sources under Engine/Nodes/Raw do not define.
"""

import argparse
import json
import os
import re
import sys
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
NODE_DIR = os.path.join(REPO, "Engine", "Nodes", "Raw")

# darktable module -> the node that ports it (file stem under Engine/Nodes/Raw)
MODULES = OrderedDict([
    ("rawprepare", "RawPrepare"),
    ("temperature", "RawTemperature"),
    ("highlights", "RawHighlights"),
    ("demosaic", "RawDemosaic"),
    ("colorin", "RawInputColor"),
    ("hotpixels", "RawHotPixels"),
    ("cacorrect", "RawCACorrect"),
    ("rawdenoise", "RawDenoise"),
    ("channelmixerrgb", "RawColorCalibration"),
])

# Headers whose described enums belong to a module besides its own .c file.
EXTRA_ENUM_FILES = {
    "channelmixerrgb": ["src/common/illuminants.h", "src/common/chromatic_adaptation.h"],
}

STATUSES = ("mapped", "N/A", "DEFERRED")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def find_ref(arg):
    candidates = [arg, os.environ.get("DARKTABLE_REF"), "/tmp/dt-src-562",
                  os.path.join(REPO, "build", "darktable-src")]
    for candidate in candidates:
        if candidate and os.path.isfile(os.path.join(candidate, "src", "iop", "rawprepare.c")):
            return candidate
    sys.exit("parity-audit: no darktable source tree found (set DARKTABLE_REF or --darktable-ref)")


class Item:
    def __init__(self, module, key, label, dt_default=None, enum_type=None, enum_value=None):
        self.module = module
        self.key = key
        self.label = label
        self.dt_default = dt_default
        self.enum_type = enum_type
        self.enum_value = enum_value
        self.entry = None
        self.status = "MISSING"


def parse_params(module, text):
    """Fields of dt_iop_<module>_params_t: [(name, type, default, description)]."""
    match = re.search(r"typedef struct dt_iop_%s_params_t\s*\{(.*?)\}\s*dt_iop_%s_params_t;" % (module, module),
                      text, re.S)
    if not match:
        return []
    fields = []
    for line in match.group(1).splitlines():
        code, _, comment = line.partition("//")
        code = re.sub(r"/\*.*?\*/", "", code).strip()
        if not code.endswith(";"):
            continue
        decl = re.match(r"^(?:struct\s+)?(\w+)\s+(.+);$", code)
        if not decl:
            continue
        ftype = decl.group(1)
        default = re.search(r"\$DEFAULT:\s*(\S+)", comment)
        description = re.search(r'\$DESCRIPTION:\s*"([^"]*)"', comment)
        for name in decl.group(2).split(","):
            name = re.sub(r"\[.*?\]", "", name).strip()
            if name:
                fields.append((name, ftype, default.group(1) if default else None,
                               description.group(1) if description else None))
    return fields


ENUM_RE = re.compile(r"typedef enum\s*(\w*)\s*\{(.*?)\}\s*(\w+);", re.S)
ENUM_VALUE_RE = re.compile(r'^\s*(\w+)\s*(?:=[^/]*?)?,?\s*//\s*\$DESCRIPTION:\s*"([^"]*)"')


def parse_enums(text):
    """Described enums: [(type, [(value, description)])]."""
    enums = []
    for match in ENUM_RE.finditer(text):
        values = []
        for line in match.group(2).splitlines():
            value = ENUM_VALUE_RE.match(line)
            if value:
                values.append((value.group(1), value.group(2)))
        if values:
            enums.append((match.group(3), values))
    return enums


def parse_colorin_profiles(ref):
    items = []
    colorspaces = read(os.path.join(ref, "src/common/colorspaces.c"))
    for name in re.findall(r'_\("([^"]+)"\),\s*\+\+in_pos', colorspaces):
        items.append(("input profile %s" % name, "input profile: %s" % name))
    colorin = read(os.path.join(ref, "src/iop/colorin.c"))
    # Only update_profile_list assigns prof->type.
    body = colorin
    seen = []
    for value in re.findall(r"prof->type\s*=\s*(DT_COLORSPACE_\w+)", body):
        if value not in seen:
            seen.append(value)
    for value in seen:
        items.append(("image profile %s" % value, "image profile: %s" % value))
    return items


def collect_items(ref, mapping):
    items = []
    for module in MODULES:
        text = read(os.path.join(ref, "src", "iop", module + ".c"))
        fields = parse_params(module, text)
        defaults_by_type = {}
        for name, ftype, default, description in fields:
            label = 'param %s%s' % (name, ' "%s"' % description if description else "")
            items.append(Item(module, "param " + name, label, dt_default=default))
            if default:
                defaults_by_type[ftype] = default
        enum_texts = [text]
        for extra in EXTRA_ENUM_FILES.get(module, []):
            enum_texts.append(read(os.path.join(ref, extra)))
        for enum_text in enum_texts:
            for etype, values in parse_enums(enum_text):
                for value, description in values:
                    is_default = defaults_by_type.get(etype) == value
                    items.append(Item(module, "enum " + value, 'enum %s "%s"' % (value, description),
                                      dt_default="default" if is_default else None,
                                      enum_type=etype, enum_value=value))
        if module == "colorin":
            for key, label in parse_colorin_profiles(ref):
                items.append(Item(module, key, label))
    for hand in mapping.get("gui_items", []):
        items.append(Item(hand["module"], hand["item"], hand["item"]))
    return items


def node_literals(module):
    """Every string literal in the node's sources, and the names it gives knobs."""
    stem = MODULES[module]
    literals = set()
    names = set()
    if not os.path.isdir(NODE_DIR):
        return literals, names
    for filename in sorted(os.listdir(NODE_DIR)):
        if not (filename.startswith(stem + ".") or filename.startswith(stem + "Kernel")):
            continue
        if not filename.endswith((".h", ".cpp", ".inc")):
            continue
        text = read(os.path.join(NODE_DIR, filename))
        literals.update(re.findall(r'"([^"\\]*)"', text))
        names.update(re.findall(r'#define\s+kRaw\w*Param\w*\s+"([^"]+)"', text))
        names.update(re.findall(r'setName\(\s*"([^"]+)"\s*\)', text))
    return literals, names


DENOISE_NAME = re.compile(r"^strength(All|Red|Green|Blue)(Coarsest|Coarse|Medium|Fine|Finest)$")


def knob_defined(module, knob, names, literals):
    if knob in names:
        return True
    # RawDenoise builds "strength" + channel + band at run time.
    if module == "rawdenoise" and DENOISE_NAME.match(knob):
        channel, band = DENOISE_NAME.match(knob).groups()
        return {"strength", channel, band} <= literals
    return False


def check_entry(item, entry, cache):
    """Problems with a mapped entry's knob and option references."""
    problems = []
    status = entry.get("status")
    if status not in STATUSES:
        return ["status %r is none of %s" % (status, "/".join(STATUSES))]
    if status != "mapped":
        if not entry.get("reason"):
            problems.append("%s has no reason" % status)
        return problems
    refs = entry.get("targets") or [{"knob": entry.get("knob"), "option": entry.get("option")}]
    if item.module not in cache:
        cache[item.module] = node_literals(item.module)
    literals, names = cache[item.module]
    for ref in refs:
        knob = ref.get("knob")
        option = ref.get("option")
        if not knob:
            problems.append("mapped without a knob")
            continue
        if not knob_defined(item.module, knob, names, literals):
            problems.append("knob '%s' is not defined in %s" % (knob, MODULES[item.module]))
        elif option and option not in literals:
            problems.append("option '%s' of knob '%s' is not in %s" % (option, knob, MODULES[item.module]))
    return problems


def natron_text(entry):
    if entry.get("status") != "mapped":
        return "%s: %s" % (entry["status"], entry.get("reason", ""))
    refs = entry.get("targets") or [{"knob": entry.get("knob"), "option": entry.get("option")}]
    parts = []
    for ref in refs:
        parts.append("%s=%s" % (ref["knob"], ref["option"]) if ref.get("option") else ref["knob"])
    if len(parts) > 3:
        parts = parts[:2] + ["... (%d knobs)" % len(refs)]
    text = ", ".join(parts)
    if entry.get("note"):
        text += " (%s)" % entry["note"]
    return text


def default_text(item):
    entry = item.entry
    dt_default = item.dt_default
    if entry and entry.get("default") is not None:
        dt_default = str(entry["default"])
    text = dt_default or ""
    if entry and entry.get("natron_default") is not None:
        text += " [Natron: %s]" % entry["natron_default"]
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--darktable-ref", default=None)
    parser.add_argument("--mapping", default=os.path.join(HERE, "darktable-parity.json"))
    args = parser.parse_args()

    ref = find_ref(args.darktable_ref)
    with open(args.mapping, encoding="utf-8") as handle:
        mapping = json.load(handle)
    entries = mapping.get("entries", {})

    items = collect_items(ref, mapping)
    cache = {}
    problems = []
    used = set()
    for item in items:
        entry = entries.get("%s|%s" % (item.module, item.key))
        if entry is None:
            continue
        used.add("%s|%s" % (item.module, item.key))
        item.entry = entry
        found = check_entry(item, entry, cache)
        for problem in found:
            problems.append("%s | %s: %s" % (item.module, item.key, problem))
        item.status = entry["status"] if not found else "BAD"
    for key in entries:
        if key not in used:
            problems.append("%s: no such darktable item (stale mapping)" % key)

    print("darktable source: %s\n" % ref)
    print("| module | darktable item | Natron knob/option | default | status |")
    print("|---|---|---|---|---|")
    for item in items:
        natron = natron_text(item.entry) if item.entry else ""
        print("| %s | %s | %s | %s | %s |" % (item.module, item.label.replace("|", "/"),
                                              natron.replace("|", "/"),
                                              default_text(item).replace("|", "/"), item.status))

    print("\n| module | mapped | N/A | DEFERRED | MISSING | BAD |")
    print("|---|---|---|---|---|---|")
    total = OrderedDict((s, 0) for s in ("mapped", "N/A", "DEFERRED", "MISSING", "BAD"))
    for module in MODULES:
        counts = OrderedDict((s, 0) for s in total)
        for item in items:
            if item.module == module:
                counts[item.status] += 1
        for status in total:
            total[status] += counts[status]
        print("| %s | %s |" % (module, " | ".join(str(counts[s]) for s in total)))
    print("| total | %s |" % " | ".join(str(total[s]) for s in total))

    if problems:
        print("\nProblems:")
        for problem in problems:
            print("- " + problem)
    return 1 if (total["MISSING"] or total["BAD"] or problems) else 0


if __name__ == "__main__":
    sys.exit(main())
