#!/usr/bin/env python3
"""Audit the native RAW nodes against darktable's modules.

Every params field and described enum value of the darktable modules the RAW nodes port, colorin's
input-profile list, and the hand list of GUI-only controls in darktable-parity.json is an item.
darktable-parity.json maps each item to a node knob or option with darktable's default, to N/A with
a reason, or to DEFERRED. An item without an entry is MISSING.

Knobs and options a node has beyond darktable's are EXTRA, each with a reason in the mapping's
"extras". With --write-docs the script rewrites the "darktable controls" section of each node's
Engine/Nodes/Raw/docs/<node>.md from the same rows; without it a stale section is a problem.

Usage: python3 -I tools/raw/parity-audit.py [--darktable-ref DIR] [--mapping FILE] [--write-docs]

DARKTABLE_REF (environment variable or --darktable-ref) defaults to /tmp/dt-src-562, then
build/darktable-src. The exit status is non-zero on any MISSING row, on any mapped knob or option
that the sources under Engine/Nodes/Raw do not define, on a node knob neither mapped nor listed as
EXTRA, and on a stale docs section.
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
DOCS_DIR = os.path.join(NODE_DIR, "docs")
DOCS_HEADING = "## darktable controls"

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
    """Fields of the module's introspected params struct: [(name, type, default, description)]."""
    introspection = re.search(r"DT_MODULE_INTROSPECTION\(\s*\d+\s*,\s*(\w+)\s*\)", text)
    struct = introspection.group(1) if introspection else "dt_iop_%s_params_t" % module
    match = re.search(r"typedef struct %s\s*\{(.*?)\}\s*%s;" % (struct, struct), text, re.S)
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
        if not filename.startswith(stem):
            continue
        if not filename.endswith((".h", ".cpp", ".inc")):
            continue
        text = read(os.path.join(NODE_DIR, filename))
        literals.update(re.findall(r'"((?:[^"\\\n]|\\.)*)"', text))
        names.update(re.findall(r'#define\s+kRaw\w*(?:Param|Tab)\w*\s+"([^"]+)"', text))
        names.update(re.findall(r'setName\(\s*"([^"]+)"\s*\)', text))
    return literals, names


DENOISE_NAME = re.compile(r"^strength(All|Red|Green|Blue)(Coarsest|Coarse|Medium|Fine|Finest)$")
CALIBRATION_ROW_NAME = re.compile(r"^(red|green|blue|saturation|lightness|grey)(R|G|B|Normalize)$")


def knob_defined(module, knob, names, literals):
    if knob in names:
        return True
    # RawDenoise builds "strength" + channel + band at run time.
    if module == "rawdenoise" and DENOISE_NAME.match(knob):
        channel, band = DENOISE_NAME.match(knob).groups()
        return {"strength", channel, band} <= literals
    # RawColorCalibration names a row's knobs row + channel, and its toggle row + "Normalize".
    if module == "channelmixerrgb" and CALIBRATION_ROW_NAME.match(knob):
        row, suffix = CALIBRATION_ROW_NAME.match(knob).groups()
        return row in names and (suffix in literals or suffix in names)
    return False


# Name fragments the node composes into knob names (see knob_defined); they name no knob.
NAME_FRAGMENTS = {
    "channelmixerrgb": {"red", "green", "blue", "saturation", "lightness", "grey", "Normalize"},
}


def check_ref(module, knob, option, cache):
    if module not in cache:
        cache[module] = node_literals(module)
    literals, names = cache[module]
    if not knob_defined(module, knob, names, literals):
        return "knob '%s' is not defined in %s" % (knob, MODULES[module])
    if option and option not in literals:
        return "option '%s' of knob '%s' is not in %s" % (option, knob, MODULES[module])
    return None


def entry_refs(entry):
    return entry.get("targets") or [{"knob": entry.get("knob"), "option": entry.get("option")}]


def check_extras(mapping, cache):
    """The EXTRA rows [(module, knob, option, reason)] and their problems."""
    rows = []
    problems = []
    referenced = {}
    for key, entry in mapping.get("entries", {}).items():
        if entry.get("status") == "mapped":
            for ref in entry_refs(entry):
                referenced.setdefault(key.split("|")[0], set()).add(ref.get("knob"))
    extras = mapping.get("extras", {})
    listed = {}
    for key, reason in extras.items():
        module, _, target = key.partition("|")
        knob, _, option = target.partition("=")
        if module not in MODULES:
            problems.append("extras %s: no such module" % key)
            continue
        if not reason:
            problems.append("extras %s: EXTRA has no reason" % key)
        problem = check_ref(module, knob, option or None, cache)
        if problem:
            problems.append("extras %s: %s" % (key, problem))
        listed.setdefault(module, set()).add(knob)
        rows.append((module, knob, option or None, reason))
    for module in MODULES:
        if module not in cache:
            cache[module] = node_literals(module)
        _, names = cache[module]
        for name in sorted(names):
            if name in referenced.get(module, set()) or name in listed.get(module, set()):
                continue
            if name in NAME_FRAGMENTS.get(module, set()):
                continue
            problems.append("%s: knob '%s' of %s is neither mapped nor an EXTRA with a reason"
                            % (module, name, MODULES[module]))
    return rows, problems


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
    for ref in entry_refs(entry):
        knob = ref.get("knob")
        if not knob:
            problems.append("mapped without a knob")
            continue
        problem = check_ref(item.module, knob, ref.get("option"), cache)
        if problem:
            problems.append(problem)
        expect = ref.get("expect", entry.get("expect"))
        if isinstance(expect, str) and not problem:
            problem = check_ref(item.module, knob, expect, cache)
            if problem:
                problems.append("expected " + problem)
    return problems


