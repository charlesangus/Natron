#!/usr/bin/env python3
"""Check the shader manifests and their hand-written layout tables.

Fails when a manifest is malformed, or when cycles_layout.json or moonray_layout.json names a node
or attribute the manifest does not have, puts an attribute in two groups, or has a wrong key-port
list. Reports every Cycles attribute no group names: Natron puts those in an "Other" group.

With --diff COMMITTED REGENERATED it compares two manifests instead and names every node,
attribute and output that differs (tools/shaders/check_manifest_drift.sh uses this).

The format is described in Engine/Nodes/docs/shader-manifest.md.
"""

import argparse
import json
import os
import sys

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DIR = os.path.join(_REPO_ROOT, "Engine", "Nodes", "Scene", "Shaders", "manifest")
RENDERERS = ("cycles", "moonray")
SCHEMA = 1
KEY_PORT_THRESHOLD = 12
OTHER_GROUP = "Other"
KINDS = {
    "cycles": {"shader", "texture", "displacement", "utility"},
    "moonray": {"material", "map", "normal_map", "displacement", "volume"},
}


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def load(directory, renderer):
    manifest = load_json(os.path.join(directory, renderer + ".json"))
    layout = load_json(os.path.join(directory, renderer + "_layout.json"))
    return manifest, layout


def ports(node):
    return [a for a in node["attributes"] if a["linkable"]]


def validate_manifest(manifest, renderer):
    """Errors in the manifest's own structure."""
    errors = []
    if manifest.get("schema") != SCHEMA:
        errors.append("%s: schema is %r, expected %d" % (renderer, manifest.get("schema"), SCHEMA))
    if manifest.get("renderer") != renderer:
        errors.append("%s: renderer is %r" % (renderer, manifest.get("renderer")))
    if not isinstance(manifest.get("source"), dict) or not manifest["source"].get("commit"):
        errors.append("%s: source has no commit" % renderer)
    nodes = manifest.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        return errors + ["%s: no nodes" % renderer]
    ids = [n.get("id") for n in nodes]
    if ids != sorted(ids):
        errors.append("%s: nodes are not sorted by id" % renderer)
    if len(set(ids)) != len(ids):
        errors.append("%s: duplicate node ids" % renderer)
    for node in nodes:
        where = "%s %s" % (renderer, node.get("id"))
        for key in ("id", "name", "kind", "targets", "attributes", "outputs"):
            if key not in node:
                errors.append("%s: no %s" % (where, key))
        if node.get("kind") not in KINDS[renderer]:
            errors.append("%s: kind %r" % (where, node.get("kind")))
        if renderer not in node.get("targets", []):
            errors.append("%s: targets do not include %s" % (where, renderer))
        names = [a.get("name") for a in node.get("attributes", [])]
        if len(set(names)) != len(names):
            errors.append("%s: duplicate attribute names" % where)
        groups = node.get("groups")
        for attr in node.get("attributes", []):
            aw = "%s.%s" % (where, attr.get("name"))
            for key in ("name", "type", "linkable", "port_only", "array"):
                if key not in attr:
                    errors.append("%s: no %s" % (aw, key))
            if attr.get("port_only"):
                if not attr.get("linkable"):
                    errors.append("%s: port-only but not linkable" % aw)
                if "default" in attr:
                    errors.append("%s: port-only with a default" % aw)
            elif "default" not in attr:
                errors.append("%s: no default" % aw)
            for choice in attr.get("enum", []):
                if not isinstance(choice.get("name"), str) or not isinstance(choice.get("value"), int):
                    errors.append("%s: malformed enum entry %r" % (aw, choice))
            if "group" in attr and (groups is None or attr["group"] not in groups):
                errors.append("%s: group %r is not in the node's groups" % (aw, attr["group"]))
        outputs = [o.get("name") for o in node.get("outputs", [])]
        if len(set(outputs)) != len(outputs):
            errors.append("%s: duplicate output names" % where)
    return errors


def check_layout(manifest, layout, renderer):
    """Returns (errors, other): stale or wrong table entries, and (node, attribute) pairs that no
    Cycles group names."""
    errors = []
    other = []
    if layout.get("schema") != SCHEMA or layout.get("renderer") != renderer:
        errors.append("%s_layout: schema or renderer mismatch" % renderer)
    nodes = {n["id"]: n for n in manifest["nodes"]}
    entries = layout.get("nodes", {})
    for node_id in sorted(entries):
        if node_id not in nodes:
            errors.append("%s_layout: %s is not in the manifest" % (renderer, node_id))
    for node_id, node in sorted(nodes.items()):
        entry = entries.get(node_id, {})
        attrs = {a["name"]: a for a in node["attributes"]}
        where = "%s_layout %s" % (renderer, node_id)

        if renderer == "cycles":
            placed = set()
            group_names = set()
            for group in entry.get("groups", []):
                name = group.get("name", "")
                if not name or name == OTHER_GROUP or name in group_names:
                    errors.append("%s: bad or repeated group name %r" % (where, name))
                group_names.add(name)
                for attr in group.get("attributes", []):
                    if attr not in attrs:
                        errors.append("%s: group %s names %s, which the manifest does not have"
                                      % (where, name, attr))
                    elif attr in placed:
                        errors.append("%s: %s is in two groups" % (where, attr))
                    placed.add(attr)
            other.extend((node_id, a) for a in attrs if a not in placed)
        elif "groups" in entry:
            errors.append("%s: MoonRay groups come from the manifest" % where)

        port_count = len(ports(node))
        if "key_ports" not in entry:
            if port_count > KEY_PORT_THRESHOLD:
                errors.append("%s: %d ports and no key-port list" % (where, port_count))
            continue
        key_ports = entry["key_ports"]
        if port_count <= KEY_PORT_THRESHOLD:
            errors.append("%s: key-port list for a node with %d ports" % (where, port_count))
        if len(key_ports) > KEY_PORT_THRESHOLD:
            errors.append("%s: %d key ports, at most %d" % (where, len(key_ports), KEY_PORT_THRESHOLD))
        if len(set(key_ports)) != len(key_ports):
            errors.append("%s: repeated key port" % where)
        for name in key_ports:
            attr = attrs.get(name)
            if attr is None:
                errors.append("%s: key port %s, which the manifest does not have" % (where, name))
            elif not attr["linkable"]:
                errors.append("%s: key port %s is not linkable" % (where, name))
            elif attr["port_only"]:
                errors.append("%s: key port %s is port-only, so always a port" % (where, name))
    return errors, other


