#!/usr/bin/env bash
# Reports which GL/Vulkan hand-off rung the viewer's Qt GL context reaches on this
# desktop session. Run it on the host (not in a container) inside an X11 or Wayland
# session, then paste back handoff-report.txt.
set -u

usage() {
    cat <<USAGE
usage: ${0##*/} --glinterop <GlInteropQt binary | directory to search> [--out <dir>]
                [--appimage <file>] [--comp <file>]

  --glinterop  the GlInteropQt probe, or a directory searched recursively for it
  --out        where the report and raw logs go (default: a new temp directory)
  --appimage, --comp   reserved; accepted but not yet supported
USAGE
}

glinterop=""
out=""
appimage=""
comp=""
while [ $# -gt 0 ]; do
    case "$1" in
        --glinterop) glinterop="${2:-}"; shift 2 || shift ;;
        --out) out="${2:-}"; shift 2 || shift ;;
        --appimage) appimage="${2:-}"; shift 2 || shift ;;
        --comp) comp="${2:-}"; shift 2 || shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$glinterop" ]; then
    usage >&2
    exit 2
fi

if [ -z "$out" ]; then
    out=$(mktemp -d -t handoff-check.XXXXXX) || exit 2
else
    mkdir -p "$out" || exit 2
fi
report="$out/handoff-report.txt"
body="$out/.body.txt"
: > "$body"

say() { printf '%s\n' "$*" >> "$body"; }
finish() {
    { printf '%s\n' "$1"; cat "$body"; } > "$report"
    rm -f "$body"
    cat "$report"
    echo
    echo "report: $report"
    exit "${2:-0}"
}

if [ -f /.dockerenv ] || [ -f /run/.containerenv ]; then
    say "WARNING: running inside a container; GL is probably not the desktop's GPU."
fi

bin=""
if [ -d "$glinterop" ]; then
    bin=$(find "$glinterop" -type f -name GlInteropQt -perm -u+x 2>/dev/null | head -n 1)
elif [ -f "$glinterop" ]; then
    bin="$glinterop"
fi
if [ -z "$bin" ] || [ ! -x "$bin" ]; then
    say "GlInteropQt not found or not executable at: $glinterop"
    finish "VERDICT no result: GlInteropQt binary missing" 1
fi
bin=$(readlink -f "$bin")

say "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "kernel: $(uname -sr)"
say "probe: $bin"
say "LD_LIBRARY_PATH (host, not passed to the probe): ${LD_LIBRARY_PATH:-(unset)}"
say ""
say "== session =="
say "XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-(unset)}"
say "DISPLAY=${DISPLAY:-(unset)}"
say "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-(unset)}"

if [ -n "$appimage" ] || [ -n "$comp" ]; then
    say "--appimage/--comp: not yet supported, ignored"
fi

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    say ""
    say "No DISPLAY or WAYLAND_DISPLAY: run this from a terminal inside a desktop session."
    finish "VERDICT no result: no display" 1
fi

say ""
say "== GL renderer =="
gl_text=""
if command -v glxinfo >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
    gl_text=$(env -u LD_LIBRARY_PATH glxinfo -B 2>&1)
    say "(glxinfo -B)"
elif command -v eglinfo >/dev/null 2>&1; then
    gl_text=$(env -u LD_LIBRARY_PATH eglinfo -B 2>&1 || env -u LD_LIBRARY_PATH eglinfo 2>&1)
    say "(eglinfo)"
else
    say "neither glxinfo nor eglinfo found (mesa-utils / mesa-utils-bin)"
fi
if [ -n "$gl_text" ]; then
    printf '%s\n' "$gl_text" > "$out/gl-info.log"
    say "$(printf '%s\n' "$gl_text" | grep -i -E 'renderer|vendor|OpenGL version|OpenGL core' | head -n 12)"
fi

say ""
say "== Vulkan device =="
if command -v vulkaninfo >/dev/null 2>&1; then
    env -u LD_LIBRARY_PATH vulkaninfo --summary > "$out/vulkaninfo.log" 2>&1
    say "$(grep -E 'deviceName|driverName|driverInfo|deviceType|apiVersion' "$out/vulkaninfo.log" | head -n 20)"
else
    say "vulkaninfo not found (vulkan-tools)"
fi

if ! printf '%s\n' "$gl_text" | grep -qi radeonsi; then
    say ""
    say "WARNING: GL renderer is not radeonsi; a zero-copy result against RADV is unlikely."
fi

say ""
say "== ldd =="
ldd_out=$(env -u LD_LIBRARY_PATH ldd "$bin" 2>&1)
printf '%s\n' "$ldd_out" > "$out/ldd.log"
notfound=$(printf '%s\n' "$ldd_out" | grep 'not found')
if [ -n "$notfound" ]; then
    say "$notfound"
    say "Missing shared libraries; install them or run from the build's environment."
    finish "VERDICT no result: GlInteropQt cannot start (missing shared libraries)" 1
fi
say "all shared libraries resolved"

platforms=()
[ -n "${DISPLAY:-}" ] && platforms+=(xcb)
[ -n "${WAYLAND_DISPLAY:-}" ] && platforms+=(wayland)

verdict=""
best_summary=""
for plat in "${platforms[@]}"; do
    log="$out/glinterop-$plat.log"
    env -u LD_LIBRARY_PATH QT_QPA_PLATFORM="$plat" timeout 120 "$bin" > "$log" 2>&1
    rc=$?
    result=$(grep '^RESULT ' "$log" | tail -n 1)
    missing=$(grep -E '^(missing|zero-copy blockers):' "$log" | tail -n 1)
    best=$(printf '%s\n' "$result" | sed -n 's/^RESULT best=\([^ ]*\).*/\1/p')
    # Older probe builds print only a "path:" line and no RESULT line.
    [ -n "$best" ] || best=$(sed -n 's/^path: *//p' "$log" | tail -n 1)
    say ""
    say "== GlInteropQt ($plat) =="
    say "exit code: $rc"
    say "${result:-(no RESULT line)}"
    say "${missing:-(no missing line)}"
    if [ -z "$best" ]; then
        say "--- last output ---"
        say "$(tail -n 15 "$log")"
    fi
    case "$best" in
        zero-copy|zero-copy-hostsync)
            if [ -z "$verdict" ]; then verdict="VERDICT zero-copy reached ($plat, best=$best)"; fi ;;
    esac
    best_summary="${best_summary:+$best_summary; }$plat: best=${best:-none} ${missing#*: }"
done

say ""
say "full logs: $out"
if [ -n "$verdict" ]; then
    finish "$verdict" 0
fi
finish "VERDICT no zero-copy: $best_summary" 1
