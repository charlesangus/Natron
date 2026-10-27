#!/usr/bin/env python3
"""Regenerate Engine/Ai/data/ai_models.json from a darktable-ai release.

usage: update-manifest.py [--tag <release tag>] [--out <file>] [--check]

The manifest pins one release of github.com/darktable-org/darktable-ai: its
tag is recorded under "source", and every entry carries the .dtmodel asset's
download URL, sha256 and size from that release. The licences come from the
package itself: the weight and training-data licences from the model_card in
the package's config.json, read from the remote zip with HTTP range requests
so no package is downloaded whole, and the OSAID v1.0 classification from the
model card README at the tag.

darktable-ai ships some packages that hold several models (RealPLKSR and
BSRGAN at 2x and 4x, RawNIND's Bayer and linear variants). Natron lists each
model as its own entry, naming the package it comes from and the files in it
it uses, so a node picks a model by task. VARIANTS below is that list.

An entry whose weight licence is not on the GPL-3-compatible allow-list is
refused, as darktable-ai's selection criteria require.

Without --tag, the tag pinned in the existing --out file is used, so a plain
run reproduces the committed file. --check writes nothing and exits non-zero
when the committed file differs from what the script would write.

Set GITHUB_TOKEN to lift the GitHub API's anonymous rate limit.
"""

import argparse
import io
import json
import os
import re
import sys
import urllib.request
import zipfile

REPOSITORY = "darktable-org/darktable-ai"
SCHEMA = 1

# GPL-3.0-compatible licences, as SPDX identifiers.
WEIGHTS_LICENCE_ALLOW_LIST = {
    "Apache-2.0",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "CC0-1.0",
    "GPL-3.0",
    "GPL-3.0-only",
    "GPL-3.0-or-later",
    "ISC",
    "LGPL-3.0",
    "LGPL-3.0-only",
    "LGPL-3.0-or-later",
    "MIT",
    "MPL-2.0",
    "Zlib",
}

# (id, task, name, package, files, default). The order is the order the
# manifest lists them in, and so the order a task's models are offered in.
VARIANTS = [
    ("mask-object-segnext-b2hq", "mask-object", "SegNext ViT-B SAx2 HQ",
     "mask-object-segnext-b2hq", ["encoder.onnx", "decoder.onnx"], True),
    ("mask-object-sam21-tiny", "mask-object", "SAM 2.1 Hiera Tiny",
     "mask-object-sam21-tiny", ["encoder.onnx", "decoder.onnx"], False),
    ("mask-object-sam21-small", "mask-object", "SAM 2.1 Hiera Small",
     "mask-object-sam21-small", ["encoder.onnx", "decoder.onnx"], False),
    ("mask-object-sam21-base-plus", "mask-object", "SAM 2.1 Hiera Base Plus",
     "mask-object-sam21-base-plus", ["encoder.onnx", "decoder.onnx"], False),
    ("denoise-nind", "denoise", "NIND UNet",
     "denoise-nind", ["model.onnx"], True),
    ("denoise-nafnet", "denoise", "NAFNet SIDD",
     "denoise-nafnet", ["model.onnx"], False),
    ("rawdenoise-nind-bayer", "denoise-raw-bayer", "RawNIND UtNet2 Bayer",
     "rawdenoise-nind", ["model_bayer.onnx"], True),
    ("rawdenoise-nind-linear", "denoise-linear", "RawNIND UtNet2 Linear",
     "rawdenoise-nind", ["model_linear.onnx"], True),
    ("upscale-realplksr-x2", "upscale-2x", "RealPLKSR 2x",
     "upscale-realplksr", ["model_x2.onnx"], True),
    ("upscale-realplksr-x4", "upscale-4x", "RealPLKSR 4x",
     "upscale-realplksr", ["model_x4.onnx"], True),
    ("upscale-bsrgan-x2", "upscale-2x", "BSRGAN 2x",
     "upscale-bsrgan", ["model_x2.onnx"], False),
    ("upscale-bsrgan-x4", "upscale-4x", "BSRGAN 4x",
     "upscale-bsrgan", ["model_x4.onnx"], False),
]


def request(url, headers=None, method="GET"):
    headers = dict(headers or {})
    headers.setdefault("User-Agent", "Natron tools/ai/update-manifest.py")
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers, method=method))


