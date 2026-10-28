#!/usr/bin/env bash
# Builds DreamWorks' MoonRay renderer and its Hydra render delegate (hdMoonray) against the image's
# own OpenUSD and installs them, relocatably, into a private prefix (/opt/moonray by default). Run by
# tools/ci/local/Dockerfile.
#
# MoonRay comes from github.com/dreamworksanimation/openmoonray, Apache-2.0, a super-repository of
# submodules. The tag is checked against its commit, and that commit pins every submodule. CPU only:
# OptiX/CUDA (the XPU path) is off. The Qt applications, the unit tests, distributed Arras
# (arras4_node, minicoord), the Houdini plug-ins, the USD geometry procedural (moonshine_usd), the
# profile viewer and the MaterialX shaders are not built, so their submodules are never fetched.
#
# Every library MoonRay links comes from the image (/usr/local: OpenUSD, oneTBB, Boost, Embree,
# OpenVDB, OpenImageIO, OpenEXR/Imath, OpenColorIO, OpenImageDenoise, OpenSubdiv, JsonCpp,
# log4cplus, Lua, Random123, libmicrohttpd, CppUnit; /lib64: uuid, curl, OpenSSL, libatomic), so
# the delegate loads the same copies Natron and USD already do; the checks at the end fail the image
# if it does not. The one tool the image lacks is a working ISPC (its /usr/local/bin/ispc needs an
# LLVM the image does not ship), so the upstream-pinned release binary is fetched for the build
# only and deleted with the build tree.
#
# The prefix holds MoonRay's own libraries in lib64/ (RUNPATH $ORIGIN/../lib64), the scene object
# DSOs in rdl2dso/, the Hydra plug-ins under plugin/ (plugin/pxr/plugInfo.json), the Arras session
# definitions in sessions/ and the execComp render process in bin/. Nothing is added to the loader
# path; Natron finds the prefix at start-up (AppManager) and stage-bundle.sh copies it into the
# bundle as moonray/.
#
# Required environment: NATRON_MOONRAY_VERSION (an openmoonray tag, e.g. v2026.29.1),
# NATRON_MOONRAY_COMMIT (the commit that tag names), NATRON_MOONRAY_ISPC_VERSION (e.g. v1.21.0),
# NATRON_MOONRAY_ISPC_SHA256 (of the ISPC Linux release archive). Optional: NATRON_MOONRAY_PREFIX
# (default /opt/moonray).

set -euo pipefail

version="${NATRON_MOONRAY_VERSION:?}"
commit="${NATRON_MOONRAY_COMMIT:?}"
ispc_version="${NATRON_MOONRAY_ISPC_VERSION:?}"
ispc_sha256="${NATRON_MOONRAY_ISPC_SHA256:?}"
prefix="${NATRON_MOONRAY_PREFIX:-/opt/moonray}"
repo_base="https://github.com/dreamworksanimation"
ispc_url="https://github.com/ispc/ispc/releases/download/${ispc_version}/ispc-${ispc_version}-linux.tar.gz"

usd_prefix=/usr/local
test -f "${usd_prefix}/pxrConfig.cmake"

work="$(mktemp -d /tmp/moonray-build.XXXXXX)"
src="${work}/src"
build="${work}/build"
ispc_dir="${work}/ispc"
mkdir -p "$build" "$ispc_dir"

wget -q -O "${work}/ispc.tar.gz" "$ispc_url"
echo "${ispc_sha256}  ${work}/ispc.tar.gz" | sha256sum -c -
tar -xzf "${work}/ispc.tar.gz" -C "$ispc_dir" --strip-components=1
ispc="${ispc_dir}/bin/ispc"
"$ispc" --version

git clone -q --depth 1 --branch "$version" "${repo_base}/openmoonray.git" "$src"
if [ "$(git -C "$src" rev-parse HEAD)" != "$commit" ]; then
    echo "build-moonray: tag ${version} is not commit ${commit}" >&2
    exit 1
fi

