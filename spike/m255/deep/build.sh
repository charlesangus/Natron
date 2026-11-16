#!/usr/bin/env bash
# Builds the pinned Cycles fork with cycles-deep.patch applied, and its hdCycles Hydra delegate, in
# the published dev image with the CMake flags tools/ci/local/build-cycles.sh uses for the image's
# own delegate. The result is laid out as the image installs it, under out/plugin/usd/, for run.sh.
#
# Usage: build.sh [--check]
#   --check  configure, then compile only the patched translation units with -fsyntax-only
# Environment: NATRON_DEV_IMAGE, DEEP_JOBS (parallel compile jobs; default one per 3 GB available).
# Everything goes to out/build.log; the last line printed is the status.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
image="${NATRON_DEV_IMAGE:-ghcr.io/charlesangus/natron-dev:6c97a956d295d586}"
repo=https://github.com/charlesangus/cycles
commit=90d7cb7bd2717142c171d811c394a8206ab447ca
mode="${1:-build}"
out="${here}/out"
log="${out}/build.log"
mkdir -p "$out"
: >"$log"
start=$(date +%s)

finish() {
    local code=$1 what=$2
    local mins=$((($(date +%s) - start + 59) / 60))
    if [ "$code" -eq 0 ]; then
        echo "DEEP BUILD: OK (${what}, ${mins} min)" | tee -a "$log"
    else
        tail -n 25 "$log"
        echo "DEEP BUILD: FAILED at ${what} (exit ${code}, ${mins} min); full log ${log}" | tee -a "$log"
    fi
    exit "$code"
}

src_git="${here}/src/cycles"
if [ ! -d "${src_git}/.git" ]; then
    mkdir -p "${here}/src"
    {
        git init -q "$src_git" &&
            git -C "$src_git" remote add origin "$repo" &&
            git -C "$src_git" fetch -q --depth 1 origin "$commit"
    } >>"$log" 2>&1 || finish $? "fetching ${repo}@${commit}"
fi
git -C "$src_git" cat-file -e "${commit}^{commit}" >>"$log" 2>&1 || finish $? "finding ${commit} in src/cycles"

# A fresh tree per run: the checkout in src/cycles stays the pristine pin the patch is checked against.
src="${out}/src"
rm -rf "${out:?}/src"
mkdir -p "$src"
{
    git -C "$src_git" archive "$commit" | tar -x -C "$src" &&
        GIT_CEILING_DIRECTORIES="$out" git -C "$src" apply --whitespace=nowarn "${here}/cycles-deep.patch" &&
        cmp "${here}/HydraDeepBuffer.h" "${src}/src/hydra/HydraDeepBuffer.h"
} >>"$log" 2>&1 || finish $? "applying cycles-deep.patch"
echo "patched source: ${src}" >>"$log"

docker run --rm -i --user "$(id -u):$(id -g)" -e HOME=/tmp -e MODE="$mode" -e OUT="$out" \
    -e DEEP_JOBS="${DEEP_JOBS:-}" -v "${here}:${here}" -w "$here" --entrypoint bash "$image" -s \
    >>"$log" 2>&1 <<'EOS'
set -euo pipefail
src="${OUT}/src"
build="${OUT}/build"
helpers="${OUT}/cmake-helpers"
usd_prefix=/usr/local
mkdir -p "$build" "${helpers}/ocio" "${helpers}/no-epoxy"

# The build-time patches of tools/ci/local/build-cycles.sh.
plugin_h="${src}/src/hydra/plugin.h"
if grep -q 'bool IsSupported(bool gpuEnabled) const override;' "$plugin_h" &&
    grep -q '102 -> 103' "${usd_prefix}/include/pxr/imaging/hd/version.h"; then
    sed -i 's/bool IsSupported(bool gpuEnabled) const override;/bool IsSupported(bool gpuEnabled) const;/' "$plugin_h"
fi
cycles_plug_info="${src}/src/hydra/resources/plugInfo.json"
grep -q '"priority": 0' "$cycles_plug_info"
sed -i 's/"priority": 0/"priority": 99/' "$cycles_plug_info"
dep_dirs=()
for dep in Imath MaterialX OpenSubdiv; do
    if [ -d "${usd_prefix}/lib/cmake/${dep}" ]; then
        dep_dirs+=("-D${dep}_DIR=${usd_prefix}/lib/cmake/${dep}")
    fi
done
if [ -d "${usd_prefix}/lib/cmake/Ptex" ]; then
    dep_dirs+=("-DPTex_DIR=${usd_prefix}/lib/cmake/Ptex")
fi
echo 'set(OpenColorIO_FOUND TRUE)' >"${helpers}/ocio/OpenColorIOConfig.cmake"

