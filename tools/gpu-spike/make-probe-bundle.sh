#!/usr/bin/env bash
# Packs the GlInteropQt probe with the container's Qt6 into a directory and a
# tarball that run on a host with no Qt installed, for desktop-handoff-check.sh.
# GL, Vulkan, DRM and C library files are never bundled: the host's drivers
# must be the ones the probe talks to.
#
#   tools/gpu-spike/make-probe-bundle.sh [--probe <GlInteropQt>] [--out <dir>]
#
# Run it from the host; it re-execs itself inside the dev container.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
EXCLUDELIST="${REPO_ROOT}/tools/release/excludelist.txt"

if [[ "${NATRON_IN_CONTAINER:-}" != "1" ]]; then
    exec "${REPO_ROOT}/tools/ci/local/devshell.sh" "${REPO_ROOT}/tools/gpu-spike/make-probe-bundle.sh" "$@"
fi

probe="${REPO_ROOT}/build/gpu-spike/GlInteropQt"
out="${REPO_ROOT}/build/gpu-spike/probe-bundle"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --probe) probe="${2:?--probe needs a path}"; shift 2 ;;
        --out) out="${2:?--out needs a path}"; shift 2 ;;
        -h|--help) sed -n 2,8p "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

for tool in patchelf ldd tar; do
    command -v "$tool" >/dev/null || { echo "error: $tool not found in PATH" >&2; exit 1; }
done
[[ -x "$probe" ]] || { echo "error: probe not built: $probe" >&2; exit 1; }

qt_plugins="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
[[ -d "$qt_plugins" ]] || { echo "error: cannot determine the Qt6 plugin directory" >&2; exit 1; }

declare -a EXCLUDE_PATTERNS=()
while IFS= read -r line; do
    line="${line%%#*}"
    line="${line// /}"
    [[ -n "$line" ]] && EXCLUDE_PATTERNS+=("$line")
done < "$EXCLUDELIST"
# The host's own C++ runtime is what its Mesa was built against.
EXCLUDE_PATTERNS+=(libstdc++.so.6 libgcc_s.so.1 'libvulkan.so*')

is_excluded() {
    local pattern
    for pattern in "${EXCLUDE_PATTERNS[@]}"; do
        # shellcheck disable=SC2254
        case "$1" in $pattern) return 0 ;; esac
    done
    return 1
}

rm -rf "$out"
mkdir -p "$out/lib" "$out/plugins"
out="$(cd "$out" && pwd)"
cp -L "$probe" "$out/GlInteropQt"

for sub in platforms xcbglintegrations wayland-graphics-integration-client \
           wayland-shell-integration wayland-decoration-client; do
    if [[ -d "$qt_plugins/$sub" ]]; then
        mkdir -p "$out/plugins/$sub"
        cp -L "$qt_plugins/$sub"/*.so "$out/plugins/$sub/"
    fi
done
# Offscreen/minimal/linuxfb/vnc cannot reach a desktop GL context.
rm -f "$out"/plugins/platforms/lib{qoffscreen,qminimal,qlinuxfb,qvnc,qvkkhrdisplay}.so

printf '[Paths]\nPrefix = .\nPlugins = plugins\n' > "$out/qt.conf"

declare -A SEEN=()
queue=("$out/GlInteropQt")
while IFS= read -r plugin; do queue+=("$plugin"); done < <(find "$out/plugins" -name '*.so')
while [[ ${#queue[@]} -gt 0 ]]; do
    elf="${queue[0]}"
    queue=("${queue[@]:1}")
    while IFS= read -r dep; do
        name="$(basename "$dep")"
        is_excluded "$name" && continue
        [[ -n "${SEEN[$name]:-}" ]] && continue
        SEEN[$name]=1
        cp -L "$dep" "$out/lib/$name"
        queue+=("$out/lib/$name")
    done < <(ldd "$elf" | sed -n 's/.* => \(\/[^ ]*\) .*/\1/p')
done

patchelf --set-rpath '$ORIGIN/lib' "$out/GlInteropQt"
for lib in "$out"/lib/*; do
    patchelf --set-rpath '$ORIGIN' "$lib"
done
while IFS= read -r plugin; do
    depth="$(realpath --relative-to="$(dirname "$plugin")" "$out/lib")"
    patchelf --set-rpath "\$ORIGIN/$depth" "$plugin"
done < <(find "$out/plugins" -name '*.so')

if ! ls "$out"/plugins/platforms/libqwayland*.so >/dev/null 2>&1; then
    echo "note: this Qt has no wayland platform plugin; the probe will run under xcb (XWayland) only" >&2
fi

tarball="${out}.tar.gz"
tar -C "$(dirname "$out")" -czf "$tarball" "$(basename "$out")"
echo "bundle:  $out"
echo "tarball: $tarball"
du -sh "$out" "$tarball"
