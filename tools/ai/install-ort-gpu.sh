#!/usr/bin/env bash
#
# Install a GPU build of ONNX Runtime where Natron looks for one: the
# onnxruntime/ folder in Natron's user data directory,
#   ${XDG_DATA_HOME:-~/.local/share}/INRIA/Natron/onnxruntime
# which the loader (Engine/Ai/OrtLoader.cpp) searches after the Preferences
# library path and before the CPU build bundled with Natron. Nothing else
# needs setting: leave Preferences > AI > AI ORT Library Path empty.
#
# --cuda   downloads Microsoft's official CUDA release of the ONNX Runtime
#          version Natron bundles (ONNXRUNTIME_VERSION in
#          tools/ci/local/fetch-assets.sh), for CUDA 12 or 13, and checks its
#          sha256 before unpacking it. Without a version it picks the newest
#          installed CUDA runtime (libcudart, nvcc) the driver supports.
# --rocm   builds ONNX Runtime from source with the MIGraphX execution
#          provider against the installed ROCm, as Microsoft publishes no
#          AMD build. The source is the release tag matching the installed
#          ROCm (the pinned version for ROCm newer than its table here),
#          checked against the tag's pinned commit. ONNX Runtime dropped its
#          separate ROCm execution provider in 1.23; MIGraphX is the AMD path.
#
# Each install goes into its own onnxruntime-<version>-<provider>.* folder
# beside onnxruntime/, which becomes a symlink switched to it once complete.
#
# usage: install-ort-gpu.sh (--cuda [12|13] | --rocm) [--prefix <dir>]
#                           [--dry-run] [--force] [-y]

set -euo pipefail

ORT_VERSION="1.31.1"
ORT_RELEASES="https://github.com/microsoft/onnxruntime/releases/download"
ORT_REPO="https://github.com/microsoft/onnxruntime.git"

# The official CUDA tarballs of ORT_VERSION, by CUDA major.
CUDA12_SHA256="57cd51f677eca19a269cdba31b0c16c15d73a8feea41f170aa8b750447ab1f70"
CUDA13_SHA256="b2146862e691f7e338f5aff9c7a9a5567c2ca57cf01442a1adb3a13bf62d9095"

# ROCm major.minor -> the ONNX Runtime release AMD supports MIGraphX on, and
# that tag's commit. ROCm newer than the first row builds ORT_VERSION. Natron
# accepts ONNX Runtime 1.18 or newer, so ROCm older than 6.2 is refused.
ROCM_TABLE="
7.2 1.23.2 a83fc4d58cb48eb68890dd689f94f28288cf2278
7.1 1.23.1 d9b2048791efb5804fe3d53a04b4971256addebf
7.0 1.22.1 89746dc19a0a1ae59ebf4b16df9acab8f99f3925
6.4 1.21.0 e0b66cad282043d4377cea5269083f17771b6dfc
6.3 1.19.0 26250ae74d2c9a3c6860625ba4a147ddfb936907
6.2 1.18.0 45737400a2f3015c11f005ed7603611eaed306a6
"
ORT_VERSION_COMMIT="6af1a932e2286d5c58166085d24c68783033edee"

STAMP_NAME=".natron-onnxruntime-gpu"

usage() {
    sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'
}

die() {
    echo "install-ort-gpu.sh: $*" >&2
    exit 1
}

MODE=""
CUDA_MAJOR=""
PREFIX=""
DRY_RUN=0
FORCE=0
YES=0

while [ $# -gt 0 ]; do
    case "$1" in
        --cuda)
            MODE="cuda"
            if [ $# -ge 2 ] && [[ "$2" =~ ^[0-9]+$ ]]; then
                CUDA_MAJOR="$2"
                shift
            fi
            ;;
        --rocm) MODE="rocm" ;;
        --prefix)
            [ $# -ge 2 ] || die "--prefix needs a directory"
            PREFIX="$2"
            shift
            ;;
        --dry-run) DRY_RUN=1 ;;
        --force) FORCE=1 ;;
        -y | --yes) YES=1 ;;
        -h | --help)
            usage
            exit 0
            ;;
        *) die "unknown argument '$1' (see --help)" ;;
    esac
    shift