# Each submodule is fetched at the commit the pinned super-repository records for it.
fetch_submodule() {
    local path="$1" sha name
    sha="$(git -C "$src" ls-tree HEAD "$path" | awk '$2 == "commit" { print $3 }')"
    if [ -z "$sha" ]; then
        echo "build-moonray: ${path} is not a submodule at ${version}" >&2
        exit 1
    fi
    name="$(git -C "$src" config -f .gitmodules --get "submodule.${path}.url")"
    name="${name##*/}"
    mkdir -p "$src/$path"
    git -C "$src/$path" init -q
    git -C "$src/$path" fetch -q --depth 1 "${repo_base}/${name}.git" "$sha"
    git -C "$src/$path" checkout -q FETCH_HEAD
}
for path in cmake_modules arras/arras4_core moonray/scene_rdl2 moonray/mcrt_denoise moonray/moonray \
    moonray/moonshine moonray/moonray_arras/mcrt_messages moonray/moonray_arras/mcrt_dataio \
    moonray/moonray_arras/mcrt_computation moonray/hydra/hdMoonray; do
    fetch_submodule "$path"
done

# Drops one add_subdirectory() line, failing if upstream has moved it.
drop_subdirectory() {
    local file="$1" dir="$2"
    grep -q "^add_subdirectory(${dir})$" "$file" || {
        echo "build-moonray: no add_subdirectory(${dir}) in ${file}" >&2
        exit 1
    }
    sed -i "/^add_subdirectory(${dir})$/d" "$file"
}
drop_subdirectory "${src}/CMakeLists.txt" rats
drop_subdirectory "${src}/arras/CMakeLists.txt" distributed
drop_subdirectory "${src}/moonray/CMakeLists.txt" moonray_dcc_plugins
drop_subdirectory "${src}/moonray/CMakeLists.txt" moonshine_usd
drop_subdirectory "${src}/moonray/CMakeLists.txt" render_profile_viewer

# OpenUSD 26.08 removed Ndr, which the shader discovery plug-ins (moonray_sdr_plugins) are written
# against. hdMoonray does not use them; they only describe MoonRay shaders to USD's Sdr registry.
if [ ! -d "${usd_prefix}/include/pxr/usd/ndr" ]; then
    drop_subdirectory "${src}/moonray/hydra/CMakeLists.txt" moonray_sdr_plugins
else
    fetch_submodule moonray/hydra/moonray_sdr_plugins
fi

# OpenUSD 26.08 (hd API 103, pxr/imaging/hd/version.h) dropped HdRendererPlugin::IsSupported(bool),
# which both hdMoonray plug-ins still mark override; it loses the keyword. Applied only when
# present, so a tag that has caught up builds unpatched.
if grep -q '102 -> 103' "${usd_prefix}/include/pxr/imaging/hd/version.h"; then
    for plugin in hd_moonray hd_moonray_debug; do
        file="${src}/moonray/hydra/hdMoonray/plugin/${plugin}/RendererPlugin.cc"
        sed -i 's/bool IsSupported(bool gpuEnabled = true) const override {/bool IsSupported(bool gpuEnabled = true) const {/' "$file"
    done
fi

# USD sorts render delegates by ascending priority and defaults to the first that loads, so
# upstream's 99 keeps MoonRay behind Storm (0); it is only used when asked for by id.
for plugin in hd_moonray hd_moonray_debug; do
    grep -q '"priority": 99' "${src}/moonray/hydra/hdMoonray/plugin/${plugin}/plugInfo.json.in"
