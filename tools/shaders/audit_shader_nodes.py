#!/usr/bin/env python3
"""Audits the shader nodes Natron builds from the committed manifests, and writes their docs.

What each manifest attribute must become is worked out here from the manifests and layout tables
alone: a knob (its script name, label, type, dimensions and default), a port, or both. Then:

- audit(app), run inside Natron (Tests/Scene/ShaderAudit_Test.cpp runs it), creates every shader
  of both renderers by ID and compares each node's knobs, ports and defaults with what its
  manifest entry asks for, returning one line per problem. An attribute without its knob or port
  is a "missing" problem; the audit passes with none of any kind.
- --docs writes one Markdown page per shader under Engine/Nodes/Scene/docs/shaders/<renderer>/,
  and --check-docs fails when a committed page differs from what the manifests give.

Run outside Natron, the script only writes or checks docs:

    python3 tools/shaders/audit_shader_nodes.py --docs
    python3 tools/shaders/audit_shader_nodes.py --check-docs
"""

import argparse
import json
import math
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MANIFEST_DIR = os.path.join(ROOT, "Engine", "Nodes", "Scene", "Shaders", "manifest")
DOCS_DIR = os.path.join(ROOT, "Engine", "Nodes", "Scene", "docs", "shaders")
RENDERERS = ("cycles", "moonray")
RENDERER_NAMES = {"cycles": "Cycles", "moonray": "MoonRay"}

CONTROLS_PAGE = "Controls"
OTHER_PAGE = "Other"

# Knob kinds, with the Python parameter classes each may be wrapped as.
PARAM_CLASSES = {
    "double": ("DoubleParam", "Double2DParam", "Double3DParam"),
    "int": ("IntParam", "Int2DParam", "Int3DParam"),
    "bool": ("BooleanParam",),
    "choice": ("ChoiceParam",),
    "color": ("ColorParam",),
    "string": ("StringParam",),
    "file": ("FileParam",),
    "parametric": ("ParametricParam",),
    "table": ("StringParam",),
}

# type -> (knob kind, dimensions); a None kind is a port without a knob.
CYCLES_TYPES = {
    "float": ("double", 1),
    "int": ("int", 1),
    "boolean": ("bool", 1),
    "enum": ("choice", 1),
    "vector": ("double", 3),
    "point": ("double", 3),
    "normal": ("double", 3),
    "color": ("color", 3),
    "string": ("string", 1),
    "closure": (None, 0),
    "array_float": ("parametric", 1),
    "array_color": ("parametric", 3),
    "array_vector": ("parametric", 3),
    "array_int": ("table", 1),
    "transform": ("double", 12),
}

MOONRAY_TYPES = {
    "Float": ("double", 1),
    "Int": ("int", 1),
    "Bool": ("bool", 1),
    "Rgb": ("color", 3),
    "Vec2f": ("double", 2),
    "Vec3f": ("double", 3),
    "Vec3d": ("double", 3),
    "Mat4d": ("double", 16),
    "String": ("string", 1),
    "SceneObject*": (None, 0),
    "SceneObjectVector": (None, 0),
    "FloatVector": ("table", 1),
    "IntVector": ("table", 1),
    "RgbVector": ("table", 1),
}

ACRONYMS = {
    "ao": "AO", "aov": "AOV", "aovs": "AOVs", "brdf": "BRDF", "bsdf": "BSDF", "bssrdf": "BSSRDF",
    "cpu": "CPU", "ggx": "GGX", "gpu": "GPU", "hdr": "HDR", "hdri": "HDRI", "hsl": "HSL",
    "hsv": "HSV", "id": "ID", "ids": "IDs", "ies": "IES", "ior": "IOR", "lod": "LOD", "ndf": "NDF",
    "osl": "OSL", "rgb": "RGB", "rgba": "RGBA", "srgb": "sRGB", "sss": "SSS", "udim": "UDIM",
    "uv": "UV", "uvw": "UVW", "xyz": "XYZ",
}

TOLERANCE = 1e-6


def label_from_script_name(name):
    """The label Natron gives a knob or node, as shaderLabelFromScriptName() in ShaderManifest.cpp."""
    words = []
    word = ""
    for i, c in enumerate(name):
        if c in "_. -":
            if word:
                words.append(word)
                word = ""
            continue
        if word:
            prev = word[-1]
            following_lower = i + 1 < len(name) and name[i + 1].islower()
            lower_to_upper = prev.islower() and c.isupper()
            run_ends = prev.isupper() and c.isupper() and following_lower
            digit_change = prev.isdigit() != c.isdigit()
            if lower_to_upper or run_ends or digit_change:
                words.append(word)
                word = ""
        word += c
    if word:
        words.append(word)
    out = []
    for w in words:
        acronym = ACRONYMS.get(w.lower())
        out.append(acronym if acronym else w[0].upper() + w[1:])
    return " ".join(out)