done

[ -n "${MODE}" ] || {
    usage >&2
    exit 1
}
if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
    die "Natron's AI runtime is Linux x86-64 only"
fi

if [ -z "${PREFIX}" ]; then
    PREFIX="${XDG_DATA_HOME:-${HOME}/.local/share}/INRIA/Natron"
fi
TARGET="${PREFIX}/onnxruntime"

confirm() {
    if [ "${YES}" -eq 1 ] || [ "${DRY_RUN}" -eq 1 ]; then
        return 0
    fi
    local answer=""
    if [ -r /dev/tty ]; then
        read -rp "Continue? [y/N] " answer </dev/tty
    else
        die "no terminal to confirm on; re-run with -y"
    fi
    [[ "${answer}" =~ ^[Yy] ]] || {
        echo "Aborted."
        exit 0
    }
}

# Installs live in uniquely named directories next to TARGET, and TARGET is a
# symlink to the current one, swapped with a rename so Natron never sees a
# half-written install. The one it replaces is kept, as a running Natron may
# still load provider libraries from it; any older ones are removed.
STAGED=""
DOWNLOAD=""
BUILD_DIR=""
LINK_TMP=""

cleanup() {
    if [ -n "${STAGED}" ]; then
        rm -rf "${STAGED:?}"
    fi
    if [ -n "${DOWNLOAD}" ]; then
        rm -f "${DOWNLOAD:?}"
    fi
    if [ -n "${LINK_TMP}" ]; then
        rm -f "${LINK_TMP:?}"
    fi
    if [ -n "${BUILD_DIR}" ]; then
        rm -rf "${BUILD_DIR:?}"
    fi
}
trap cleanup EXIT

# Serialises installs into one prefix, for the life of the script.
lock_prefix() {
    mkdir -p "${PREFIX}"
    exec 9>"${PREFIX}/.onnxruntime.lock"
    flock -n 9 || die "another install into ${PREFIX} is running"
}

# Creates the empty directory a new install is staged in, named for what it
# holds, and records it for cleanup until it is installed.
make_staging_dir() {
    local name="$1"
    STAGED=$(mktemp -d "${PREFIX}/onnxruntime-${name}.XXXXXX")
}

install_staged() {
    local want="$1"
    local previous=""
    echo "${want}" >"${STAGED}/${STAMP_NAME}"
    chmod 755 "${STAGED}"

    if [ -L "${TARGET}" ]; then
        previous=$(readlink "${TARGET}")
    elif [ -e "${TARGET}" ]; then
        # A plain directory from an older install: moved aside, never deleted
        # here, and pruned like any superseded install next time.
        previous=$(basename "$(mktemp -d "${PREFIX}/onnxruntime-previous.XXXXXX")")
        mv -T "${TARGET}" "${PREFIX}/${previous}"
    fi

    LINK_TMP="${PREFIX}/.onnxruntime.link.$$"
    ln -sfn "$(basename "${STAGED}")" "${LINK_TMP}"
    mv -T "${LINK_TMP}" "${TARGET}"
    LINK_TMP=""
    local current
    current=$(basename "${STAGED}")
    STAGED=""

    local dir name
    for dir in "${PREFIX:?}"/onnxruntime-*.??????; do
        if [ ! -d "${dir}" ] || [ -L "${dir}" ]; then
            continue
        fi
        name=$(basename "${dir}")
        if [ "${name}" != "${current}" ] && [ "${name}" != "${previous}" ]; then
            rm -rf "${PREFIX:?}/${name:?}"
        fi
    done
}

already_installed() {
    local want="$1"
    [ "${FORCE}" -eq 0 ] && [ -f "${TARGET}/${STAMP_NAME}" ] && [ "$(cat "${TARGET}/${STAMP_NAME}")" = "${want}" ]
}