def check(directory):
    """Returns (errors, other, summary) for both renderers' files in directory."""
    errors = []
    other = []
    summary = []
    for renderer in RENDERERS:
        try:
            manifest, layout = load(directory, renderer)
        except (OSError, ValueError) as e:
            errors.append("%s: %s" % (renderer, e))
            continue
        manifest_errors = validate_manifest(manifest, renderer)
        errors.extend(manifest_errors)
        if manifest_errors:
            continue
        layout_errors, layout_other = check_layout(manifest, layout, renderer)
        errors.extend(layout_errors)
        other.extend((renderer,) + pair for pair in layout_other)
        summary.append("%s %d nodes, %d attributes" % (
            renderer, len(manifest["nodes"]), sum(len(n["attributes"]) for n in manifest["nodes"])))
    return errors, other, summary


def _describe(value):
    text = json.dumps(value, sort_keys=True)
    return text if len(text) <= 80 else text[:77] + "..."


def _diff_fields(where, old, new):
    out = []
    for key in sorted(set(old) | set(new)):
        if old.get(key) != new.get(key):
            out.append("%s: %s was %s, regenerated %s" % (
                where, key, _describe(old.get(key, "(absent)")), _describe(new.get(key, "(absent)"))))
    return out


def _diff_named(where, kind, old, new):
    old_by = {e["name"]: e for e in old}
    new_by = {e["name"]: e for e in new}
    out = ["%s: %s %s is gone" % (where, kind, n) for n in old_by if n not in new_by]
    out += ["%s: %s %s is new" % (where, kind, n) for n in new_by if n not in old_by]
    for name in old_by:
        if name in new_by:
            out += _diff_fields("%s %s %s" % (where, kind, name), old_by[name], new_by[name])
    if not out and [e["name"] for e in old] != [e["name"] for e in new]:
        out.append("%s: %s order changed" % (where, kind))
    return out


def diff_manifests(old, new):
    """Differences between a committed manifest and a regenerated one, naming nodes and attributes."""
    out = []
    for key in sorted(set(old) | set(new)):
        if key != "nodes" and old.get(key) != new.get(key):
            out.append("%s was %s, regenerated %s" % (key, _describe(old.get(key)), _describe(new.get(key))))
    old_nodes = {n["id"]: n for n in old.get("nodes", [])}
    new_nodes = {n["id"]: n for n in new.get("nodes", [])}
    out += ["node %s is gone" % i for i in sorted(old_nodes) if i not in new_nodes]
    out += ["node %s is new" % i for i in sorted(new_nodes) if i not in old_nodes]
    for node_id in sorted(old_nodes):
        if node_id not in new_nodes:
            continue
        a, b = old_nodes[node_id], new_nodes[node_id]
        where = "node %s" % node_id
        rest = [k for k in set(a) | set(b) if k not in ("attributes", "outputs")]
        out += _diff_fields(where, {k: a.get(k) for k in rest if k in a}, {k: b.get(k) for k in rest if k in b})
        out += _diff_named(where, "attribute", a.get("attributes", []), b.get("attributes", []))
        out += _diff_named(where, "output", a.get("outputs", []), b.get("outputs", []))
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest-dir", default=DEFAULT_DIR,
                        help="directory holding the manifests and layouts (default: the committed ones)")
    parser.add_argument("--strict", action="store_true",
                        help='also fail when an attribute falls into "Other"')
    parser.add_argument("--diff", nargs=2, metavar=("COMMITTED", "REGENERATED"),
                        help="compare two manifests instead, and fail when they differ")
    args = parser.parse_args(argv)

    if args.diff:
        changes = diff_manifests(load_json(args.diff[0]), load_json(args.diff[1]))
        for change in changes:
            print("check_manifest: %s: %s" % (os.path.basename(args.diff[0]), change), file=sys.stderr)
        if changes:
            print("check_manifest: %s differs from the renderer (%d changes)"
                  % (args.diff[0], len(changes)), file=sys.stderr)
            return 1
        print("check_manifest: %s matches the renderer" % args.diff[0])
        return 0

    errors, other, summary = check(args.manifest_dir)
    for renderer, node_id, attr in other:
        print('check_manifest: %s %s.%s is in no group and falls into "%s"'
              % (renderer, node_id, attr, OTHER_GROUP))
    for error in errors:
        print("check_manifest: error: %s" % error, file=sys.stderr)
    if errors or (args.strict and other):
        print("check_manifest: FAILED (%d errors, %d in Other)" % (len(errors), len(other)), file=sys.stderr)
        return 1
    print("check_manifest: OK (%s; %d in Other)" % ("; ".join(summary), len(other)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