def knob_name(attribute):
    """The knob's script name, as NetworkNode::knobNameOf()."""
    name = re.sub(r"[^A-Za-z0-9]", "_", attribute)
    return "_" + name if name[:1].isdigit() else name


def numbers(value):
    if isinstance(value, list):
        return [n for item in value for n in numbers(item)]
    if isinstance(value, bool):
        return [1.0 if value else 0.0]
    if isinstance(value, (int, float)):
        return [float(value)]
    return []


def format_number(value):
    return "{:g}".format(value)


def table_text(value):
    return "\n".join(" ".join(format_number(n) for n in numbers(row)) for row in (value or []))


def load(renderer, manifest_dir=MANIFEST_DIR):
    with open(os.path.join(manifest_dir, renderer + ".json"), encoding="utf-8") as f:
        manifest = json.load(f)
    with open(os.path.join(manifest_dir, renderer + "_layout.json"), encoding="utf-8") as f:
        layout = json.load(f).get("nodes", {})
    return manifest, layout


def expected_attribute(renderer, attribute):
    """What Natron builds for one manifest attribute; "error" is set for a type it cannot map."""
    name = attribute["name"]
    kind_type = attribute["type"]
    value = attribute.get("default")
    types = CYCLES_TYPES if renderer == "cycles" else MOONRAY_TYPES
    ret = {
        "name": name,
        "knob": knob_name(name),
        "label": label_from_script_name(name),
        "type": kind_type,
        "linkable": bool(attribute.get("linkable")),
        "port_only": bool(attribute.get("port_only")),
        "error": None,
    }
    if kind_type not in types:
        ret["error"] = "unknown type " + kind_type
        ret["kind"] = None
        return ret
    kind, dims = types[kind_type]
    enum = attribute.get("enum") or []
    if renderer == "moonray" and kind_type == "Int" and enum:
        kind = "choice"
    if renderer == "moonray" and kind_type == "String" and "asset" in attribute:
        kind = "file"
    ret["kind"] = kind
    ret["dimensions"] = dims
    if kind in ("double", "int", "bool", "color"):
        defaults = numbers(value)[:dims]
        ret["defaults"] = defaults + [0.0] * (dims - len(defaults))
    elif kind == "choice":
        ret["choices"] = [option["name"] for option in enum]
        ret["default_choice"] = 0
        for i, option in enumerate(enum):
            if (renderer == "cycles" and option["name"] == value) or \
                    (renderer == "moonray" and float(option["value"]) == float(value or 0)):
                ret["default_choice"] = i
                break
    elif kind in ("string", "file"):
        ret["default_text"] = value if isinstance(value, str) else ""
    elif kind == "table":
        ret["default_text"] = table_text(value)
    return ret


def default_ports(node, node_layout):
    """The ports a new node starts with, as buildLayout() in ShaderManifest.cpp."""
    key_ports = node_layout.get("key_ports") or []
    if not key_ports:
        return [a["name"] for a in node["attributes"] if a.get("linkable")]
    return list(key_ports) + [a["name"] for a in node["attributes"] if a.get("port_only")]


def pages_of(renderer, node, node_layout):
    """Each attribute's page, as buildLayout() places it."""
    attributes = node["attributes"]
    if renderer == "cycles":
        listed = {}
        for group in node_layout.get("groups") or []:
            for name in group.get("attributes") or []:
                listed[name] = group["name"]
        fallback = OTHER_PAGE if node_layout.get("groups") else CONTROLS_PAGE
        return {a["name"]: listed.get(a["name"], fallback) for a in attributes}
    fallback = OTHER_PAGE if node.get("groups") else CONTROLS_PAGE
    return {a["name"]: a.get("group") or fallback for a in attributes}


def _close(a, b):
    return math.isclose(a, b, rel_tol=TOLERANCE, abs_tol=TOLERANCE)