# The CUDA majors whose runtime (libcudart) the dynamic loader can find, or
# whose toolkit nvcc reports, one per line.
installed_cuda_runtimes() {
    local dir
    {
        ldconfig -p 2>/dev/null | grep -oP 'libcudart\.so\.\K[0-9]+' || true
        local dirs=()
        IFS=: read -ra dirs <<<"${LD_LIBRARY_PATH:-}"
        dirs+=("${CUDA_HOME:-/usr/local/cuda}/lib64" /usr/local/cuda/lib64 /usr/local/cuda-*/lib64 /usr/lib/x86_64-linux-gnu /usr/lib64)
        for dir in "${dirs[@]}"; do
            [ -n "${dir}" ] && [ -d "${dir}" ] || continue
            find "${dir}" -maxdepth 1 -name 'libcudart.so.*' -printf '%f\n' 2>/dev/null \
                | grep -oP '^libcudart\.so\.\K[0-9]+' || true
        done
        local nvcc
        for nvcc in "$(command -v nvcc 2>/dev/null || true)" "${CUDA_HOME:-/usr/local/cuda}/bin/nvcc"; do
            [ -n "${nvcc}" ] && [ -x "${nvcc}" ] || continue
            "${nvcc}" --version 2>/dev/null | grep -oP 'release \K[0-9]+' || true
        done
    } | sort -nu
}

# The newest CUDA major the NVIDIA driver supports, empty when nvidia-smi is
# missing or says nothing.
driver_cuda_major() {
    if command -v nvidia-smi >/dev/null 2>&1; then
        nvidia-smi 2>/dev/null | grep -oP 'CUDA Version:\s*\K[0-9]+' | head -1 || true
    fi
}

have_cudnn9() {
    local cache
    cache=$(ldconfig -p 2>/dev/null || true)
    [[ "${cache}" == *"libcudnn.so.9 "* ]] && return 0
    local dir dirs=()
    IFS=: read -ra dirs <<<"${LD_LIBRARY_PATH:-}"
    dirs+=("${CUDA_HOME:-/usr/local/cuda}/lib64" /usr/local/cuda/lib64 /usr/lib/x86_64-linux-gnu /usr/lib64)
    for dir in "${dirs[@]}"; do
        [ -n "${dir}" ] && [ -e "${dir}/libcudnn.so.9" ] && return 0
    done
    return 1
}

# Picks the CUDA build from the installed runtime: the newest of 12 and 13
# that is installed and that the driver supports.
select_cuda_major() {
    local driver runtimes major chosen=""
    driver=$(driver_cuda_major)
    runtimes=$(installed_cuda_runtimes)
    for major in ${runtimes}; do
        if [ "${major}" != 12 ] && [ "${major}" != 13 ]; then
            continue
        fi
        if [ -n "${driver}" ] && [ "${major}" -gt "${driver}" ]; then
            echo "  note:   the CUDA ${major} runtime is installed but the driver only supports CUDA ${driver}" >&2
            continue
        fi
        chosen="${major}"
    done
    if [ -z "${chosen}" ]; then
        if [ -n "${runtimes}" ]; then
            die "no installed CUDA runtime is one ONNX Runtime ${ORT_VERSION} is built for (12 or 13) and the driver supports (found: $(echo "${runtimes}" | tr '\n' ' ')); name one with --cuda 12 or --cuda 13"
        fi
        die "no CUDA runtime (libcudart or nvcc) found; install the CUDA 12 or 13 runtime, or name the build with --cuda 12 or --cuda 13"
    fi
    echo "${chosen}"
}

