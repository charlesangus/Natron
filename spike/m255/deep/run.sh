#!/usr/bin/env bash
# Builds deep_probe and runs it in the published dev image against the hdCycles that build.sh put
# in out/plugin/usd/, then once against the image's stock hdCycles as a flat-only baseline.
#
# Usage: run.sh [--compile-only] [size] [samples] [maxDeepSamples]
# Output: out/probe.log (patched delegate), out/probe-stock.log (stock delegate), images in out/.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
image="${NATRON_DEV_IMAGE:-ghcr.io/charlesangus/natron-dev:6c97a956d295d586}"
out="${here}/out"
compile_only=0
if [ "${1:-}" = "--compile-only" ]; then
    compile_only=1
    shift
fi
size="${1:-256}" samples="${2:-16}" max_deep="${3:-64}"
mkdir -p "$out"

if [ "$compile_only" -eq 0 ] && [ ! -f "${out}/plugin/usd/hdCycles.so" ]; then
    echo "DEEP PROBE: FAILED, ${out}/plugin/usd/hdCycles.so is missing; run build.sh first"
    exit 1
fi

in_image() {
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "${here}:${here}" -w "$here" "$@"
}

in_image --entrypoint bash "$image" -c '
set -euo pipefail
py_inc="$(python3 -c "import sysconfig; print(sysconfig.get_paths()[\"include\"])")"
/opt/rh/gcc-toolset-14/root/usr/bin/g++ -std=gnu++20 -O1 -w -pthread -I. -I/usr/local/include -I"$py_inc" \
    deep_probe.cpp -o out/deep_probe -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
    -lusd_usdImagingGL -lusd_usdImaging -lusd_hdx -lusd_hd -lusd_hgi -lusd_cameraUtil \
    -lusd_usdLux -lusd_usdShade -lusd_usdGeom -lusd_usd -lusd_sdf -lusd_vt -lusd_gf -lusd_tf \
    -lusd_plug -lusd_arch -lusd_python -ltbb -lpython3.13 -lOpenImageIO -lOpenImageIO_Util
' >"${out}/probe-compile.log" 2>&1 || {
    tail -n 20 "${out}/probe-compile.log"
    echo "DEEP PROBE: FAILED to compile deep_probe.cpp; see ${out}/probe-compile.log"
    exit 1
}
if [ "$compile_only" -eq 1 ]; then
    echo "DEEP PROBE: compiled ${out}/deep_probe"
    exit 0
fi

# The image registers every */resources/ under /usr/local/plugin/usd, and both delegates are named
# HdCyclesPlugin, so the stock one is hidden behind an empty tmpfs for the patched run.
start=$(date +%s)
in_image --tmpfs /usr/local/plugin/usd/hdCycles -e PXR_PLUGINPATH_NAME="${out}/plugin/usd/hdCycles/resources" \
    --entrypoint "${out}/deep_probe" "$image" "$size" "$samples" "$max_deep" "$out" \
    >"${out}/probe.log" 2>"${out}/probe.stderr.log"
code=$?
mkdir -p "${out}/stock"
in_image --entrypoint "${out}/deep_probe" "$image" "$size" "$samples" "$max_deep" "${out}/stock" --flat-only \
    >"${out}/probe-stock.log" 2>"${out}/probe-stock.stderr.log" || echo "stock baseline failed; see ${out}/probe-stock.log"

cat "${out}/probe.log"
echo "-- stock delegate, flat only"
grep -E '^==|^  flat:|descriptor' "${out}/probe-stock.log"
secs=$(($(date +%s) - start))
if [ "$code" -eq 0 ] && grep -q 'RESULT: deep buffer handed out' "${out}/probe.log"; then
    echo "DEEP PROBE: OK (${secs} s); images in ${out}"
else
    tail -n 20 "${out}/probe.stderr.log"
    echo "DEEP PROBE: FAILED (exit ${code}, ${secs} s); see ${out}/probe.log and ${out}/probe.stderr.log"
    exit 1
fi
