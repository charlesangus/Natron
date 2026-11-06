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

# createLocal announces every local session to DreamWorks' coordinator through a SessionNotification,
# whose constructor starts a thread that deletes the object, then assigns and detaches that thread
# into a member of it. With LOGNAME unset the thread has nothing to send and can finish first, so the
# constructor or the destructor meets a joinable std::thread and the whole process dies with
# "terminate called without an active exception". No coordinator exists outside DreamWorks, so the
# call goes rather than being made safe; a missing anchor fails the build.
client_cc="${src}/arras/arras4_core/arras4_client/lib/client/api/Client.cc"
if [ "$(grep -cF 'new SessionNotification(aDefinition.getObject(),sessionOptions,' "$client_cc")" != 1 ] ||
    [ "$(grep -cF 'sessionId, mUserAgent);' "$client_cc")" != 1 ]; then
    echo "build-moonray: SessionNotification call not found exactly once in ${client_cc}" >&2
    exit 1
fi
sed -i '/new SessionNotification(aDefinition.getObject(),sessionOptions,$/,/sessionId, mUserAgent);$/d' "$client_cc"
if grep -q 'new SessionNotification' "$client_cc"; then
    echo "build-moonray: SessionNotification patch did not apply to ${client_cc}" >&2
    exit 1
fi

# OpenUSD 26.08 removed Ndr, which the shader discovery plug-ins (moonray_sdr_plugins) are written
# against. hdMoonray does not use them; they only describe MoonRay shaders to USD's Sdr registry.
if [ ! -d "${usd_prefix}/include/pxr/usd/ndr" ]; then
    drop_subdirectory "${src}/moonray/hydra/CMakeLists.txt" moonray_sdr_plugins
else
    fetch_submodule moonray/hydra/moonray_sdr_plugins
fi

# OpenUSD 26.08 (hd API 103, pxr/imaging/hd/version.h) dropped HdRendererPlugin::IsSupported(bool)
# and made IsSupported(const HdRendererCreateArgsSchema &, std::string *) pure. Both hdMoonray
# plug-ins still mark the bool overload override and lack the new one, so they cannot be
# instantiated. The bool overload loses the keyword, and the new override is added next to it,
# delegating to it with its old default. Upstream has no fix for API 103 (its unmerged
# test/usd263-issupported-override branch targets the removed HdRendererCreateArgs). Applied only
# when the plug-in still lacks the new overload; a missing anchor fails the build.
if grep -q '102 -> 103' "${usd_prefix}/include/pxr/imaging/hd/version.h"; then
    for plugin in hd_moonray hd_moonray_debug; do
        file="${src}/moonray/hydra/hdMoonray/plugin/${plugin}/RendererPlugin.cc"
        if grep -q 'HdRendererCreateArgsSchema' "$file"; then
            continue
        fi
        anchor='    bool IsSupported(bool gpuEnabled = true) const override {'
        if [ "$(grep -cxF "$anchor" "$file")" != 1 ]; then
            echo "build-moonray: IsSupported(bool) anchor not found exactly once in ${file}" >&2
            exit 1
        fi
        sed -i 's/^    bool IsSupported(bool gpuEnabled = true) const override {$/    bool IsSupported(const pxr::HdRendererCreateArgsSchema \&, std::string * = nullptr) const override {\n        return IsSupported(true);\n    }\n    bool IsSupported(bool gpuEnabled = true) const {/' "$file"
        grep -qF 'bool IsSupported(const pxr::HdRendererCreateArgsSchema &, std::string * = nullptr) const override {' "$file" || {
            echo "build-moonray: IsSupported patch did not apply to ${file}" >&2
            exit 1
        }
    done
fi

# mcrt_dataio's frame-buffer receiver has an unconditional "for debug" block that writes the
# session's host names to stderr whenever the MCRT count grows, which is at every render's start.
# The write is dropped; the count it keeps stays. A missing anchor fails the build.
file="${src}/moonray/moonray_arras/mcrt_dataio/lib/client/receiver/ClientReceiverFb.cc"
anchor='std::cerr << mGlobalNodeInfo.showAllHostsName()'
if [ "$(grep -cF "$anchor" "$file")" != 1 ]; then
    echo "build-moonray: host-name debug print not found exactly once in ${file}" >&2
    exit 1
fi
sed -i '/std::cerr << mGlobalNodeInfo\.showAllHostsName()/d' "$file"
if grep -qF 'showAllHostsName' "$file"; then
    echo "build-moonray: host-name debug print patch did not apply to ${file}" >&2
    exit 1