install_cuda() {
    if [ -z "${CUDA_MAJOR}" ]; then
        CUDA_MAJOR=$(select_cuda_major)
    else
        local driver
        driver=$(driver_cuda_major)
        if [ -n "${driver}" ] && [ "${CUDA_MAJOR}" -gt "${driver}" ]; then
            echo "warning: the NVIDIA driver supports CUDA ${driver}, older than the CUDA ${CUDA_MAJOR} build asked for" >&2
        fi
        local runtimes
        runtimes=$(installed_cuda_runtimes)
        if ! grep -qx "${CUDA_MAJOR}" <<<"${runtimes}"; then
            echo "warning: no CUDA ${CUDA_MAJOR} runtime (libcudart.so.${CUDA_MAJOR}) was found" >&2
        fi
    fi
    if [ -z "$(driver_cuda_major)" ]; then
        echo "warning: nvidia-smi reports no driver, so driver support for CUDA ${CUDA_MAJOR} is unchecked" >&2
    fi
    if ! have_cudnn9; then
        echo "warning: cuDNN 9 (libcudnn.so.9) was not found; the CUDA provider needs it at run time" >&2
    fi
    local sha256
    case "${CUDA_MAJOR}" in
        12) sha256="${CUDA12_SHA256}" ;;
        13) sha256="${CUDA13_SHA256}" ;;
        *) die "ONNX Runtime ${ORT_VERSION} is published for CUDA 12 and 13, not ${CUDA_MAJOR}" ;;
    esac
    local archive="onnxruntime-linux-x64-gpu_cuda${CUDA_MAJOR}-${ORT_VERSION}.tgz"
    local url="${ORT_RELEASES}/v${ORT_VERSION}/${archive}"
    local want="onnxruntime=${ORT_VERSION} cuda=${CUDA_MAJOR} sha256=${sha256}"

    echo "ONNX Runtime ${ORT_VERSION}, CUDA ${CUDA_MAJOR} build"
    echo "  needs:  an NVIDIA driver for CUDA ${CUDA_MAJOR}.x, the CUDA ${CUDA_MAJOR}.x runtime"
    echo "          libraries (cudart, cuBLAS, cuFFT, cuRAND) and cuDNN 9.x for CUDA ${CUDA_MAJOR},"
    echo "          all findable by the dynamic loader (ldconfig or LD_LIBRARY_PATH)"
    echo "  url:    ${url}"
    echo "  sha256: ${sha256}"
    echo "  target: ${TARGET}"
    if [ "${DRY_RUN}" -eq 1 ]; then
        return 0
    fi
    if already_installed "${want}"; then
        echo "Already installed; --force reinstalls."
        return 0
    fi
    confirm

    command -v curl >/dev/null 2>&1 || die "curl is required"
    lock_prefix
    make_staging_dir "${ORT_VERSION}-cuda${CUDA_MAJOR}"
    DOWNLOAD="${STAGED}.tgz"
    curl -fL --retry 3 --progress-bar -o "${DOWNLOAD}" "${url}"
    echo "${sha256}  ${DOWNLOAD}" | sha256sum -c --quiet - || die "${url} does not match the pinned sha256"
    tar -xzf "${DOWNLOAD}" -C "${STAGED}" --strip-components=1
    [ -e "${STAGED}/lib/libonnxruntime.so.1" ] || die "the archive has no lib/libonnxruntime.so.1"
    [ -f "${STAGED}/lib/libonnxruntime_providers_cuda.so" ] || die "the archive has no CUDA execution provider"
    install_staged "${want}"
    echo "Installed to ${TARGET}."
}

