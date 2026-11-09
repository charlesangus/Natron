#!/usr/bin/env bash
# Builds the spike, runs the baseline build (reference, interpreter, JIT) and the x86-64-v3
# interpreter build, then prints LLVM's packaging footprint. Needs the natron-dev image (LLVM 21
# headers, libLLVM, clang). Arguments after the script name go to both runs, e.g. --points 2000000.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build="$root/build/spikes/geo-kernel"

cmake -S "$root/tools/spikes/geo-kernel" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j"${GK_JOBS:-4}" >/dev/null

# gk_bench_rel runs the JIT on the distribution's release libLLVM; gk_bench on the image's
# assertions build, which compiles slower.
main="$build/gk_bench"
[[ -x "$build/gk_bench_rel" ]] && main="$build/gk_bench_rel"

echo "== host: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //'), $(nproc) threads"
echo "== $(basename "$main")"
"$main" "$@"
echo
echo "== interpreter and reference rebuilt for x86-64-v3"
"$build/gk_bench_v3" --no-jit --edits 0 "$@"
echo

lib="$(ldd "$main" | awk '/libLLVM/ { print $3; exit }')"
if [[ -z "$lib" ]]; then
    echo "gk_bench does not link libLLVM; no footprint to report"
    exit 0
fi
lib="$(readlink -f "$lib")"
stripped="${build:?}/libLLVM.stripped"
strip -o "$stripped" "$lib"
mb() { awk -v b="$1" 'BEGIN { printf "%.1f MB", b / 1048576 }'; }
echo "== LLVM footprint ($lib)"
echo "  on disk:           $(mb "$(stat -c %s "$lib")")"
echo "  stripped:          $(mb "$(stat -c %s "$stripped")")"
echo "  stripped, xz -9:   $(mb "$(xz -9 -T0 -c "$stripped" | wc -c)")"
if command -v zstd >/dev/null; then
    echo "  stripped, zstd -19: $(mb "$(zstd -19 -T0 -c "$stripped" | wc -c)")"
fi
echo "  SONAME:            $(objdump -p "$lib" | awk '/SONAME/ { print $2 }')"
echo "  symbol version:    $(objdump -T "$lib" | awk '$6 ~ /^LLVM_/ { print $6; exit }')"
echo "  bench binary:      $(mb "$(stat -c %s "$main")") (links libLLVM dynamically)"
rm -f "$stripped"
