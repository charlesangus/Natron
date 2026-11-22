#!/usr/bin/env bash
# Whether the two executables of a release agree on a content hash, which is what lets a Natron and
# a NatronRenderer reuse each other's disk-tier files. Takes an AppImage or an unpacked bundle.
#
#   tools/bench/cache_hash_pair.sh Natron-x86_64.AppImage
#   tools/bench/cache_hash_pair.sh build/stage
#
# The ctest case can only link a build tree's pair side by side; only this runs the shipped `.bin`s.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <AppImage or bundle directory>" >&2
    exit 2
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/cache-hash-pair.XXXXXX")
trap 'rm -rf -- "${scratch:?}"' EXIT

bundle=$1
if [ -f "$bundle" ]; then
    image=$(realpath "$bundle")
    (cd "$scratch" && "$image" --appimage-extract >/dev/null)
    bundle=$scratch/squashfs-root
fi

for name in Natron NatronRenderer; do
    if [ ! -x "$bundle/bin/$name" ]; then
        echo "no bin/$name in $bundle" >&2
        exit 2
    fi
done

cat >"$scratch/hash.py" <<'PY'
import os
source = app.createNode("Constant")
blur = app.createNode("Blur")
blur.connectInput(0, source)
blur.getParam("size").set(7.0, 7.0)
with open(os.environ["CACHE_HASH_FILE"], "w") as f:
    f.write("%x" % blur.getContentHash())
PY

run() {
    local name=$1
    local home=$scratch/home-$name
    mkdir -p "$home"
    # The exit code says nothing: a script without a writer ends in an error after it has done its work.
    env HOME="$home" XDG_CONFIG_HOME="$home/config" XDG_DATA_HOME="$home/data" \
        NATRON_CACHE_DISK_TIER=off CACHE_HASH_FILE="$scratch/$name.hash" \
        "$bundle/bin/$name" -b "$scratch/hash.py" >"$scratch/$name.log" 2>&1 || true
    if [ ! -s "$scratch/$name.hash" ]; then
        echo "$name wrote no hash:" >&2
        tail -n 20 "$scratch/$name.log" >&2
        exit 1
    fi
}

run Natron
run NatronRenderer

natron=$(cat "$scratch/Natron.hash")
renderer=$(cat "$scratch/NatronRenderer.hash")
echo "Natron         $natron"
echo "NatronRenderer $renderer"
if [ "$natron" != "$renderer" ]; then
    echo "the executables disagree: their files are not shared" >&2
    exit 1
fi
