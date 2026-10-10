#!/usr/bin/env bash
#
# Run a spike binary natively on the host so it sees the real GPU (RADV).
# Build in the dev container as usual, then:
#
#   GPU_SPIKE_BUILD=<build dir> tools/gpu-spike/run-host.sh <target> [args...]
#
# <target> is a binary name found directly under the build dir, or a path.
# GPU_SPIKE_BUILD defaults to <repo-of-this-script>/build/gpu-spike.
# It refuses to run when no render node is openable, unless
# GPU_SPIKE_ALLOW_SOFTWARE=1 accepts the lavapipe/llvmpipe fallback.
#
# The container cannot use RADV: its libvulkan_radeon.so needs the AMDGPU
# target from the system libLLVM.so.21, but /usr/local carries a clang
# build of that library without it, so the loader drops the ICD and only
# lavapipe remains. Spike binaries are therefore built with static
# libstdc++/libgcc and depend only on libvulkan.so.1 and glibc <= 2.35.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <target> [args...]" >&2
    exit 2
fi

SPIKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
BUILD_DIR="${GPU_SPIKE_BUILD:-${SPIKE_DIR}/../../build/gpu-spike}"

target="$1"
shift
if [[ -x "${target}" && "${target}" == */* ]]; then
    bin="${target}"
elif [[ -x "${BUILD_DIR}/${target}" ]]; then
    bin="${BUILD_DIR}/${target}"
else
    echo "run-host.sh: '${target}' not found in ${BUILD_DIR} (set GPU_SPIKE_BUILD)" >&2
    exit 1
fi

# Without an openable render node the Vulkan loader and EGL silently fall
# back to lavapipe/llvmpipe, which would make RADV results meaningless.
render_ok=0
for node in /dev/dri/renderD*; do
    if [[ -r "${node}" && -w "${node}" ]]; then
        render_ok=1
        break
    fi
done
if [[ ${render_ok} -eq 0 ]]; then
    echo "run-host.sh: no /dev/dri/renderD* node is openable ($(ls -ln /dev/dri 2>&1 | tr '\n' ' '))" >&2
    if [[ "${GPU_SPIKE_ALLOW_SOFTWARE:-0}" != 1 ]]; then
        echo "run-host.sh: RADV unavailable; set GPU_SPIKE_ALLOW_SOFTWARE=1 to run on llvmpipe anyway" >&2
        exit 1
    fi
    echo "run-host.sh: WARNING: continuing on llvmpipe/lavapipe" >&2
fi

# RADV only exposes the dedicated SDMA transfer-only queue family when asked.
export RADV_PERFTEST="${RADV_PERFTEST:+${RADV_PERFTEST},}transfer_queue"

# The host has no gtest; the build stages the container's copy in hostlibs/.
if [[ -d "${BUILD_DIR}/hostlibs" ]]; then
    export LD_LIBRARY_PATH="${BUILD_DIR}/hostlibs${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

exec "${bin}" "$@"
