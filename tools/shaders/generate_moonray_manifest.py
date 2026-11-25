#!/usr/bin/env python3
"""Generate Natron's MoonRay shader manifest (Engine/Nodes/Scene/Shaders/manifest/moonray.json).

The source is MoonRay's rdl2 class JSON: one file per rdl2 DSO, as rdl2_json_exporter writes it from
the DSO's attribute declarations with every include resolved. The image installs those files in
<moonray>/coredata/. With --from-dsos the files are exported afresh from <moonray>/rdl2dso/ instead,
so the manifest reflects the DSOs themselves; both give the same manifest.

Kept: materials, maps, normal maps, displacements and volume shaders. Display filters, lights,
cameras, geometry and the other scene classes are not shaders and are left out, and so are the
Test* classes MoonRay builds for its own unit tests.

The manifest format is described in Engine/Nodes/docs/shader-manifest.md.
"""

import argparse
import ast
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_OUTPUT = os.path.join(_REPO_ROOT, "Engine", "Nodes", "Scene", "Shaders", "manifest", "moonray.json")
DEFAULT_ROOT = "/opt/moonray"
SCHEMA = 1

# rdl2 base type of a scene class -> manifest kind.
KINDS = {
    "Material": "material",
    "DwaBaseLayerable": "material",
    "DwaBaseHairLayerable": "material",
    "Map": "map",
    "NormalMap": "normal_map",
    "Displacement": "displacement",
    "Volume": "volume",
}

# MoonRay's own test shaders (TestInputsMap, TestColorOutputMap, ...) are not user shaders.
TEST_CLASS_PREFIX = "Test"

SOURCE_SUBMODULES = ("moonray/moonray", "moonray/moonshine", "moonray/scene_rdl2")


def format_json(value, indent=0):
    """json.dumps(indent=2) layout, except that a list of scalars stays on one line."""
    pad = " " * (indent + 2)
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = ["%s%s: %s" % (pad, json.dumps(k, ensure_ascii=False), format_json(v, indent + 2))
                 for k, v in value.items()]
        return "{\n" + ",\n".join(items) + "\n" + " " * indent + "}"
    if isinstance(value, list):
        if not value:
            return "[]"
        if not any(isinstance(v, (dict, list)) for v in value):
            return "[" + ", ".join(format_json(v) for v in value) + "]"
        items = [pad + format_json(v, indent + 2) for v in value]
        return "[\n" + ",\n".join(items) + "\n" + " " * indent + "]"
    return json.dumps(value, ensure_ascii=False)


def read_source(root):
    """Repository and commits from <moonray>/SOURCE, which the image's build-moonray.sh writes."""
    path = os.path.join(root, "SOURCE")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    top = re.search(r"built from (\S+?), commit ([0-9a-f]{40})", text)
    if not top:
        raise SystemExit("generate_moonray_manifest: no repository and commit in %s" % path)
    source = {
        "description": "rdl2 class JSON exported from each rdl2 DSO by rdl2_json_exporter",
        "repository": top.group(1),
        "commit": top.group(2),
    }
    submodules = {}
    for line in text.splitlines():
        m = re.match(r"\s+(\S+): (\S+?), commit ([0-9a-f]{40})", line)
        if m and m.group(1) in SOURCE_SUBMODULES:
            submodules[m.group(1)] = {"repository": m.group(2), "commit": m.group(3)}
    missing = [s for s in SOURCE_SUBMODULES if s not in submodules]
    if missing:
        raise SystemExit("generate_moonray_manifest: %s does not name %s" % (path, ", ".join(missing)))
    source["submodules"] = {k: submodules[k] for k in SOURCE_SUBMODULES}
    return source


def export_from_dsos(root, out_dir):
    exporter = os.path.join(root, "bin", "rdl2_json_exporter")
    dso_dir = os.path.join(root, "rdl2dso")
    for proxy in sorted(glob.glob(os.path.join(dso_dir, "*.so.proxy"))):
        name = os.path.basename(proxy)[: -len(".so.proxy")]
        subprocess.run([exporter, "--dso_path", dso_dir, "--in", os.path.basename(proxy),
                        "--out", os.path.join(out_dir, name + ".json")],
                       cwd=out_dir, check=True, stdout=subprocess.DEVNULL)


def load_classes(json_dir):
    classes = {}
    versions = set()
    for path in sorted(glob.glob(os.path.join(json_dir, "*.json"))):
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        versions.add(data.get("moonray version", ""))
        for name, cls in data["scene_classes"].items():
            if name in classes:
                raise SystemExit("generate_moonray_manifest: %s is declared twice" % name)
            classes[name] = cls
    if not classes:
        raise SystemExit("generate_moonray_manifest: no class JSON in %s" % json_dir)
    return classes, sorted(versions)


def parse_condition(text):
    """MoonRay writes UI conditions as the repr of a Python OrderedDict."""
    m = re.fullmatch(r"OrderedDict\((.*)\)", text.strip(), re.S)
    try:
        value = ast.literal_eval(m.group(1) if m else text)
    except (ValueError, SyntaxError):
        return text
    if isinstance(value, dict):
        return {str(k): str(v) for k, v in value.items()}
    return text


def parse_number(text):
    try:
        return float(str(text).rstrip("fF"))
    except ValueError:
        return text


def is_binding(attr_type):
    return attr_type.startswith("SceneObject")


def is_array(attr_type):
    return attr_type.endswith("Vector") or attr_type.endswith("Indexable")