def audit_knob(prefix, param, expected):
    problems = []
    kind = expected["kind"]
    class_name = type(param).__name__
    if class_name not in PARAM_CLASSES[kind]:
        return [prefix + ": knob is a {} where a {} knob is expected".format(class_name, kind)]
    if param.getLabel() != expected["label"]:
        problems.append(prefix + ": label {!r}, expected {!r}".format(param.getLabel(), expected["label"]))
    if kind in ("double", "int", "color", "parametric") and param.getNumDimensions() != expected["dimensions"]:
        problems.append(prefix + ": {} dimensions, expected {}".format(param.getNumDimensions(), expected["dimensions"]))
        return problems
    if kind in ("double", "int", "color"):
        for dim, want in enumerate(expected["defaults"]):
            got = float(param.getDefaultValue(dim))
            if not _close(got, want):
                problems.append(prefix + ": default {} of dimension {}, expected {}".format(got, dim, want))
    elif kind == "bool":
        if bool(param.getDefaultValue()) != bool(expected["defaults"][0]):
            problems.append(prefix + ": default {}, expected {}".format(param.getDefaultValue(), bool(expected["defaults"][0])))
    elif kind == "choice":
        if param.getNumOptions() != len(expected["choices"]):
            problems.append(prefix + ": {} options, expected {}".format(param.getNumOptions(), len(expected["choices"])))
        if param.getDefaultValue() != expected["default_choice"]:
            problems.append(prefix + ": default option {}, expected {}".format(param.getDefaultValue(), expected["default_choice"]))
    elif kind in ("string", "file", "table"):
        if param.getDefaultValue() != expected["default_text"]:
            problems.append(prefix + ": default {!r}, expected {!r}".format(param.getDefaultValue(), expected["default_text"]))
    return problems


def audit_node(effect, renderer, node, node_layout):
    problems = []
    shader_id = node["id"]
    ports = list(effect.getPorts())
    port_attributes = set(effect.getPortAttributes())
    inputs = set(effect.getInputLabel(i) for i in range(effect.getMaxInputCount()))
    for attribute in node["attributes"]:
        expected = expected_attribute(renderer, attribute)
        prefix = "{} {}.{}".format(renderer, shader_id, expected["name"])
        if expected["error"]:
            problems.append("missing " + prefix + ": " + expected["error"])
            continue
        param = effect.getParam(expected["knob"])
        if expected["kind"] is None:
            if param is not None:
                problems.append(prefix + ": a port-only attribute has a knob")
            if expected["name"] not in ports or expected["name"] not in inputs:
                problems.append("missing " + prefix + ": no port")
        elif param is None:
            problems.append("missing " + prefix + ": no knob " + expected["knob"])
        else:
            problems += audit_knob(prefix, param, expected)
        if expected["linkable"] and expected["name"] not in port_attributes:
            problems.append("missing " + prefix + ": linkable but cannot be promoted")
        if not expected["linkable"] and expected["name"] in port_attributes:
            problems.append(prefix + ": not linkable but can be promoted")
    for port in default_ports(node, node_layout):
        if port not in ports:
            problems.append("missing {} {}.{}: not a port of a new node".format(renderer, shader_id, port))
    return problems


def audit(app, renderers=RENDERERS, manifest_dir=MANIFEST_DIR):
    """Creates every shader in app and returns one line per problem; "missing ..." lines name an
    attribute without its knob or port."""
    problems = []
    for renderer in renderers:
        manifest, layout = load(renderer, manifest_dir)
        for node in manifest["nodes"]:
            effect = app.createNode(node["id"])
            if effect is None:
                problems.append("missing {} {}: no node created".format(renderer, node["id"]))
                continue
            try:
                problems += audit_node(effect, renderer, node, layout.get(node["id"], {}))
            finally:
                effect.destroy()
    return problems


def _cell(text):
    return str(text).replace("|", "\\|").replace("\n", " ")


def _default_cell(expected, attribute):
    kind = expected["kind"]
    if kind is None:
        return ""
    if kind == "choice":
        return expected["choices"][expected["default_choice"]] if expected["choices"] else ""
    if kind == "bool":
        return "on" if expected["defaults"][0] else "off"
    if kind in ("double", "int", "color"):
        return " ".join(format_number(n) for n in expected["defaults"])
    if kind in ("string", "file"):
        return "`{}`".format(expected["default_text"]) if expected["default_text"] else ""
    if kind == "table":
        rows = [r for r in expected["default_text"].split("\n") if r]
        return "{} rows".format(len(rows)) if rows else "empty"
    return "empty"


def _role_cell(expected, starts_as_port):
    if expected["kind"] is None:
        return "port only"
    if not expected["linkable"]:
        return "knob only"
    return "port" if starts_as_port else "can be a port"


