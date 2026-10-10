#!/usr/bin/env bash
#
# GPU spike benchmark: Grade, Blur (sigma 3, 25, 100) and Grade->Blur on RADV at UHD, 8K, 16k and
# 24k RGBA float, next to Natron's native Grade and Blur on the CPU at the same sizes.
#
#   tools/gpu-spike/run-bench.sh [--no-cpu] [--no-gpu] [--runs N] [--iters N] [--sizes uhd,8k,16k,24k[,24k-single]]
#
# Environment:
#   GPU_SPIKE_BUILD   build dir holding GpuBench (default <repo>/build/gpu-spike)
#   GPU_BENCH_OUT     results dir (default <repo>/build/gpu-bench)
#   NATRON_BUILD      Natron build dir with Renderer/NatronRenderer (default <main checkout>/build/release);
#                     its plugins are read from <main checkout>/build/assets/Plugins unless NATRON_PLUGINS is set
#   BUSY_CONTAINER    container whose build load the CPU baselines wait out (default natron-dev)
#   CPU_WAIT_MIN      minutes to wait for that container's build to finish (default 45)
#
# GpuBench runs on the host through run-host.sh, which exits 1 without an openable render node.
# The CPU baselines render generator-fed graphs with tools/bench/graph_bench.py in a throwaway
# container of the dev image, so no running container is touched except for the pgrep below.
# Each Grade/Blur cost is the marginal cost over a zero-node graph at the same size, which removes
# the generator and the EXR write.

set -euo pipefail

SPIKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SPIKE_DIR}/../.." && pwd)"
BUILD_DIR="${GPU_SPIKE_BUILD:-${REPO_ROOT}/build/gpu-spike}"
OUT="${GPU_BENCH_OUT:-${REPO_ROOT}/build/gpu-bench}"
BUSY_CONTAINER="${BUSY_CONTAINER:-natron-dev}"
CPU_WAIT_MIN="${CPU_WAIT_MIN:-45}"
IMAGE="${NATRON_DEV_IMAGE:-natron-dev:2027-clang21.1}"

RUNS=2
ITERS=3
SIZES="uhd,8k,16k,24k,24k-single"
DO_CPU=1
DO_GPU=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-cpu) DO_CPU=0 ;;
        --no-gpu) DO_GPU=0 ;;
        --runs) RUNS="$2"; shift ;;
        --iters) ITERS="$2"; shift ;;
        --sizes) SIZES="$2"; shift ;;
        *) echo "usage: $0 [--no-cpu] [--no-gpu] [--runs N] [--iters N] [--sizes list]" >&2; exit 2 ;;
    esac
    shift
done

mkdir -p "${OUT}/work"

# True while a compiler or build driver is alive in the build container. Defunct processes are
# ignored: they linger after a build ends and are not work in progress. The bracketed patterns keep
# the shell that runs the check from matching itself.
build_busy() {
    docker exec "${BUSY_CONTAINER}" sh -c '
        for p in $(pgrep -f "[n]inja|[c]c1plus|[c]lang"); do
            grep -q "^State:.*Z" "/proc/$p/status" 2>/dev/null || exit 0
        done
        exit 1' >/dev/null 2>&1
}

context() {
    if build_busy; then echo "build running in ${BUSY_CONTAINER}"; else echo "no build running"; fi
}

for r in $(seq 1 "${RUNS}"); do
    [[ "${DO_GPU}" -eq 1 ]] || break
    note="$(context); $(nproc) threads"
    echo "== GPU run ${r}/${RUNS} (${note})"
    GPU_SPIKE_BUILD="${BUILD_DIR}" "${SPIKE_DIR}/run-host.sh" GpuBench \
        --sizes "${SIZES}" --iters "${ITERS}" --csv "${OUT}/gpu-run${r}.csv" --note "${note}" \
        | tee "${OUT}/gpu-run${r}.log"
done

