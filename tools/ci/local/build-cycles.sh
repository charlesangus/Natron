#!/usr/bin/env bash
# Builds the Cycles Hydra render delegate (hdCycles) against the image's own OpenUSD and installs it
# into USD's plug-in directory, next to hdStorm. Run by tools/ci/local/Dockerfile.
#
# Cycles comes from the upstream standalone repository (https://projects.blender.org/blender/cycles,
# mirrored at github.com/blender/cycles), Apache-2.0. Only the hdCycles target is built: no
# standalone app, no tests. CPU devices only; every GPU backend is off.
#
# Cycles' FindUSDPixar.cmake takes TBB, OpenSubdiv, OpenVDB, OpenColorIO, OpenEXR and OpenImageIO
# from the directory USD's libraries live in, which here is /usr/local/lib, and Embree,
# OpenImageDenoise, pugixml and zstd come from the same prefix. So the delegate links the very
# copies Natron and USD already load; the checks at the end fail the image if it does not. The
# blender/lib-* precompiled library submodules are never fetched (the archive does not carry them),
# which is what puts Cycles on that system-library path.
#
# Required environment: NATRON_CYCLES_VERSION (an upstream tag, e.g. v5.2.0), NATRON_CYCLES_SHA256
# (of the GitHub tag archive). Optional: NATRON_CYCLES_OIDN (ON/OFF, default ON) for the
# OpenImageDenoise denoiser.

set -euo pipefail

version="${NATRON_CYCLES_VERSION:?}"
sha256="${NATRON_CYCLES_SHA256:?}"
oidn="${NATRON_CYCLES_OIDN:-ON}"
url="https://github.com/blender/cycles/archive/refs/tags/${version}.tar.gz"

usd_prefix=/usr/local
plugin_dir="${usd_prefix}/plugin/usd"
test -f "${usd_prefix}/pxrConfig.cmake"
test -f "${plugin_dir}/plugInfo.json"

work="$(mktemp -d /tmp/cycles-build.XXXXXX)"
src="${work}/src"
build="${work}/build"
helpers="${work}/cmake"
mkdir -p "$src" "$build" "${helpers}/ocio" "${helpers}/no-epoxy"

wget -q -O "${work}/cycles.tar.gz" "$url"
echo "${sha256}  ${work}/cycles.tar.gz" | sha256sum -c -
tar -xzf "${work}/cycles.tar.gz" -C "$src" --strip-components=1
test -f "${src}/src/hydra/plugInfo.json"

# OpenUSD 26.08 (hd API 103, pxr/imaging/hd/version.h) dropped HdRendererPlugin::IsSupported(bool),
# which Cycles v5.2.0 still marks override; the method is never called, so it just loses the
# keyword. Applied only when present, so a tag that has caught up builds unpatched.
plugin_h="${src}/src/hydra/plugin.h"
if grep -q 'bool IsSupported(bool gpuEnabled) const override;' "$plugin_h" &&
    grep -q '102 -> 103' "${usd_prefix}/include/pxr/imaging/hd/version.h"; then
    sed -i 's/bool IsSupported(bool gpuEnabled) const override;/bool IsSupported(bool gpuEnabled) const;/' "$plugin_h"
fi

# hdCycles declares priority 0, Storm's. USD sorts render delegates by ascending priority, ties by
# type name, and takes the first that loads as its default
# (HdRendererPluginRegistry::GetDefaultPluginId), so HdCyclesPlugin would win over
# HdStormRendererPlugin for every engine made without an explicit id. 99, as hdEmbree declares,
# puts it after Storm: used only when asked for by id.
cycles_plug_info="${src}/src/hydra/resources/plugInfo.json"
grep -q '"priority": 0' "$cycles_plug_info"
sed -i 's/"priority": 0/"priority": 99/' "$cycles_plug_info"

# The image's pxrConfig.cmake still names the Conan build tree for each dependency's <Pkg>_DIR;
# point them at the image's installs, as cmake/NatronUsd.cmake does for Natron's own configure.
dep_dirs=()
for dep in Imath MaterialX OpenSubdiv; do
    if [ -d "${usd_prefix}/lib/cmake/${dep}" ]; then
        dep_dirs+=("-D${dep}_DIR=${usd_prefix}/lib/cmake/${dep}")
    fi
done
if [ -d "${usd_prefix}/lib/cmake/Ptex" ]; then
    dep_dirs+=("-DPTex_DIR=${usd_prefix}/lib/cmake/Ptex")