done

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
# USD was built by Conan, so pxrTargets.cmake names MaterialX's targets materialx::<Lib>; the image's
# MaterialX config names them <Lib>. This include (run after every project()) adds the aliases, as
# cmake/NatronUsd.cmake does, without patching MoonRay. IMPORTED_GLOBAL lets the alias be seen from
# the subdirectory that calls find_package(pxr).
mtlx_aliases="${work}/natron-mtlx-aliases.cmake"
cat >"$mtlx_aliases" <<'MTLX_EOF'
if(NOT TARGET materialx::MaterialXCore)
    find_package(MaterialX CONFIG QUIET PATHS "${NATRON_MTLX_USD_PREFIX}/lib/cmake/MaterialX")
    foreach(_natron_mtlx_lib MaterialXCore MaterialXFormat MaterialXGenShader MaterialXGenGlsl MaterialXRender)
        if(TARGET ${_natron_mtlx_lib} AND NOT TARGET materialx::${_natron_mtlx_lib})
            set_target_properties(${_natron_mtlx_lib} PROPERTIES IMPORTED_GLOBAL TRUE)
            add_library(materialx::${_natron_mtlx_lib} ALIAS ${_natron_mtlx_lib})
        endif()
    endforeach()
    unset(_natron_mtlx_lib)
endif()
MTLX_EOF

boost_python="$(find "${usd_prefix}/lib" -maxdepth 1 -name 'libboost_python3*.so' -printf '%f\n' | head -n 1)"
boost_python="${boost_python#libboost_}"
boost_python="${boost_python%.so}"
test -n "$boost_python"

# UDPSYSLOG_NO_BOOST: Arras' syslog sink is written against boost::asio::io_service, which Boost
# 1.87 removed; its POSIX-socket variant is selected instead.
# CMAKE_CUDA_COMPILER=NOTFOUND stops the top-level check_language(CUDA) from enabling CUDA because
# the image happens to carry a toolkit.
# MoonRay was written against older libstdc++ headers that pulled <algorithm>, <cstdint>, <cstring> and
# <limits> in transitively; GCC 14's do not, so they are force-included for every C++ unit.
cmake -S "$src" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$usd_prefix" \
    -DNATRON_MTLX_USD_PREFIX="$usd_prefix" \
    -DCMAKE_PROJECT_INCLUDE="$mtlx_aliases" \
    -DCMAKE_CXX_FLAGS="-DUDPSYSLOG_NO_BOOST -include algorithm -include cstdint -include cstring -include limits" \
    -DCMAKE_ISPC_COMPILER="$ispc" \
    -DCMAKE_CUDA_COMPILER=NOTFOUND \
    -DPXR_USD_LOCATION="$usd_prefix" \
    -Dpxr_DIR="$usd_prefix" \
    "${dep_dirs[@]}" \
    -DPython_EXECUTABLE="$(command -v python3)" \
    -DBOOST_PYTHON_COMPONENT_NAME="$boost_python" \
    -DMOONRAY_USE_OPTIX=NO \
    -DBUILD_QT_APPS=NO \
    -DBUILD_MATERIALX_SHADERS=OFF \
    -DBUILD_TESTING=OFF \
    -DARRASCORE_BUILD_TESTING=OFF

# One job per 2 GB of RAM keeps a 16 GB CI runner out of the OOM killer.
mem_kb="$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)"
jobs=$((mem_kb / (2 * 1024 * 1024)))
if [ "$jobs" -gt "$(nproc)" ]; then jobs="$(nproc)"; fi
if [ "$jobs" -lt 1 ]; then jobs=1; fi
SECONDS=0
cmake --build "$build" -j "$jobs"
echo "build-moonray: compiled in $((SECONDS / 60)) min with ${jobs} jobs"
cmake --install "$build" --prefix "$prefix" >/dev/null
install -m 0644 "${src}/LICENSE" "${prefix}/LICENSE"
install -m 0644 "${src}/THIRD-PARTY.md" "${prefix}/THIRD-PARTY.md"

# The layout the delegate needs at run time.
for file in plugin/hd_moonray.so plugin/hd_moonray_debug.so plugin/pxr/plugInfo.json \
    plugin/pxr/hd_moonray/plugInfo.json plugin/pxr/hd_moonray_debug/plugInfo.json \
    sessions/hd_single.sessiondef sessions/hd_multi.sessiondef bin/execComp \
    lib64/libcomputation_progmcrt.so lib64/libdenoiser.so; do
    if [ ! -e "${prefix}/${file}" ]; then
        echo "build-moonray: ${prefix}/${file} was not installed" >&2
        exit 1
    fi
