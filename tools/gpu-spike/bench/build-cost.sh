#!/usr/bin/env bash
#
# Measure what the GPU spike costs to build and ship: Slang download and
# configure time, slangc time per kernel and target, embedded SPIR-V size,
# binary sizes and their runtime library dependencies.
#
# Usage (inside the dev container, from anywhere):
#   tools/gpu-spike/bench/build-cost.sh [jobs]
#
# Uses fresh build dirs build/gpu-spike-cost-<stamp>-{cold,cached} and a
# throwaway NATRON_DEPS_CACHE, so the Slang download is always measured and
# the user's real cache is untouched. Builds run at -j<jobs> (default 4) to
# stay out of the way of CPU benchmarks on the same host.

set -euo pipefail

JOBS="${1:-4}"
SPIKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SPIKE_DIR}/../.." >/dev/null 2>&1 && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
COLD_DIR="${REPO_ROOT}/build/gpu-spike-cost-${STAMP}-cold"
CACHED_DIR="${REPO_ROOT}/build/gpu-spike-cost-${STAMP}-cached"
DEPS_CACHE="${REPO_ROOT}/build/gpu-spike-cost-${STAMP}-deps"

now() { echo "${EPOCHREALTIME/./}"; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", (b - a) / 1e6 }'; }
mib() { awk -v b="$1" 'BEGIN { printf "%.1f MiB", b / 1048576 }'; }
kib() { awk -v b="$1" 'BEGIN { printf "%.1f KiB", b / 1024 }'; }

echo "== host =="
echo "date:    $(date -u +%FT%TZ)"
echo "uptime:  $(uptime)"
echo "cpus:    $(nproc)  build jobs: -j${JOBS}"
echo "build:   ${COLD_DIR}"
echo

configure() {
    local dir="$1"
    NATRON_DEPS_CACHE="${DEPS_CACHE}" cmake -S "${SPIKE_DIR}" -B "${dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release >"${dir}.configure.log" 2>&1
}

echo "== Slang download and configure =="
t0="$(now)"
configure "${COLD_DIR}"
t1="$(now)"
echo "configure, cold (empty NATRON_DEPS_CACHE, downloads Slang): $(secs "$t0" "$t1") s"

tarball="$(ls "${DEPS_CACHE}"/slang-*-linux-x86_64.tar.gz)"
tar_bytes="$(stat -c %s "${tarball}")"
echo "tarball: $(basename "${tarball}")  ${tar_bytes} bytes ($(mib "${tar_bytes}"))"
slang_root="$(ls -d "${DEPS_CACHE}"/slang-*/ | head -n1)"
slang_root="${slang_root%/}"
echo "extracted: $(du -sb "${slang_root}" | cut -f1) bytes ($(mib "$(du -sb "${slang_root}" | cut -f1)"))"

t0="$(now)"
configure "${CACHED_DIR}"
t1="$(now)"
echo "configure, cached (Slang already extracted): $(secs "$t0" "$t1") s"
echo

echo "== build (ninja -j${JOBS}) =="
t0="$(now)"
cmake --build "${COLD_DIR}" -j"${JOBS}" >"${COLD_DIR}.build.log" 2>&1
t1="$(now)"
echo "full build wall time: $(secs "$t0" "$t1") s"
echo

echo "== slangc time per kernel and target (single run each) =="
printf '%-18s %-8s %10s  %s\n' kernel target seconds source
scratch="$(mktemp -d)"
trap 'rm -rf "${scratch}"' EXIT
total_us=0
# ninja -t commands lists the exact slangc invocations the build runs;
# re-running them rewrites the same generated files with identical content.
(cd "${COLD_DIR}" && ninja -t commands) | grep -F "/bin/slangc" | grep -v -F -- "-help" > "${scratch}/cmds" || true
while IFS= read -r cmd; do
    target="$(sed -n 's/.* -target \([a-z]*\) .*/\1/p' <<<"${cmd}")"
    entry="$(sed -n 's/.* -entry \([A-Za-z0-9_]*\) .*/\1/p' <<<"${cmd}")"
    src="$(grep -o '[^/ ]*\.slang' <<<"${cmd}" | head -n1)"
    # Strip the "cd <dir> && " prefix ninja adds when the command has one.
    run="${cmd#*&& }"
    s="$(now)"
    (cd "${COLD_DIR}" && eval "${run}") >/dev/null 2>&1
    e="$(now)"
    total_us=$((total_us + e - s))
    printf '%-18s %-8s %10s  %s\n' "${entry}" "${target}" "$(secs "$s" "$e")" "${src}"
done < "${scratch}/cmds"
echo "total slangc: $(secs 0 "${total_us}") s over $(wc -l < "${scratch}/cmds") invocations"
echo

echo "== embedded SPIR-V per kernel =="
printf '%-18s %10s %10s\n' kernel spv_bytes header_bytes
spv_total=0
for spv in "${COLD_DIR}"/gen/*/*.spv; do
    name="$(basename "${spv}" .spv)"
    b="$(stat -c %s "${spv}")"
    h="$(stat -c %s "$(dirname "${spv}")/${name}_spirv.h")"
    spv_total=$((spv_total + b))
    printf '%-18s %10s %10s\n' "${name}" "${b}" "${h}"
done
echo "total SPIR-V: ${spv_total} bytes ($(kib "${spv_total}"))"
echo

echo "== spike binaries and runtime NEEDED libraries =="
expected='^(libvulkan\.so\.1|libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libgcc_s\.so\.1|libstdc\+\+\.so\.6|ld-linux-x86-64\.so\.2)$'
flagged=0
while IFS= read -r bin; do
    name="$(basename "${bin}")"
    size="$(stat -c %s "${bin}")"
    needed="$(readelf -d "${bin}" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
    printf '%-20s %10s bytes (%s)\n' "${name}" "${size}" "$(kib "${size}")"
    printf '    NEEDED: %s\n' "${needed:-<none>}"
    for lib in ${needed}; do
        if [[ ! "${lib}" =~ ${expected} ]]; then
            flagged=1
            case "${lib}" in
                libEGL*|libGL*|libOpenGL*) why="GL interop tests only" ;;
                libQt*) why="Qt variant only" ;;
                libgtest*) why="test harness only, not a runtime dependency" ;;
                *) why="unexpected" ;;
            esac
            printf '    FLAG: %s (%s)\n' "${lib}" "${why}"
        fi
    done
done < <(find "${COLD_DIR}" -maxdepth 1 -type f -executable | sort)
[[ "${flagged}" -eq 1 ]] || echo "no unexpected NEEDED entries"
if [[ ! -x "${COLD_DIR}/GlInteropQt" ]]; then
    echo "note: GlInteropQt was not built (Qt6 OpenGLWidgets not found in this environment)"
fi
echo

echo "== packaging notes for tools/ci/local/package.sh =="
cat <<'NOTES'
- Runtime needs only the Vulkan loader (libvulkan.so.1) and a Vulkan ICD
  (Mesa RADV/ANV/lavapipe or a vendor driver); both come from the user's
  system and must not be bundled.
- Nothing Slang-related ships: slangc runs at build time, SPIR-V and the
  Slang C++ output are compiled into the binary, and libslang is never linked.
- Licenses: Slang is Apache-2.0 WITH LLVM-exception and
  VulkanMemoryAllocator is MIT; both are compatible with distributing under
  Natron's GPLv2+.
- package.sh needs no change: it stages build/<type> through
  stage-bundle.sh and the spike is built separately by tools/gpu-spike, so it
  never enters the bundle. If the spike is later linked into Natron, the
  bundle's library exclusion list must keep libvulkan.so.1 out of the AppImage.
NOTES
echo

echo "== CI time =="
echo "n/a (spike is local-only)"
echo

echo "== load at end =="
uptime
echo "leaving build dirs: ${COLD_DIR} ${CACHED_DIR}; deps cache: ${DEPS_CACHE}"