cpu_status="skipped (--no-cpu)"
[[ -f "${OUT}/cpu-status.txt" && "${DO_CPU}" -eq 0 ]] && cpu_status="$(cat "${OUT}/cpu-status.txt")"
if [[ "${DO_CPU}" -eq 1 ]]; then
    MAIN_REPO="$(cd "$(git -C "${REPO_ROOT}" rev-parse --git-common-dir)/.." && pwd)"
    NATRON_BUILD="${NATRON_BUILD:-${MAIN_REPO}/build/release}"
    PLUGINS="${NATRON_PLUGINS:-${MAIN_REPO}/build/assets/Plugins}"
    RENDERER="${NATRON_BUILD}/Renderer/NatronRenderer"
    deadline=$(( $(date +%s) + CPU_WAIT_MIN * 60 ))
    : > "${OUT}/cpu-context.tsv"
    rm -f "${OUT}/cpu-results.jsonl"
    cpu_status="ok"

    wait_idle() {
        while build_busy || [[ ! -x "${RENDERER}" ]]; do
            if [[ $(date +%s) -ge ${deadline} ]]; then
                return 1
            fi
            echo "$(date +%T) waiting: build running in ${BUSY_CONTAINER} or ${RENDERER} missing"
            sleep 180
        done
    }

    # size_label WxH grades frames
    cpu_sizes=("UHD 3840x2160 16 3" "8K 7680x4320 8 3" "16k 16000x16000 4 2" "24k 24000x24000 4 2")
    run_cpu() {
        local tag="$1" topo="$2" n="$3" label="$4" dims="$5" frames="$6" blur="$7"
        wait_idle || return 1
        local before after
        before="$(cat /proc/loadavg | cut -d' ' -f1)"
        echo "== CPU ${tag} (${topo} n=${n} ${label} ${dims}, load1 ${before})"
        rm -f "${OUT}"/work/*.exr
        docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp/natron-bench-home \
            -e BENCH_TOPO="${topo}" -e BENCH_N="${n}" -e BENCH_RES="${label}" -e BENCH_SIZE="${dims}" \
            -e BENCH_FRAMES="${frames}" -e BENCH_BLUR_SIZE="${blur}" -e BENCH_OUT="${OUT}/cpu-results.jsonl" \
            -e BENCH_WORK="${OUT}/work" -e BENCH_RENDERER="${RENDERER}" -e OFX_PLUGIN_PATH="${PLUGINS}" \
            -v "${MAIN_REPO}:${MAIN_REPO}:ro" -v "${REPO_ROOT}:${REPO_ROOT}" -w "${REPO_ROOT}" "${IMAGE}" \
            bash -lc "timeout 3000 xvfb-run --auto-servernum '${RENDERER}' -b tools/bench/graph_bench.py" \
            > "${OUT}/cpu-${tag}.log" 2>&1 || { echo "run failed, see ${OUT}/cpu-${tag}.log"; return 2; }
        rm -f "${OUT}"/work/*.exr
        if build_busy; then after=busy; else after=idle; fi
        printf '%s\t%s\t%s\t%s\n' "${tag}" "${before}" "${after}" "$(nproc)" >> "${OUT}/cpu-context.tsv"
    }

    for spec in "${cpu_sizes[@]}"; do
        read -r label dims grades frames <<< "${spec}"
        run_cpu "${label}-chain0" chain 0 "${label}" "${dims}" "${frames}" 3 || { cpu_status="incomplete at ${label}: status $? (1 = build still running after the wait, 2 = run failed)"; break; }
        run_cpu "${label}-chain${grades}" chain "${grades}" "${label}" "${dims}" "${frames}" 3 || { cpu_status="incomplete at ${label}: status $? (1 = build still running after the wait, 2 = run failed)"; break; }
        for sigma in 3 25 100; do
            size=$(python3 -c "print(${sigma} * 2.4)")
            run_cpu "${label}-blur${sigma}" blurchain 1 "${label}" "${dims}" "${frames}" "${size}" || { cpu_status="incomplete at ${label}: status $? (1 = build still running after the wait, 2 = run failed)"; break 2; }
        done
    done
    if [[ "${cpu_status}" != ok ]]; then
        echo "CPU baselines ${cpu_status} (waited up to ${CPU_WAIT_MIN} min for ${BUSY_CONTAINER}'s build)"
    fi
fi
[[ "${DO_CPU}" -eq 1 ]] && echo "${cpu_status}" > "${OUT}/cpu-status.txt"

python3 -I "${SPIKE_DIR}/bench/summarize.py" "${OUT}" "${RUNS}" | tee "${OUT}/summary.txt"
