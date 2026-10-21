#!/usr/bin/env bash
# Prints the published dev image reference, ghcr.io/charlesangus/natron-dev:<tag>.
# The tag is the first 16 hex characters of a SHA-256 over tools/ci/local/Dockerfile
# followed by cmake/FetchSlang.cmake and cmake/FetchGlslang.cmake, the files
# that decide the image's contents. This is the only place the tag is defined; devshell.sh and the
# workflows call it, so they can never disagree about which image is current.
#
# Usage: dev-image-tag.sh [repo-root]

set -euo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"

tag="$(cat "${root}/tools/ci/local/Dockerfile" "${root}/cmake/FetchSlang.cmake" \
    "${root}/cmake/FetchGlslang.cmake" |
    sha256sum | cut -c1-16)"

echo "ghcr.io/charlesangus/natron-dev:${tag}"