FORBIDDEN_NAMES = re.compile(r"\b(nuke|houdini|sidefx|foundry)\b", re.IGNORECASE)

# MoonRay comments that name another product, rewritten to neutral wording.
COMMENT_REWRITES = (
    ("in the style of Nuke where the pivot is at 0.18", "with the pivot at 0.18"),
)


def clean_comment(text):
    for old, new in COMMENT_REWRITES:
        text = text.replace(old, new)
    return text


def attribute_entry(name, attr, group):
    attr_type = attr["attrType"]
    meta = attr.get("metadata") or {}
    port_only = is_binding(attr_type)
    entry = {"name": name, "type": attr_type}
    if not port_only:
        entry["default"] = attr["default"]
    if "enum" in attr:
        entry["enum"] = [{"name": k, "value": v}
                         for k, v in sorted(attr["enum"].items(), key=lambda kv: (kv[1], kv[0]))]
    entry["linkable"] = bool(attr.get("bindable", False)) or port_only
    entry["port_only"] = port_only
    entry["array"] = is_array(attr_type)
    if group is not None:
        entry["group"] = group
    if meta.get("subgroup"):
        entry["subgroup"] = meta["subgroup"]
    if meta.get("comment"):
        entry["comment"] = clean_comment(meta["comment"])
    if "interface" in attr:
        entry["interface"] = attr["interface"]
    if attr.get("filename"):
        entry["asset"] = True
    for key in ("min", "max"):
        if key in meta:
            entry[key] = parse_number(meta[key])
    if attr.get("aliases"):
        entry["aliases"] = list(attr["aliases"])
    if "enable if" in meta:
        entry["enable_if"] = parse_condition(meta["enable if"])
    if "disable when" in meta:
        entry["disable_when"] = parse_condition(meta["disable when"])
    if "structure_type" in meta:
        entry["structure"] = {"type": meta["structure_type"],
                              "name": meta.get("structure_name", ""),
                              "path": meta.get("structure_path", "")}
    return entry


def node_entry(name, cls):
    attrs = cls.get("attributes") or {}
    grouping = cls.get("grouping") or {}
    structures = {}
    for attr_name, attr in attrs.items():
        structure = (attr.get("metadata") or {}).get("structure_name")
        if structure:
            structures.setdefault(structure, []).append(attr_name)
    group_of = {}
    for group, members in (grouping.get("groups") or {}).items():
        for member in members:
            # A group names a ramp by its structure name, which stands for the ramp's arrays.
            resolved = [member] if member in attrs else sorted(structures.get(member, []))
            if not resolved:
                raise SystemExit("generate_moonray_manifest: %s groups unknown attribute %s in %s"
                                 % (name, member, group))
            for attr_name in resolved:
                if attr_name in group_of:
                    raise SystemExit("generate_moonray_manifest: %s puts %s in two groups"
                                     % (name, attr_name))
                group_of[attr_name] = group
    ordered = sorted(attrs.items(), key=lambda kv: (kv[1].get("order", 0), kv[0]))
    entry = {
        "id": name,
        "name": name,
        "kind": KINDS[cls["type"]],
        "targets": ["moonray"],
    }
    if grouping.get("order"):
        entry["groups"] = list(grouping["order"])
    entry["attributes"] = [attribute_entry(n, a, group_of.get(n)) for n, a in ordered]
    entry["outputs"] = [{"name": "out", "type": "SceneObject*", "interface": cls["type"], "array": False}]
    return entry


def build_manifest(root, json_dir):
    classes, versions = load_classes(json_dir)
    source = read_source(root)
    source["moonray_version"] = ",".join(versions)
    source["generator"] = "tools/shaders/generate_moonray_manifest.py"
    nodes = [node_entry(n, c) for n, c in sorted(classes.items())
             if c.get("type") in KINDS and not n.startswith(TEST_CLASS_PREFIX)]
    return {"schema": SCHEMA, "renderer": "moonray", "source": source, "nodes": nodes}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--moonray-root", default=os.environ.get("NATRON_MOONRAY_ROOT", DEFAULT_ROOT),
                        help="MoonRay install prefix (default: %(default)s)")
    parser.add_argument("--from-dsos", action="store_true",
                        help="export the class JSON from rdl2dso/ instead of reading coredata/")
    parser.add_argument("--output", default=DEFAULT_OUTPUT,
                        help="manifest to write, or - for stdout (default: the committed manifest)")
    args = parser.parse_args(argv)

    if args.from_dsos:
        with tempfile.TemporaryDirectory(prefix="moonray-classes.") as tmp:
            export_from_dsos(args.moonray_root, tmp)
            manifest = build_manifest(args.moonray_root, tmp)
    else:
        manifest = build_manifest(args.moonray_root, os.path.join(args.moonray_root, "coredata"))

    offenders = ["%s.%s" % (n["id"], a["name"]) for n in manifest["nodes"] for a in n["attributes"]
                 if FORBIDDEN_NAMES.search(a.get("comment", ""))]
    if offenders:
        print("generate_moonray_manifest: comments name a product that docs must not mention; "
              "add a COMMENT_REWRITES entry for: " + ", ".join(offenders), file=sys.stderr)
        return 1

    text = format_json(manifest) + "\n"
    if args.output == "-":
        sys.stdout.write(text)
    else:
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    attrs = sum(len(n["attributes"]) for n in manifest["nodes"])
    print("generate_moonray_manifest: %d shaders, %d attributes" % (len(manifest["nodes"]), attrs),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