def github_api(path):
    headers = {"Accept": "application/vnd.github+json"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    with request("https://api.github.com/" + path, headers) as r:
        return json.load(r)


class RemoteFile(io.RawIOBase):
    """A seekable read-only view of a remote file, fetched with HTTP range requests."""

    def __init__(self, url):
        super().__init__()
        with request(url, method="HEAD") as r:
            self.size = int(r.headers["Content-Length"])
            # Release assets redirect to a signed URL: range-read that directly.
            self.url = r.url
        self.pos = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_SET:
            self.pos = offset
        elif whence == io.SEEK_CUR:
            self.pos += offset
        else:
            self.pos = self.size + offset
        return self.pos

    def readinto(self, buffer):
        end = min(self.size, self.pos + len(buffer))
        if end <= self.pos:
            return 0
        with request(self.url, {"Range": "bytes=%d-%d" % (self.pos, end - 1)}) as r:
            data = r.read()
        buffer[:len(data)] = data
        self.pos += len(data)
        return len(data)


def package_contents(url, package):
    """The package's config.json and the names of its members, relative to its folder."""
    with zipfile.ZipFile(io.BufferedReader(RemoteFile(url), 1 << 16)) as archive:
        prefix = package + "/"
        names = [n[len(prefix):] if n.startswith(prefix) else n for n in archive.namelist()]
        config_name = prefix + "config.json" if prefix + "config.json" in archive.namelist() else "config.json"
        config = json.loads(archive.read(config_name))
    return config, names


def osaid(tag, package):
    url = "https://raw.githubusercontent.com/%s/%s/models/%s/README.md" % (REPOSITORY, tag, package)
    with request(url) as r:
        card = r.read().decode("utf-8")
    match = re.search(r"^\|\s*OSAID v1\.0\s*\|\s*([^|]+?)\s*\|", card, re.MULTILINE)
    if not match:
        sys.exit("%s: the model card at %s has no OSAID v1.0 row" % (package, url))
    return match.group(1)


def build(tag):
    release = github_api("repos/%s/releases/tags/%s" % (REPOSITORY, tag))
    assets = {a["name"]: a for a in release["assets"]}

    packages = {}
    for _, _, _, package, _, _ in VARIANTS:
        if package in packages:
            continue
        asset = assets.get(package + ".dtmodel")
        if asset is None:
            sys.exit("%s: release %s has no %s.dtmodel" % (package, tag, package))
        digest = asset.get("digest") or ""
        if not digest.startswith("sha256:"):
            sys.exit("%s: release %s gives no sha256 for %s.dtmodel" % (package, tag, package))
        config, names = package_contents(asset["browser_download_url"], package)
        card = config.get("model_card", {})
        packages[package] = {
            "url": asset["browser_download_url"],
            "sha256": digest[len("sha256:"):],
            "size": asset["size"],
            "weightsLicence": card.get("license", ""),
            "trainingDataLicence": card.get("training_data_license", ""),
            "osaid": osaid(tag, package),
            "cardUrl": "https://github.com/%s/blob/%s/models/%s/README.md" % (REPOSITORY, tag, package),
            "names": names,
        }

    models = []
    for model_id, task, name, package, files, default in VARIANTS:
        p = packages[package]
        if p["weightsLicence"] not in WEIGHTS_LICENCE_ALLOW_LIST:
            sys.exit("%s: weight licence %r is not on the GPL-3-compatible allow-list"
                     % (model_id, p["weightsLicence"]))
        for f in files:
            if f not in p["names"]:
                sys.exit("%s: %s.dtmodel at %s holds no %s" % (model_id, package, tag, f))
        models.append({
            "id": model_id,
            "task": task,
            "name": name,
            "package": package,
            "files": files,
            "url": p["url"],
            "sha256": p["sha256"],
            "size": p["size"],
            "weightsLicence": p["weightsLicence"],
            "trainingDataLicence": p["trainingDataLicence"],
            "osaid": p["osaid"],
            "cardUrl": p["cardUrl"],
            "default": default,
        })

    manifest = {
        "schema": SCHEMA,
        "source": {"repository": REPOSITORY, "tag": tag},
        "models": models,
    }
    return json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", help="darktable-ai release tag (default: the one pinned in --out)")
    parser.add_argument("--out", default=os.path.join(root, "Engine", "Ai", "data", "ai_models.json"))
    parser.add_argument("--check", action="store_true",
                        help="compare against --out instead of writing it")
    args = parser.parse_args()

    tag = args.tag
    if not tag:
        try:
            with open(args.out, encoding="utf-8") as f:
                tag = json.load(f)["source"]["tag"]
        except (OSError, ValueError, KeyError):
            sys.exit("no --tag given and %s pins none" % args.out)

    text = build(tag)
    if args.check:
        try:
            with open(args.out, encoding="utf-8") as f:
                current = f.read()
        except FileNotFoundError:
            current = None
        if current != text:
            print("%s is out of date for %s" % (args.out, tag), file=sys.stderr)
            return 1
        return 0

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    print("%s: %d models from %s %s" % (args.out, len(json.loads(text)["models"]), REPOSITORY, tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