done
grep -q '"HdMoonrayRendererPlugin"' "${prefix}/plugin/pxr/hd_moonray/plugInfo.json"
grep -q '"priority": 99' "${prefix}/plugin/pxr/hd_moonray/plugInfo.json"
dso_count="$(find "${prefix}/rdl2dso" -maxdepth 1 -name '*.so' | wc -l)"
proxy_count="$(find "${prefix}/rdl2dso" -maxdepth 1 -name '*.so.proxy' | wc -l)"
if [ "$dso_count" -eq 0 ] || [ "$proxy_count" -eq 0 ]; then
    echo "build-moonray: rdl2dso holds ${dso_count} DSOs and ${proxy_count} proxies" >&2
    exit 1
fi
echo "build-moonray: ${dso_count} rdl2 DSOs, ${proxy_count} proxies"

# What the delegate loads: the plug-ins, the render process, MoonRay's libraries and the DSOs.
elves=()
while IFS= read -r file; do
    if file -b "$file" | grep -q '^ELF'; then
        elves+=("$file")
    fi
done < <(find "${prefix}/plugin" "${prefix}/lib64" "${prefix}/rdl2dso" -type f \( -name '*.so' -o -name '*.so.*' \)
    echo "${prefix}/bin/execComp")

# Every dependency resolves, each soname to one file, none from a second version of a library
# (libtbb next to a private libtbb, libOpenEXR-3_3 next to libOpenEXR-3_4), and the public ones
# from the image's /usr/local/lib.
raw="$(for elf in "${elves[@]}"; do ldd "$elf"; done)"
if grep -q 'not found' <<<"$raw"; then
    grep 'not found' <<<"$raw" | sort -u >&2
    echo "build-moonray: unresolved dependency" >&2
    exit 1
fi
deps="$(awk '/=>/ { print $1, $3 }' <<<"$raw" | sort -u)"
split="$(awk '{ print $1 }' <<<"$deps" | uniq -d)"
if [ -n "$split" ]; then
    echo "build-moonray: resolved to more than one file: ${split}" >&2
    exit 1
fi
for lib in libusd_hd libtbb libOpenImageIO libOpenEXR libembree4 libopenvdb libOpenImageDenoise; do
    if ! grep -q "^${lib}[.-].* /usr/local/lib/" <<<"$deps"; then
        echo "build-moonray: ${lib} does not resolve to /usr/local/lib" >&2
        exit 1
    fi
done
# libjpeg is exempt: the image's libjpeg-turbo (libjpeg.so.8, /usr/local) sits next to the system
# libjpeg.so.62 that the system libtiff.so.5 (/lib64) needs. Natron.bin carries the same pair, the
# sonames differ so they do not clash at runtime, and the image has no /usr/local libtiff to
# replace the system one.
dupes="$(awk '{ print $1 }' <<<"$deps" | sed -e 's/\.so.*$//' -e 's/-[0-9_]*$//' | sort | uniq -d | grep -vx 'libjpeg' || true)"
if [ -n "$dupes" ]; then
    echo "build-moonray: more than one version of: ${dupes}" >&2
    exit 1
fi
# MoonRay's own libraries must not shadow a library the image already provides.
while IFS= read -r lib; do
    for dir in /usr/local/lib /lib64 /usr/lib64; do
        if [ -e "${dir}/${lib}" ]; then
            echo "build-moonray: ${prefix}/lib64/${lib} shadows ${dir}/${lib}" >&2
            exit 1
        fi
    done
done < <(find "${prefix}/lib64" -maxdepth 1 -name '*.so*' -printf '%f\n')

du -sh "$prefix" "${prefix}/lib64" "${prefix}/rdl2dso" "${prefix}/plugin" "${prefix}/bin"
rm -rf "${work:?}"