fi

# MoonRay's TextureSampler rejects any image that is not tiled and MIP-mapped, so a plain jpg or png
# fails with "image was untiled" and shades black unless it was first run through maketx. It now
# accepts them and has OpenImageIO generate the MIP levels on demand. autotile stays 0, one tile per
# level: scanline formats read only forwards, so a JPEG re-decodes from its first row whenever a tile
# above the last one read is asked for, which outweighs the finer eviction of 64x64 tiles. A missing
# anchor fails the build.
file="${src}/moonray/moonray/lib/rendering/texturing/sampler/TextureSampler.cc"
for attribute in automip accept_untiled accept_unmipped; do
    anchor="    mTextureSystem->attribute(\"${attribute}\", int(false));"
    if [ "$(grep -cxF "$anchor" "$file")" != 1 ]; then
        echo "build-moonray: ${attribute} setting not found exactly once in ${file}" >&2
        exit 1
    fi
    sed -i "s/^    mTextureSystem->attribute(\"${attribute}\", int(false));\$/    mTextureSystem->attribute(\"${attribute}\", int(true));/" "$file"
    if grep -qxF "$anchor" "$file"; then
        echo "build-moonray: ${attribute} patch did not apply to ${file}" >&2
        exit 1
    fi
done

# HdxTaskController adds a "depth" AOV next to "color" for any non-Storm delegate. hdMoonray maps
# both depth and cameraDepth to RESULT_DEPTH, and an unnamed RenderOutput takes its channel name from
# its result ("Z"), so MoonRay skips one of the two with an error-level "both target channel" log on
# every render. The depth AOV (the remapped clip-space one, which Natron does not read) gets its own
# channel name; cameraDepth keeps "Z". A missing anchor fails the build.
file="${src}/moonray/hydra/hdMoonray/lib/hydramoonray/RenderBuffer.cc"
anchor='                            mDepth = true; // process to OpenGL type clip space'
if ! grep -qF 'ro.setChannelName("hydraDepth");' "$file"; then
    if [ "$(grep -cxF "$anchor" "$file")" != 1 ]; then
        echo "build-moonray: depth AOV anchor not found exactly once in ${file}" >&2
        exit 1
    fi
    sed -i 's|^                            mDepth = true; // process to OpenGL type clip space$|&\n                            ro.setChannelName("hydraDepth");|' "$file"
fi
if [ "$(grep -cF 'ro.setChannelName("hydraDepth");' "$file")" != 1 ]; then
    echo "build-moonray: depth AOV channel patch did not apply to ${file}" >&2
    exit 1
fi

# A light's Sync reads its lightLink and shadowLink tokens with VtValue::Get<TfToken>(), and the
# scene delegate answers nothing for a light without links, so each light posts two "Attempted to
# get value of type 'TfToken' from empty VtValue" coding errors and falls back to the empty token
# anyway; GetWithDefault<TfToken>() is that fallback without the error. The same Sync dirties every
# rprim to pick up the categories through the change tracker, which in a render index fed by a
# scene index (Natron's) posts "Calling method on HdChangeTracker that requires emulation" and then
# applies the dirty bits all the same (pxr/imaging/hd/changeTracker.cpp, MarkAllRprimsDirty), so
# the error is the only fault. That call goes through RenderDelegate::markAllRprimsDirty, which
# takes the error off the thread's stack. A missing anchor fails the build.
lights="${src}/moonray/hydra/hdMoonray/lib/hydramoonray/Light.cc"
for link in lightLink shadowLink; do
    anchor="sceneDelegate->GetLightParamValue(id, pxr::HdTokens->${link}).Get<pxr::TfToken>();"
    if [ "$(grep -cF "$anchor" "$lights")" != 1 ]; then
        echo "build-moonray: ${link} read not found exactly once in ${lights}" >&2
        exit 1
    fi
    sed -i "s|sceneDelegate->GetLightParamValue(id, pxr::HdTokens->${link})\\.Get<pxr::TfToken>();|sceneDelegate->GetLightParamValue(id, pxr::HdTokens->${link}).GetWithDefault<pxr::TfToken>();|" "$lights"
    if grep -qF "$anchor" "$lights"; then
        echo "build-moonray: ${link} patch did not apply to ${lights}" >&2
        exit 1
    fi