# The flags of tools/ci/local/build-cycles.sh, plus ccache (kept in out/) and compile_commands.json
# for --check; neither changes what is compiled.
export CCACHE_DIR="${OUT}/ccache"
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--as-needed \
    -DPXR_ROOT="$usd_prefix" \
    "${dep_dirs[@]}" \
    -DOpenColorIO_DIR="${helpers}/ocio" \
    -DEpoxy_INCLUDE_DIR="${helpers}/no-epoxy" \
    -DEpoxy_LIBRARY=/usr/lib64/libepoxy.so.0 \
    -DWITH_LIBS_PRECOMPILED=OFF \
    -DWITH_STRICT_BUILD_OPTIONS=ON \
    -DWITH_CYCLES_HYDRA_RENDER_DELEGATE=ON \
    -DWITH_CYCLES_USD=OFF \
    -DWITH_CYCLES_STANDALONE_GUI=OFF \
    -DWITH_CYCLES_NATIVE_ONLY=OFF \
    -DWITH_CYCLES_EMBREE=ON \
    -DWITH_CYCLES_OPENIMAGEDENOISE=ON \
    -DWITH_CYCLES_OPENSUBDIV=ON \
    -DWITH_CYCLES_OPENVDB=ON \
    -DWITH_CYCLES_NANOVDB=OFF \
    -DWITH_CYCLES_OSL=OFF \
    -DWITH_CYCLES_ALEMBIC=OFF \
    -DWITH_CYCLES_PUGIXML=OFF \
    -DWITH_CYCLES_LOGGING=OFF \
    -DWITH_CYCLES_DEVICE_CUDA=OFF \
    -DWITH_CYCLES_DEVICE_OPTIX=OFF \
    -DWITH_CYCLES_DEVICE_HIP=OFF \
    -DWITH_CYCLES_DEVICE_HIPRT=OFF \
    -DWITH_CYCLES_DEVICE_ONEAPI=OFF \
    -DWITH_CYCLES_DEVICE_METAL=OFF \
    -DWITH_CYCLES_CUDA_BINARIES=OFF \
    -DWITH_CYCLES_HIP_BINARIES=OFF \
    -DWITH_GTESTS=OFF
echo "STAGE configured"

if [ "$MODE" = "--check" ]; then
    python3 - "$build/compile_commands.json" <<'PY'
import json, shlex, subprocess, sys
touched = ("src/hydra/output_driver.cpp", "src/hydra/render_delegate.cpp", "src/hydra/session.cpp",
           "src/integrator/path_trace.cpp")
failed = 0
for entry in json.load(open(sys.argv[1])):
    if not entry["file"].endswith(touched):
        continue
    args = entry.get("arguments") or shlex.split(entry["command"])
    args = [a for a in args if a != "ccache"]
    if "-o" in args:
        i = args.index("-o")
        del args[i:i + 2]
    args = [a for a in args if a != "-c"] + ["-fsyntax-only"]
    r = subprocess.run(args, cwd=entry["directory"])
    print("SYNTAX", "ok" if r.returncode == 0 else "FAILED", entry["file"], flush=True)
    failed += r.returncode != 0
sys.exit(1 if failed else 0)
PY
    exit 0
fi

# One job per 3 GB of available memory: the CPU kernel translation units each take several GB.
mem_kb="$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)"
jobs="${DEEP_JOBS:-$((mem_kb / (3 * 1024 * 1024)))}"
if [ "$jobs" -gt "$(nproc)" ]; then jobs="$(nproc)"; fi
if [ "$jobs" -lt 1 ]; then jobs=1; fi
echo "STAGE compiling with ${jobs} jobs"
cmake --build "$build" --target hdCycles -j "$jobs"

plugin_dir="${OUT}/plugin/usd"
install -d "${plugin_dir}/hdCycles/resources"
install -m 0755 "${build}/src/hydra/hdCycles.so" "${plugin_dir}/hdCycles.so"
install -m 0644 "${build}/src/hydra/resources/plugInfo.json" "${plugin_dir}/hdCycles/resources/plugInfo.json"
grep -q '"LibraryPath": "../hdCycles.so"' "${plugin_dir}/hdCycles/resources/plugInfo.json"
deps="$(ldd "${plugin_dir}/hdCycles.so")"
if grep -q 'not found' <<<"$deps"; then
    echo "$deps"
    echo "unresolved dependency" >&2
    exit 1
fi
du -h "${plugin_dir}/hdCycles.so"
echo "STAGE installed ${plugin_dir}"
EOS
code=$?
if [ "$code" -ne 0 ]; then
    case "$(grep '^STAGE' "$log" | tail -n 1)" in
    "") failed_at="cmake configure" ;;
    "STAGE configured") failed_at="$([ "$mode" = "--check" ] && echo "syntax check" || echo "compile setup")" ;;
    "STAGE compiling"*) failed_at="compile or install of hdCycles" ;;
    *) failed_at="post-install checks" ;;
    esac
    finish "$code" "$failed_at"
fi
if [ "$mode" = "--check" ]; then
    finish 0 "configure and syntax check"
fi
finish 0 "${out}/plugin/usd/hdCycles.so"