def node_doc(renderer, node, node_layout):
    """The Markdown page of one shader."""
    renderer_name = RENDERER_NAMES[renderer]
    label = label_from_script_name(node["name"]) if renderer == "cycles" else node["name"]
    lines = ["# " + label, ""]
    targets = node.get("targets") or [renderer]
    lines.append("{} {} `{}`, for the {} target{}.".format(
        renderer_name, node.get("kind") or "shader", node["id"], ", ".join(targets),
        "s" if len(targets) > 1 else ""))
    if node.get("comment"):
        lines += ["", node["comment"].strip()]
    lines += ["", "Generated from the {} shader manifest by `tools/shaders/audit_shader_nodes.py`.".format(renderer_name)]

    outputs = node.get("outputs") or []
    if outputs:
        lines += ["", "## Outputs", "", "| Name | Type |", "| --- | --- |"]
        for output in outputs:
            lines.append("| {} | {} |".format(_cell(output["name"]), _cell(output.get("interface") or output["type"])))

    pages = pages_of(renderer, node, node_layout)
    starting = set(default_ports(node, node_layout))
    order = []
    if renderer == "cycles":
        order = [group["name"] for group in node_layout.get("groups") or []]
    else:
        order = list(node.get("groups") or [])
    for page in pages.values():
        if page not in order:
            order.append(page)
    for page in order:
        attributes = [a for a in node["attributes"] if pages[a["name"]] == page]
        if not attributes:
            continue
        lines += ["", "## " + page, "",
                  "| Label | Script name | Type | Default | Port | Description |",
                  "| --- | --- | --- | --- | --- | --- |"]
        for attribute in attributes:
            expected = expected_attribute(renderer, attribute)
            description = attribute.get("comment") or attribute.get("ui_name") or ""
            if attribute.get("enum"):
                description = (description + " " if description else "") + "Options: " + \
                    ", ".join(option["name"] for option in attribute["enum"]) + "."
            lines.append("| {} | `{}` | {} | {} | {} | {} |".format(
                _cell(expected["label"]), expected["knob"] if expected["kind"] else attribute["name"], _cell(attribute["type"]),
                _cell(_default_cell(expected, attribute)),
                _role_cell(expected, attribute["name"] in starting), _cell(description)))
    return "\n".join(lines) + "\n"


def docs(manifest_dir=MANIFEST_DIR):
    """Every shader's page, as {relative path: text}."""
    ret = {}
    for renderer in RENDERERS:
        manifest, layout = load(renderer, manifest_dir)
        for node in manifest["nodes"]:
            ret[os.path.join(renderer, node["id"] + ".md")] = node_doc(renderer, node, layout.get(node["id"], {}))
    return ret


def mapping_problems(manifest_dir=MANIFEST_DIR):
    """The attributes no knob or port kind exists for: each would be missing from its node."""
    problems = []
    for renderer in RENDERERS:
        manifest, _ = load(renderer, manifest_dir)
        for node in manifest["nodes"]:
            for attribute in node["attributes"]:
                expected = expected_attribute(renderer, attribute)
                if expected["error"]:
                    problems.append("missing {} {}.{}: {}".format(renderer, node["id"], attribute["name"], expected["error"]))
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--manifest-dir", default=MANIFEST_DIR)
    parser.add_argument("--docs-dir", default=DOCS_DIR)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--docs", action="store_true", help="write the per-shader docs")
    action.add_argument("--check-docs", action="store_true", help="fail when a committed doc is stale")
    args = parser.parse_args(argv)

    problems = mapping_problems(args.manifest_dir)
    for problem in problems:
        print("audit_shader_nodes: " + problem, file=sys.stderr)
    pages = docs(args.manifest_dir)

    if args.docs:
        for renderer in RENDERERS:
            directory = os.path.join(args.docs_dir, renderer)
            os.makedirs(directory, exist_ok=True)
            for name in os.listdir(directory):
                if name.endswith(".md") and os.path.join(renderer, name) not in pages:
                    os.remove(os.path.join(directory, name))
        for path, text in sorted(pages.items()):
            with open(os.path.join(args.docs_dir, path), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        print("audit_shader_nodes: wrote {} pages".format(len(pages)))
        return 1 if problems else 0

    stale = []
    for path, text in sorted(pages.items()):
        try:
            with open(os.path.join(args.docs_dir, path), encoding="utf-8") as f:
                if f.read() != text:
                    stale.append(path)
        except OSError:
            stale.append(path)
    for renderer in RENDERERS:
        directory = os.path.join(args.docs_dir, renderer)
        if os.path.isdir(directory):
            for name in os.listdir(directory):
                if name.endswith(".md") and os.path.join(renderer, name) not in pages:
                    stale.append(os.path.join(renderer, name))
    for path in stale:
        print("audit_shader_nodes: stale doc " + path, file=sys.stderr)
    if stale or problems:
        return 1
    print("audit_shader_nodes: OK ({} pages)".format(len(pages)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
