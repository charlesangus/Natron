#!/usr/bin/env python3
"""Checks the committed shader manifests (Engine/Nodes/Scene/Shaders/manifest) and their layout
tables with tools/shaders/check_manifest.py. Needs only the host's python3, no renderer.
"""

import copy
import json
import os
import shutil
import sys
import tempfile
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_TOOLS = os.path.join(_REPO_ROOT, "tools", "shaders")
_MANIFESTS = os.path.join(_REPO_ROOT, "Engine", "Nodes", "Scene", "Shaders", "manifest")
sys.path.insert(0, _TOOLS)

import check_manifest  # noqa: E402
from generate_moonray_manifest import format_json  # noqa: E402

_FILES = ("cycles.json", "moonray.json", "cycles_layout.json", "moonray_layout.json")


def _load(name):
    with open(os.path.join(_MANIFESTS, name), encoding="utf-8") as f:
        return json.load(f)


def _node(manifest, node_id):
    return next(n for n in manifest["nodes"] if n["id"] == node_id)


class ShaderManifestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cycles = _load("cycles.json")
        cls.moonray = _load("moonray.json")

    def test_manifests_are_valid(self):
        for renderer, manifest in (("cycles", self.cycles), ("moonray", self.moonray)):
            self.assertEqual(check_manifest.validate_manifest(manifest, renderer), [])

    def test_files_are_in_canonical_layout(self):
        for name in _FILES:
            with open(os.path.join(_MANIFESTS, name), encoding="utf-8") as f:
                text = f.read()
            self.assertEqual(text, format_json(json.loads(text)) + "\n", name)

    def test_dwa_base_material(self):
        node = _node(self.moonray, "DwaBaseMaterial")
        self.assertEqual(node["kind"], "material")
        self.assertEqual(node["targets"], ["moonray"])
        self.assertGreaterEqual(len(node["attributes"]), 100)
        self.assertIn("Specular", node["groups"])
        grouped = [a for a in node["attributes"] if "group" in a]
        self.assertGreaterEqual(len(grouped), 100)
        self.assertEqual(set(a["group"] for a in grouped), set(node["groups"]))
        albedo = next(a for a in node["attributes"] if a["name"] == "albedo")
        self.assertEqual((albedo["type"], albedo["default"], albedo["linkable"]), ("Rgb", [1.0, 1.0, 1.0], True))
        normal = next(a for a in node["attributes"] if a["name"] == "input_normal")
        self.assertTrue(normal["port_only"])
        self.assertEqual(normal["interface"], "NormalMap")

    def test_moonray_has_no_display_filters_or_scene_classes(self):
        ids = [n["id"] for n in self.moonray["nodes"]]
        self.assertFalse([i for i in ids if i.endswith("DisplayFilter")])
        for absent in ("RectLight", "PerspectiveCamera", "RdlMeshGeometry", "DecayLightFilter"):
            self.assertNotIn(absent, ids)
        self.assertFalse([i for i in ids if i.startswith("Test")])
        for present in ("ImageMap", "ImageNormalMap", "NormalDisplacement", "VdbVolume", "UsdPreviewSurface"):
            self.assertIn(present, ids)

    def test_principled_bsdf(self):
        node = _node(self.cycles, "cycles_principled_bsdf")
        self.assertEqual(node["name"], "principled_bsdf")
        self.assertEqual(len(node["attributes"]), 33)
        self.assertEqual([o["name"] for o in node["outputs"]], ["BSDF"])
        base = next(a for a in node["attributes"] if a["name"] == "base_color")
        self.assertEqual((base["type"], base["default"], base["linkable"]), ("color", [0.8, 0.8, 0.8], True))
        distribution = next(a for a in node["attributes"] if a["name"] == "distribution")
        self.assertFalse(distribution["linkable"])
        self.assertIn(distribution["default"], [c["name"] for c in distribution["enum"]])

    def test_image_texture_outputs(self):
        node = _node(self.cycles, "cycles_image_texture")
        self.assertEqual([(o["ui_name"], o["type"]) for o in node["outputs"]], [("Color", "color"), ("Alpha", "float")])

    def test_cycles_has_no_osl_or_internal_nodes(self):
        names = [n["name"] for n in self.cycles["nodes"]]
        self.assertFalse([n for n in names if n.startswith("osl") or n.startswith("convert_")])
        for absent in ("output", "proxy", "set_normal", "mix_closure_weight"):
            self.assertNotIn(absent, names)
        self.assertTrue(all(n["id"] == "cycles_" + n["name"] for n in self.cycles["nodes"]))

    def test_check_manifest_passes(self):
        errors, other, _ = check_manifest.check(_MANIFESTS)
        self.assertEqual(errors, [])
        self.assertEqual(other, [])

    def test_principled_groups_cover_every_attribute(self):
        layout = _load("cycles_layout.json")["nodes"]["cycles_principled_bsdf"]
        grouped = [a for g in layout["groups"] for a in g["attributes"]]
        node = _node(self.cycles, "cycles_principled_bsdf")
        self.assertEqual(sorted(grouped), sorted(a["name"] for a in node["attributes"]))

    def _check_copy(self, edit):
        tmp = tempfile.mkdtemp(prefix="shader-manifest.")
        try:
            for name in _FILES:
                shutil.copy(os.path.join(_MANIFESTS, name), tmp)
            path = os.path.join(tmp, "cycles.json")
            manifest = _load("cycles.json")
            edit(manifest)
            with open(path, "w", encoding="utf-8") as f:
                f.write(format_json(manifest) + "\n")
            return check_manifest.check(tmp)
        finally:
            shutil.rmtree(tmp)

    def test_removed_attribute_makes_the_layout_stale(self):
        def drop_base_color(manifest):
            node = _node(manifest, "cycles_principled_bsdf")
            node["attributes"] = [a for a in node["attributes"] if a["name"] != "base_color"]

        errors, _, _ = self._check_copy(drop_base_color)
        self.assertTrue(any("base_color" in e and "cycles_principled_bsdf" in e for e in errors), errors)

    def test_new_attribute_falls_into_other(self):
        def add_socket(manifest):
            node = _node(manifest, "cycles_diffuse_bsdf")
            extra = copy.deepcopy(node["attributes"][0])
            extra["name"] = "new_socket"
            node["attributes"].append(extra)

        errors, other, _ = self._check_copy(add_socket)
        self.assertEqual(errors, [])
        self.assertEqual(other, [("cycles", "cycles_diffuse_bsdf", "new_socket")])


if __name__ == "__main__":
    unittest.main(verbosity=1)