def natron_text(entry, full=False):
    if entry.get("status") != "mapped":
        return "%s: %s" % (entry["status"], entry.get("reason", ""))
    refs = entry_refs(entry)
    parts = []
    for ref in refs:
        parts.append("%s=%s" % (ref["knob"], ref["option"]) if ref.get("option") else ref["knob"])
    if len(parts) > 3 and not full:
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
    return dt_default or ""


def value_text(value):
    if isinstance(value, bool):
        return "on" if value else "off"
    return str(value)


def natron_default_text(entry):
    """The node's default: the documented one, the image's, or the knobs' static 'expect'."""
    if not entry or entry.get("status") != "mapped":
        return ""
    if entry.get("natron_default") is not None:
        return str(entry["natron_default"])
    if entry.get("file"):
        return "from the image"
    if "expect" in entry:
        return value_text(entry["expect"])
    values = [value_text(ref["expect"]) for ref in entry_refs(entry) if "expect" in ref]
    return ", ".join(values)


def cell(text):
    return str(text).replace("|", "/")


def docs_section(module, items, extras):
    lines = [DOCS_HEADING, "",
             "Generated by `python3 -I tools/raw/parity-audit.py --write-docs` from",
             "`tools/raw/darktable-parity.json`: every control of darktable's `%s` and where it is here." % module,
             "",
             "| darktable item | Natron knob/option | darktable default | Natron default | status |",
             "| --- | --- | --- | --- | --- |"]
    for item in items:
        if item.module != module:
            continue
        natron = natron_text(item.entry, full=True) if item.entry else ""
        lines.append("| %s | %s | %s | %s | %s |" % (cell(item.label), cell(natron), cell(default_text(item)),
                                                    cell(natron_default_text(item.entry)), item.status))
    for extra_module, knob, option, reason in extras:
        if extra_module == module:
            lines.append("| | %s | | | EXTRA: %s |" % (cell("%s=%s" % (knob, option) if option else knob), cell(reason)))
    return "\n".join(lines) + "\n"


def splice_docs(text, section):
    """text with its darktable controls section, up to the next level-2 heading, replaced."""
    start = text.find("\n" + DOCS_HEADING + "\n")
    if start < 0:
        return text.rstrip("\n") + "\n\n" + section
    start += 1
    following = text.find("\n## ", start + len(DOCS_HEADING))
    rest = text[following + 1:] if following >= 0 else ""
    return text[:start] + section + ("\n" + rest if rest else "")


def sync_docs(items, extras, write):
    """The docs files whose section is stale; with write, rewrites them instead."""
    stale = []
    for module, stem in MODULES.items():
        path = os.path.join(DOCS_DIR, stem + ".md")
        if not os.path.isfile(path):
            stale.append("%s: no %s" % (module, os.path.relpath(path, REPO)))
            continue
        text = read(path)
        updated = splice_docs(text, docs_section(module, items, extras))
        if updated == text:
            continue
        if write:
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(updated)
        else:
            stale.append("%s: the darktable controls section is stale (run with --write-docs)"
                         % os.path.relpath(path, REPO))
    return stale


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--darktable-ref", default=None)
    parser.add_argument("--mapping", default=os.path.join(HERE, "darktable-parity.json"))
    parser.add_argument("--write-docs", action="store_true")
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
    extras, extra_problems = check_extras(mapping, cache)
    problems += extra_problems
    problems += sync_docs(items, extras, args.write_docs)

    print("darktable source: %s\n" % ref)
    print("| module | darktable item | Natron knob/option | darktable default | Natron default | status |")
    print("|---|---|---|---|---|---|")
    for item in items:
        natron = natron_text(item.entry) if item.entry else ""
        print("| %s | %s | %s | %s | %s | %s |" % (item.module, cell(item.label), cell(natron), cell(default_text(item)),
                                                   cell(natron_default_text(item.entry)), item.status))
    for module, knob, option, reason in extras:
        print("| %s | | %s | | | EXTRA: %s |" % (module, cell("%s=%s" % (knob, option) if option else knob), cell(reason)))

    print("\n| module | mapped | N/A | DEFERRED | MISSING | BAD | EXTRA |")
    print("|---|---|---|---|---|---|---|")
    total = OrderedDict((s, 0) for s in ("mapped", "N/A", "DEFERRED", "MISSING", "BAD", "EXTRA"))
    for module in MODULES:
        counts = OrderedDict((s, 0) for s in total)
        for item in items:
            if item.module == module:
                counts[item.status] += 1
        counts["EXTRA"] = sum(1 for row in extras if row[0] == module)
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