fi

# OpenColorIO is the exception: FindUSDPixar.cmake imports OpenColorIO::OpenColorIO itself, from
# the same /usr/local/lib library, and fails if pxrConfig.cmake has already done so. None of the USD
# targets the delegate links reaches OpenColorIO, so a config that only reports success is enough.
echo 'set(OpenColorIO_FOUND TRUE)' >"${helpers}/ocio/OpenColorIOConfig.cmake"

# Epoxy: Cycles requires it whenever the delegate is on, but only its GL display driver includes
# it, and that file is compiled only when USD_INCLUDE_DIR is set, which FindUSDPixar.cmake never
# does. The image has the runtime library but no headers, so an empty include directory satisfies
# the find, and --as-needed keeps the unused library out of the delegate's NEEDED list.
#
# WITH_CYCLES_NATIVE_ONLY stays off: the CPU kernels are built for every x86-64 level Cycles
# supports and picked at run time, so the image's build host does not decide what users can run.
cmake -S "$src" -B "$build" -G Ninja \
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
    -DWITH_CYCLES_OPENIMAGEDENOISE="$oidn" \
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

# The CPU kernel translation units each take several GB to compile; one job per 3 GB of RAM keeps
# a 16 GB CI runner out of the OOM killer.
mem_kb="$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)"
jobs=$((mem_kb / (3 * 1024 * 1024)))
if [ "$jobs" -gt "$(nproc)" ]; then jobs="$(nproc)"; fi
if [ "$jobs" -lt 1 ]; then jobs=1; fi
cmake --build "$build" --target hdCycles -j "$jobs"

# Cycles' own install puts the delegate at <prefix>/hydra/hdCycles.so with its plugInfo.json at
# <prefix>/hydra/hdCycles/resources/; USD's plugin/usd has the same */resources/ layout, so the
# generated plugInfo.json (LibraryPath ../hdCycles.so, relative to its Root ..) is valid there
# unchanged. Its ResourcePath, the Cycles data root, then resolves to <prefix>/plugin; the CPU
# device reads nothing from it.
delegate="${plugin_dir}/hdCycles.so"
install -m 0755 "${build}/src/hydra/hdCycles.so" "$delegate"
install -d "${plugin_dir}/hdCycles/resources"
install -m 0644 "${build}/src/hydra/resources/plugInfo.json" "${plugin_dir}/hdCycles/resources/plugInfo.json"
install -m 0644 "${src}/LICENSE" "${plugin_dir}/hdCycles/LICENSE"
grep -q '"LibraryPath": "../hdCycles.so"' "${plugin_dir}/hdCycles/resources/plugInfo.json"
grep -q '"HdCyclesPlugin"' "${plugin_dir}/hdCycles/resources/plugInfo.json"
grep -q '"priority": 99' "${plugin_dir}/hdCycles/resources/plugInfo.json"

# Every dependency resolves, none from outside the image's prefixes, epoxy is not linked, and no
# library appears in two versions (libOpenEXR-3_3 next to libOpenEXR-3_4, say).
deps="$(ldd "$delegate")"
echo "$deps"
if grep -q 'not found' <<<"$deps"; then
    echo "build-cycles: unresolved dependency" >&2
    exit 1
fi
if readelf -d "$delegate" | grep -q 'NEEDED.*libepoxy'; then
    echo "build-cycles: hdCycles.so links libepoxy" >&2
    exit 1
fi
for lib in libusd_hd libtbb libOpenImageIO libOpenEXR libembree4 libopenvdb; do
    if ! grep -q "${lib}[.-].* => /usr/local/lib/" <<<"$deps"; then
        echo "build-cycles: ${lib} does not resolve to /usr/local/lib" >&2
        exit 1
    fi
done
# libjpeg is exempt: the image's libjpeg-turbo (libjpeg.so.8, /usr/local) sits next to the system
# libjpeg.so.62 that the system libtiff.so.5 (/lib64) needs. Natron.bin carries the same pair, the
# sonames differ so they do not clash at runtime, and the image has no /usr/local libtiff to
# replace the system one.
dupes="$(awk '/=>/ { print $1 }' <<<"$deps" | sed -e 's/\.so.*$//' -e 's/-[0-9_]*$//' | sort | uniq -d | grep -vx 'libjpeg' || true)"
if [ -n "$dupes" ]; then
    echo "build-cycles: more than one version of: ${dupes}" >&2
    exit 1
fi

du -h "$delegate"
rm -rf "${work:?}"
