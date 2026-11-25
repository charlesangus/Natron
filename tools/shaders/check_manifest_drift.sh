#!/usr/bin/env bash
# Regenerates the Cycles and MoonRay shader manifests from the renderers installed in the dev image
# and fails, naming each node and attribute that changed, when they differ from the committed
# manifests. Run it in the image (or at image build, once the renderers are installed), so a
# renderer upgrade cannot change shaders unnoticed.
#
# Usage: check_manifest_drift.sh [options]
#   --manifest-dir DIR    committed manifests (default: Engine/Nodes/Scene/Shaders/manifest)
#   --cycles-source DIR   Cycles source at hdCycles' commit, to build the dump tool
#   --dump-tool PATH      an already built dump_cycles_manifest, instead of --cycles-source
#   --hdcycles PATH       the plug-in (default: /usr/local/plugin/usd/hdCycles.so)
#   --moonray-root DIR    the MoonRay install (default: /opt/moonray)
#   --only cycles|moonray check one renderer
#
# Regenerating is described in Engine/Nodes/docs/shader-manifest.md.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
manifest_dir="${root}/Engine/Nodes/Scene/Shaders/manifest"
cycles_source=""
dump_tool=""
hdcycles="/usr/local/plugin/usd/hdCycles.so"
moonray_root="${NATRON_MOONRAY_ROOT:-/opt/moonray}"
only=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --manifest-dir) manifest_dir="$2"; shift 2 ;;
        --cycles-source) cycles_source="$2"; shift 2 ;;
        --dump-tool) dump_tool="$2"; shift 2 ;;
        --hdcycles) hdcycles="$2"; shift 2 ;;
        --moonray-root) moonray_root="$2"; shift 2 ;;
        --only) only="$2"; shift 2 ;;
        *) echo "check_manifest_drift.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

work="$(mktemp -d "${TMPDIR:-/tmp}/manifest-drift.XXXXXX")"
trap 'rm -rf "${work:?}"' EXIT

status=0

if [[ -z "${only}" || "${only}" == "moonray" ]]; then
    python3 "${root}/tools/shaders/generate_moonray_manifest.py" \
        --moonray-root "${moonray_root}" --output "${work}/moonray.json"
    python3 "${root}/tools/shaders/check_manifest.py" \
        --diff "${manifest_dir}/moonray.json" "${work}/moonray.json" || status=1
fi

if [[ -z "${only}" || "${only}" == "cycles" ]]; then
    if [[ -z "${dump_tool}" ]]; then
        if [[ -z "${cycles_source}" ]]; then
            echo "check_manifest_drift.sh: Cycles needs --cycles-source or --dump-tool" >&2
            exit 2
        fi
        cmake -S "${root}/tools/shaders" -B "${work}/tools" -DCMAKE_BUILD_TYPE=Release \
            -DCYCLES_SOURCE_DIR="${cycles_source}" >/dev/null
        cmake --build "${work}/tools" >/dev/null
        dump_tool="${work}/tools/dump_cycles_manifest"
    fi
    "${dump_tool}" --library "${hdcycles}" --source "${hdcycles%.so}/SOURCE" \
        --output "${work}/cycles.json"
    python3 "${root}/tools/shaders/check_manifest.py" \
        --diff "${manifest_dir}/cycles.json" "${work}/cycles.json" || status=1
fi

if [[ "${status}" -ne 0 ]]; then
    echo "check_manifest_drift.sh: the renderers' shaders changed; regenerate the manifests" \
        "(Engine/Nodes/docs/shader-manifest.md) and update the layout tables" >&2
fi
exit "${status}"
