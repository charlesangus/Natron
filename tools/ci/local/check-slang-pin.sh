#!/usr/bin/env bash
# Fails when the Slang release baked into tools/ci/local/Dockerfile differs
# from the one cmake/FetchSlang.cmake pins, so the local image and CI never
# build against different compilers. Reads files only; no network access.
#
# Usage: check-slang-pin.sh [repo-root]

set -euo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
cmake_file="${root}/cmake/FetchSlang.cmake"
docker_file="${root}/tools/ci/local/Dockerfile"

cmake_value() {
    # Value may sit on the line after "set(NAME".
    tr '\n' ' ' <"$cmake_file" |
        sed -n "s/.*set( *$1 *\"\([^\"]*\)\".*/\1/p"
}

docker_value() {
    sed -n "s/^ARG $1=\(.*\)$/\1/p" "$docker_file" | head -n 1
}

status=0
for name in NATRON_SLANG_VERSION NATRON_SLANG_SHA256; do
    want="$(cmake_value "$name")"
    have="$(docker_value "$name")"
    if [ -z "$want" ] || [ -z "$have" ]; then
        echo "check-slang-pin: could not read ${name} (cmake='${want}', Dockerfile='${have}')" >&2
        status=1
    elif [ "$want" != "$have" ]; then
        echo "check-slang-pin: ${name} differs: cmake/FetchSlang.cmake='${want}' tools/ci/local/Dockerfile='${have}'" >&2
        status=1
    fi
done
exit "$status"