done
anchor='sceneDelegate->GetRenderIndex().GetChangeTracker().MarkAllRprimsDirty('
if [ "$(grep -cF "$anchor" "$lights")" != 1 ]; then
    echo "build-moonray: MarkAllRprimsDirty call not found exactly once in ${lights}" >&2
    exit 1
fi
sed -i 's|sceneDelegate->GetRenderIndex()\.GetChangeTracker()\.MarkAllRprimsDirty(|renderDelegate.markAllRprimsDirty(|' "$lights"
if grep -qF "$anchor" "$lights"; then
    echo "build-moonray: MarkAllRprimsDirty patch did not apply to ${lights}" >&2
    exit 1
fi
file="${src}/moonray/hydra/hdMoonray/lib/hydramoonray/RenderDelegate.cc"
anchor='    if (mRenderIndex) mRenderIndex->GetChangeTracker().MarkAllRprimsDirty(bits);'
if [ "$(grep -cxF "$anchor" "$file")" != 1 ]; then
    echo "build-moonray: markAllRprimsDirty body not found exactly once in ${file}" >&2
    exit 1
fi
if [ "$(grep -cxF '#include <pxr/base/gf/vec2f.h>' "$file")" != 1 ]; then
    echo "build-moonray: vec2f include not found exactly once in ${file}" >&2
    exit 1
fi
sed -i 's|^    if (mRenderIndex) mRenderIndex->GetChangeTracker().MarkAllRprimsDirty(bits);$|    if (mRenderIndex) {\n        pxr::TfErrorMark mark;\n        mRenderIndex->GetChangeTracker().MarkAllRprimsDirty(bits);\n        mark.Clear();\n    }|; s|^#include <pxr/base/gf/vec2f.h>$|&\n#include <pxr/base/tf/errorMark.h>|' "$file"
if ! grep -qxF '        pxr::TfErrorMark mark;' "$file" || ! grep -qxF '#include <pxr/base/tf/errorMark.h>' "$file"; then
    echo "build-moonray: markAllRprimsDirty patch did not apply to ${file}" >&2
    exit 1
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
# <limits> (and more) in transitively; GCC 14's do not, so the common standard headers are force-included
# for every C++ unit.
cmake -S "$src" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$usd_prefix" \
    -DNATRON_MTLX_USD_PREFIX="$usd_prefix" \
    -DCMAKE_PROJECT_INCLUDE="$mtlx_aliases" \
    -DCMAKE_CXX_FLAGS="-DUDPSYSLOG_NO_BOOST -include algorithm -include array -include atomic -include chrono -include cmath -include cstdint -include cstdio -include cstdlib -include cstring -include functional -include iomanip -include limits -include memory -include mutex -include numeric -include optional -include sstream -include stdexcept -include string -include thread -include utility -include vector" \
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
# -k reports every failing unit in one image build instead of stopping at the first.
cmake --build "$build" -j "$jobs" -- -k
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
# Arras starts execComp, the render process, with its output on Natron's console, at the session
# definition's log level (resources.logLevel, 3, info, when unset) and trace level (traceThreshold,
# 0 when unset, which prints every level-0 trace). Each computation is set to errors only and no
# traces, so only a failing render prints.
python3 - "${prefix}/sessions/hd_single.sessiondef" "${prefix}/sessions/hd_multi.sessiondef" <<'PY'
import json
import sys

for path in sys.argv[1:]:
    with open(path) as f:
        definition = json.load(f)
    computations = {name: computation for name, computation in definition["computations"].items() if name != "(client)"}
    if "mcrt" not in computations:
        sys.exit("build-moonray: no mcrt computation in " + path)
    for computation in computations.values():
        computation["traceThreshold"] = -1
        computation.setdefault("requirements", {}).setdefault("resources", {})["logLevel"] = 1
    with open(path, "w") as f:
        json.dump(definition, f, indent=4)
        f.write("\n")
PY
for file in hd_single hd_multi; do
    grep -q '"traceThreshold": -1' "${prefix}/sessions/${file}.sessiondef"
    grep -q '"logLevel": 1' "${prefix}/sessions/${file}.sessiondef"
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
# ldd prints the path as the RUNPATH spelled it, so one file can appear as lib64/x.so and
# rdl2dso/../lib64/x.so. Canonicalise each path first: the same file then collapses, while two
# distinct files for one soname still stay apart and fail the check below.
deps="$(awk '/=>/ { print $1, $3 }' <<<"$raw" | sort -u | while read -r soname path; do
    echo "${soname} $(realpath -e "$path" 2>/dev/null || echo "$path")"
done | sort -u)"
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
