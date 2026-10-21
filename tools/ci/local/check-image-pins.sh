#!/usr/bin/env bash
# Fails when the Slang or glslang release baked into tools/ci/local/Dockerfile
# differs from the one its cmake/Fetch*.cmake pins, so the local image and CI
# never build against different shader toolchains. Reads files only; no
# network access.
#
# Usage: check-image-pins.sh [repo-root]

set -euo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
docker_file="${root}/tools/ci/local/Dockerfile"

cmake_value() {
    # Value may sit on the line after "set(NAME".
    tr '\n' ' ' <"${root}/cmake/$1" |
        sed -n "s/.*set( *$2 *\"\([^\"]*\)\".*/\1/p"
}

docker_value() {
    sed -n "s/^ARG $1=\(.*\)$/\1/p" "$docker_file" | head -n 1
}

status=0
check_pin() {
    local fetch_file="$1" name="$2" want have
    want="$(cmake_value "$fetch_file" "$name")"
    have="$(docker_value "$name")"
    if [ -z "$want" ] || [ -z "$have" ]; then
        echo "check-image-pins: could not read ${name} (cmake='${want}', Dockerfile='${have}')" >&2
        status=1
    elif [ "$want" != "$have" ]; then
        echo "check-image-pins: ${name} differs: cmake/${fetch_file}='${want}' tools/ci/local/Dockerfile='${have}'" >&2
        status=1
    fi
}

for name in NATRON_SLANG_VERSION NATRON_SLANG_SHA256; do
    check_pin FetchSlang.cmake "$name"
done
for name in NATRON_GLSLANG_VERSION NATRON_GLSLANG_SHA256; do
    check_pin FetchGlslang.cmake "$name"
done
exit "$status"