install_rocm() {
    local rocm_home="${ROCM_HOME:-/opt/rocm}"
    local rocm_version="" rocm_mm=""
    if [ -f "${rocm_home}/.info/version" ]; then
        rocm_version=$(cat "${rocm_home}/.info/version")
    elif command -v hipconfig >/dev/null 2>&1; then
        rocm_version=$(hipconfig --version 2>/dev/null || true)
    fi
    rocm_mm=$(echo "${rocm_version}" | grep -oP '^[0-9]+\.[0-9]+' || true)
    [ -n "${rocm_mm}" ] || die "no ROCm found at ${rocm_home} (set ROCM_HOME); install ROCm 6.2 or newer with MIGraphX"

    local version="" commit="" newest=""
    newest=$(echo "${ROCM_TABLE}" | awk 'NF { print $1; exit }')
    if [ "$(printf '%s\n%s\n' "${rocm_mm}" "${newest}" | sort -V | tail -1)" != "${newest}" ]; then
        version="${ORT_VERSION}"
        commit="${ORT_VERSION_COMMIT}"
    else
        read -r version commit < <(echo "${ROCM_TABLE}" | awk -v r="${rocm_mm}" '$1 == r { print $2, $3 }') || true
        [ -n "${version}" ] || die "ROCm ${rocm_mm} is not supported; Natron needs ROCm 6.2 or newer"
    fi
    local want="onnxruntime=${version} migraphx rocm=${rocm_mm} commit=${commit}"

    echo "ONNX Runtime ${version}, built from source with MIGraphX for ROCm ${rocm_version}"
    echo "  needs:  ROCm ${rocm_mm} with MIGraphX (libmigraphx) in ${rocm_home}, and to build:"
    echo "          git, cmake, g++ and python3; about 2 GB of disk and 30-60 minutes"
    echo "  source: ${ORT_REPO} v${version} (commit ${commit})"
    echo "  target: ${TARGET}"
    if [ "${DRY_RUN}" -eq 1 ]; then
        return 0
    fi
    if already_installed "${want}"; then
        echo "Already installed; --force reinstalls."
        return 0
    fi
    if [ ! -e "${rocm_home}/lib/libmigraphx.so" ] && [ ! -e "${rocm_home}/lib64/libmigraphx.so" ]; then
        die "MIGraphX is not installed in ${rocm_home} (the migraphx and migraphx-dev/-devel packages)"
    fi
    local tool missing=""
    for tool in git cmake g++ python3; do
        command -v "${tool}" >/dev/null 2>&1 || missing="${missing} ${tool}"
    done
    [ -z "${missing}" ] || die "missing build tools:${missing}"
    confirm

    lock_prefix
    BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/natron-ort-migraphx.XXXXXX")
    git clone --depth 1 --branch "v${version}" "${ORT_REPO}" "${BUILD_DIR}/onnxruntime"
    local head
    head=$(git -C "${BUILD_DIR}/onnxruntime" rev-parse HEAD)
    [ "${head}" = "${commit}" ] || die "v${version} is commit ${head}, not the pinned ${commit}"
    (
        cd "${BUILD_DIR}/onnxruntime"
        ./build.sh --config Release --build_shared_lib --parallel --skip_tests \
            --use_migraphx --migraphx_home "${rocm_home}"
    )
    local built="${BUILD_DIR}/onnxruntime/build/Linux/Release"
    [ -e "${built}/libonnxruntime.so.1" ] || [ -e "${built}/libonnxruntime.so.${version}" ] \
        || die "the build produced no libonnxruntime.so"

    make_staging_dir "${version}-migraphx"
    mkdir -p "${STAGED}/lib"
    cp -P "${built}"/libonnxruntime*.so* "${STAGED}/lib/"
    cp "${BUILD_DIR}/onnxruntime/LICENSE" "${BUILD_DIR}/onnxruntime/ThirdPartyNotices.txt" "${STAGED}/"
    install_staged "${want}"
    echo "Installed to ${TARGET}."
}

echo "Installing a GPU ONNX Runtime for Natron"
case "${MODE}" in
    cuda) install_cuda ;;
    rocm) install_rocm ;;
esac

if [ "${DRY_RUN}" -eq 0 ]; then
    echo ""
    echo "In Natron: Preferences > AI, turn on AI Enabled, set AI Device to Auto (or the GPU),"
    echo "leave AI ORT Library Path empty, and restart Natron. AI Detected Providers lists the"
    echo "execution providers the installed library offers."
fi
