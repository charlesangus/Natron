#!/usr/bin/env bash
# Natron Utility, Ray mode against Raster mode: an HD frame of a card carrying a projected plate, sliding and
# turning over an open shutter, at 36 samples a pixel in each mode (36 passes in Raster). Each mode is drawn once
# untimed, then three times; the times are the means. It prints one "[   BENCH  ]" line.
#
#   tools/bench/utility_raster_bench.sh [--tests build/release/Tests/Tests] [--device <name>]
#
# --device sets NATRON_GPU_DEVICE (default: the first hardware device, else llvmpipe).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
TESTS=$REPO/build/release/Tests/Tests

usage() { echo "usage: $0 [--tests <Tests binary>] [--device <name>]" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case $1 in
        --tests) TESTS=${2:-}; shift 2 ;;
        --device) export NATRON_GPU_DEVICE=${2:-}; shift 2 ;;
        *) usage ;;
    esac
done
[ -x "$TESTS" ] || { echo "no Tests binary at $TESTS" >&2; exit 1; }

NATRON_UTILITY_RASTER_BENCH=1 "$TESTS" --gtest_filter='NatronUtilityRaster.Bench*' | grep -E 'BENCH|SKIPPED|FAILED|Failure|OK \]'
