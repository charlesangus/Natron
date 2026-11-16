#!/usr/bin/env bash
# Builds librendering_pbr.so from the image's pinned MoonRay with moonray-true-deep.patch applied,
# then reruns the transparency variants (and, with --probe, the hdMoonray probe) with the patched
# library bind-mounted over the image's copy. Only librendering_pbr changes: the patch touches the
# path integrator and the vector shade handler, both private to that library.
#
# The build runs tools/ci/local/build-moonray.sh with the image's pins (from the Dockerfile), edited
# to apply the patch and build the rendering_pbr target and its dependencies only. It needs network
# access for the ISPC release and the MoonRay sources.
#
#   build.sh [--skip-build] [--probe]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
image="${NATRON_DEV_IMAGE:-ghcr.io/charlesangus/natron-dev:6c97a956d295d586}"
work="$here/out/true-deep"
lib="$work/librendering_pbr.so"
skip_build=0
probe=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) skip_build=1 ;;
        --probe) probe=1 ;;
        *) echo "unknown argument $1"; exit 2 ;;
    esac
    shift
done
mkdir -p "$work"

dockerfile_arg() {
    sed -n "s/^ARG $1=//p" "$root/tools/ci/local/Dockerfile"
}

if [ "$skip_build" = 0 ]; then
    rm -f "${lib:?}"
    script="$work/build-moonray-pbr.sh"
    cp "$root/tools/ci/local/build-moonray.sh" "$script"
    # Each edit is checked so a change to build-moonray.sh fails here rather than building stock.
    edit() {
        grep -qF -- "$1" "$script" || { echo "build.sh: '$1' not found in build-moonray.sh"; exit 1; }
        python3 -I -c 'import sys; p,o,n=sys.argv[1:]; s=open(p).read(); open(p,"w").write(s.replace(o,n,1))' \
            "$script" "$1" "$2"
    }
    edit 'work="$(mktemp -d /tmp/moonray-build.XXXXXX)"' 'work="$TRUE_DEEP_WORK/build-tree"; mkdir -p "$work"'
    # The build tree persists under out/true-deep, so a rerun rebuilds incrementally; the source is
    # reset before the patch is applied again.
    edit '# USD sorts render delegates' 'git -C "${src}/moonray/moonray" checkout -q -- .
git -C "${src}/moonray/moonray" apply "$TRUE_DEEP_PATCH"
# USD sorts render delegates'
    edit 'cmake --build "$build" -j "$jobs" -- -k' 'cmake --build "$build" -j "$jobs" --target rendering_pbr'
    edit 'cmake --install "$build" --prefix "$prefix" >/dev/null' \
        'install -m 0755 "$(find "$build" -name librendering_pbr.so -type f | head -n 1)" "$TRUE_DEEP_WORK/librendering_pbr.so"; exit 0'

    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "${here}:${here}" -v "${root}/tools:${root}/tools:ro" \
        -e TRUE_DEEP_WORK="$work" -e TRUE_DEEP_PATCH="$here/moonray-true-deep.patch" \
        -e NATRON_MOONRAY_REPO="$(dockerfile_arg NATRON_MOONRAY_REPO)" \
        -e NATRON_MOONRAY_COMMIT="$(dockerfile_arg NATRON_MOONRAY_COMMIT)" \
        -e NATRON_MOONRAY_ISPC_VERSION="$(dockerfile_arg NATRON_MOONRAY_ISPC_VERSION)" \
        -e NATRON_MOONRAY_ISPC_SHA256="$(dockerfile_arg NATRON_MOONRAY_ISPC_SHA256)" \
        -e NATRON_MOONRAY_PREFIX="$work/unused-prefix" \
        --entrypoint bash "$image" "$script" 2>&1 \
        | grep -v -E 'CUDA|NVIDIA|^$|=====|license|Container|GPU|docs.nvidia' | tee "$work/build.log" | tail -n 40
fi
[ -f "$lib" ] || { echo "build.sh: no $lib; see $work/build.log"; exit 1; }

export MOONRAY_PBR_LIB="$lib"
"$here/variants.sh" --size 128 --pixel-samples 4 | tee "$work/variants.log"
if [ "$probe" = 1 ]; then
    "$here/run.sh" | tee "$work/probe.log"
fi
