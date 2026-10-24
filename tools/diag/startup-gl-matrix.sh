#!/usr/bin/env bash
# Usage: tools/diag/startup-gl-matrix.sh <Natron.AppImage> [options]
#
# Launches Natron repeatedly to find what makes Qt's compositor fail to compile
# its shaders at startup. Each launch quits by itself once the main window has
# been exposed and has had time to paint; a crash, a shader-compile failure or a
# hang is recorded instead.
#
# Modes:
#   --mode capture     Plain launches against your real caches. Before each one,
#                      the GL/Qt/driver caches and Natron's settings are copied to
#                      a snapshot; the snapshot taken just before the first failing
#                      launch is kept and the run stops there.
#   --mode matrix      Interleaved launches over every configuration: baseline,
#                      each NATRON_DIAG_* switch, NATRON_GPU=off,
#                      QT_DISABLE_SHADER_DISK_CACHE=1, __GL_SHADER_DISK_CACHE=0 and
#                      a fresh XDG_CACHE_HOME.
#   --mode replay <snapshot>
#                      Restores a snapshot (a directory, or the tarball written by
#                      capture) into private XDG_CACHE_HOME/XDG_CONFIG_HOME
#                      directories before every launch, runs the matrix on it, and
#                      adds runs with only one store restored (Qt shader cache,
#                      Natron's other caches, driver caches, settings file).
#
# Options:
#   --runs N           Launches per configuration (matrix/replay, default 5) or
#                      the maximum number of launches (capture, default 20).
#   --timeout S        Seconds before a launch counts as hung (default 25).
#   --settle-ms MS     Time the window stays up after its first expose (default 3000).
#
# Everything is written to a new directory under the current one and packed into
# one tarball whose path is printed at the end. Your own caches and settings are
# only ever read and copied; capture mode changes nothing else. Matrix mode runs
# Natron against your real caches, exactly as a normal launch does.

set -u

usage() {
    sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

APPIMAGE=""
MODE="matrix"
SNAPSHOT_IN=""
RUNS=""
TIMEOUT_S=25
SETTLE_MS=3000

while [ $# -gt 0 ]; do
    case "$1" in
        --mode)
            [ $# -ge 2 ] || usage
            MODE="$2"
            shift 2
            if [ "$MODE" = "replay" ]; then
                [ $# -ge 1 ] || { echo "replay needs a snapshot directory or tarball" >&2; exit 2; }
                SNAPSHOT_IN="$1"
                shift
            fi
            ;;
        --runs)
            [ $# -ge 2 ] || usage
            RUNS="$2"
            shift 2
            ;;
        --timeout)
            [ $# -ge 2 ] || usage
            TIMEOUT_S="$2"
            shift 2
            ;;
        --settle-ms)
            [ $# -ge 2 ] || usage
            SETTLE_MS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        -*)
            echo "unknown option: $1" >&2
            usage
            ;;
        *)
            if [ -n "$APPIMAGE" ]; then
                echo "unexpected argument: $1" >&2
                usage
            fi
            APPIMAGE="$1"
            shift
            ;;
    esac
done

[ -n "$APPIMAGE" ] || usage
case "$MODE" in
    capture|matrix|replay) ;;
    *) echo "unknown mode: $MODE" >&2; usage ;;
esac
if [ -z "$RUNS" ]; then
    if [ "$MODE" = "capture" ]; then RUNS=20; else RUNS=5; fi
fi
for n in "$RUNS" "$TIMEOUT_S" "$SETTLE_MS"; do
    case "$n" in
        ''|*[!0-9]*) echo "not a number: $n" >&2; exit 2 ;;
    esac
done
if [ ! -f "$APPIMAGE" ]; then
    echo "no such file: $APPIMAGE" >&2
    exit 2
fi
APPIMAGE="$(cd "$(dirname "$APPIMAGE")" && pwd)/$(basename "$APPIMAGE")"
if [ ! -x "$APPIMAGE" ]; then
    echo "$APPIMAGE is not executable (chmod +x it first)" >&2
    exit 2
fi
for tool in timeout find sha1sum tar cp du df diff; do
    command -v "$tool" > /dev/null 2>&1 || { echo "missing tool: $tool" >&2; exit 2; }
done

HOME_CACHE="$HOME/.cache"
CACHE_ROOT="${XDG_CACHE_HOME:-$HOME_CACHE}"
CONFIG_ROOT="${XDG_CONFIG_HOME:-$HOME/.config}"
NV_PATH="${__GL_SHADER_DISK_CACHE_PATH:-}"

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$(mktemp -d "$PWD/startup-gl-$MODE-$STAMP.XXXXXX")" || { echo "cannot create an output directory in $PWD" >&2; exit 2; }
mkdir -p "${OUT:?}/logs" "${OUT:?}/listings"
RESULTS="$OUT/results.tsv"
SUMMARY="$OUT/summary.txt"
printf 'seq\tconfig\trun\texit\tclass\tseconds\texposed\tmismatch\tqt_stale\tcache_changes\n' > "$RESULTS"

# Files above this size are listed without a hash, so a large image disk cache
# does not dominate the run time.
MAX_HASH_BYTES=$((64 * 1024 * 1024))

QUIT_SCRIPT="$OUT/autoquit.py"
cat > "$QUIT_SCRIPT" <<'PY'
import os
import sys

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication

_settle_ms = int(os.environ.get("NATRON_DIAG_SETTLE_MS", "3000"))


def _say(msg):
    # Natron replaces sys.stderr with a catcher that has no flush(), and its output
    # reaches the log late; write to the real descriptor.
    os.write(2, ("[startup-gl-matrix] %s\n" % msg).encode())


def _main_window():
    for w in QApplication.topLevelWidgets():
        if w.inherits("QMainWindow") and w.isVisible():
            return w
    return None


def _instance():
    g = globals()
    for name in ("app", "app1"):
        if g.get(name) is not None:
            return g[name]
    import NatronEngine
    return NatronEngine.natron.getInstance(0)


def _force_exit():
    _say("forced exit")
    os._exit(0)


def _quit():
    _say("quitting")
    QTimer.singleShot(5000, _force_exit)
    try:
        _instance().closeProject()
    except Exception as e:
        _say("closeProject failed: %r" % (e,))
        _force_exit()


def _poll():
    w = _main_window()
    handle = w.windowHandle() if w is not None else None
    if handle is not None and handle.isExposed():
        _say("exposed")
        QTimer.singleShot(_settle_ms, _quit)
        return
    QTimer.singleShot(100, _poll)


_say("autoquit armed")
QTimer.singleShot(0, _poll)
PY
QUIT_CMD="import os; exec(open(os.environ['NATRON_DIAG_QUIT_SCRIPT']).read())"

say() {
    printf '%s\n' "$*" | tee -a "$SUMMARY"
}

# Roots whose files are listed before and after every launch.
cache_roots() {
    local cache="$1" config="$2" nv="$3"
    printf '%s\n' "$cache/INRIA" "$cache/nvidia" "$cache/mesa_shader_cache" "$cache/mesa_shader_cache_db" "$config/INRIA"
    if [ -n "$nv" ]; then
        printf '%s\n' "$nv"
    fi
    if [ "$cache" != "$HOME_CACHE" ]; then
        printf '%s\n' "$HOME_CACHE/INRIA" "$HOME_CACHE/nvidia"
    fi
    printf '%s\n' "$HOME/.nv"
}

# Writes "path<TAB>size<TAB>mtime<TAB>sha1" for every file under the roots on stdin.
list_files() {
    local out="$1" root size mtime path sha
    : > "$out"
    while IFS= read -r root; do
        if [ ! -e "$root" ]; then
            printf '%s\t-\t-\tmissing\n' "$root" >> "$out"
            continue
        fi
        find "$root" -type f -printf '%s\t%TY-%Tm-%Td %TT\t%p\n' 2> /dev/null | sort -t "$(printf '\t')" -k3,3 |
            while IFS="$(printf '\t')" read -r size mtime path; do
                if [ "$size" -le "$MAX_HASH_BYTES" ]; then
                    sha="$(sha1sum < "$path" 2> /dev/null | cut -c1-40)"
                    [ -n "$sha" ] || sha="unreadable"
                else
                    sha="not-hashed"
                fi
                printf '%s\t%s\t%s\t%s\n' "$path" "$size" "$mtime" "$sha"
            done >> "$out"
    done
}

dir_bytes() {
    local total=0 b p
    for p in "$@"; do
        if [ -e "$p" ]; then
            b="$(du -sb "$p" 2> /dev/null | cut -f1)"
            total=$((total + ${b:-0}))
        fi
    done
    printf '%s\n' "$total"
}

free_bytes() {
    df -B1 --output=avail "$1" 2> /dev/null | tail -n 1 | tr -d ' '
}

copy_if_present() {
    local src="$1" dst="$2"
    if [ -e "$src" ]; then
        mkdir -p "$(dirname "$dst")"
        cp -a --reflink=auto "$src" "$dst"
    fi
}

take_snapshot() {
    local dst="$1"
    mkdir -p "$dst"
    copy_if_present "$CACHE_ROOT/INRIA" "$dst/cache/INRIA"
    copy_if_present "$CACHE_ROOT/nvidia" "$dst/cache/nvidia"
    copy_if_present "$CACHE_ROOT/mesa_shader_cache" "$dst/cache/mesa_shader_cache"
    copy_if_present "$CACHE_ROOT/mesa_shader_cache_db" "$dst/cache/mesa_shader_cache_db"
    copy_if_present "$CONFIG_ROOT/INRIA" "$dst/config/INRIA"
    if [ -n "$NV_PATH" ]; then
        copy_if_present "$NV_PATH" "$dst/nv-path"
    fi
    if [ "$CACHE_ROOT" != "$HOME_CACHE" ]; then
        copy_if_present "$HOME_CACHE/INRIA" "$dst/home-cache/INRIA"
        copy_if_present "$HOME_CACHE/nvidia" "$dst/home-cache/nvidia"
    fi
    copy_if_present "$HOME/.nv" "$dst/nv-legacy"
    {
        printf 'taken\t%s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
        printf 'cache_root\t%s\n' "$CACHE_ROOT"
        printf 'config_root\t%s\n' "$CONFIG_ROOT"
        printf 'nv_path\t%s\n' "$NV_PATH"
    } > "$dst/snapshot-info.tsv"
}

classify() {
    local rc="$1" log="$2"
    if grep -q 'Caught segmentation fault' "$log" || [ "$rc" -eq 139 ] || [ "$rc" -eq $((128 + 11)) ]; then
        echo crash
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        if grep -qE 'Failed to compile shader|Failed to build graphics pipeline' "$log"; then
            echo shader-fail-hang
        else
            echo timeout
        fi
    elif grep -qE 'Failed to compile shader|Failed to build graphics pipeline' "$log"; then
        echo shader-fail
    elif [ "$rc" -eq 0 ] && grep -q '^\[startup-gl-matrix\] quitting' "$log"; then
        echo clean
    elif [ "$rc" -gt 128 ]; then
        echo "signal-$((rc - 128))"
    else
        echo "exit-$rc"
    fi
}

SEQ=0
LAST_CLASS=""

# run_launch <config> <run> <cache-root> <config-root> <nv-path> [VAR=value ...]
run_launch() {
    local cfg="$1" run="$2" cache="$3" config="$4" nv="$5"
    shift 5
    SEQ=$((SEQ + 1))
    local tag
    tag="$(printf '%03d' "$SEQ")-$cfg-$run"
    local log="$OUT/logs/$tag.log"
    local before="$OUT/listings/$tag.before.tsv" after="$OUT/listings/$tag.after.tsv"

    cache_roots "$cache" "$config" "$nv" | list_files "$before"

    local start end rc
    start="$(date +%s)"
    {
        printf '# config=%s run=%s seq=%s\n# env:' "$cfg" "$run" "$SEQ"
        printf ' %s' "$@"
        printf '\n'
    } > "$log"
    # The braces take bash's own "Segmentation fault" job report off the terminal.
    {
        env -u NATRON_DIAG_SKIP_GL_PROBE -u NATRON_DIAG_SKIP_VK_ENUM -u NATRON_DIAG_GPU_INIT_SYNC \
            -u NATRON_DIAG_NO_SPLASH_PUMP -u NATRON_DIAG_GL_FORCE_PROBE_CONTEXT \
            NATRON_DIAG_QUIT_SCRIPT="$QUIT_SCRIPT" NATRON_DIAG_SETTLE_MS="$SETTLE_MS" \
            "$@" \
            timeout -k 5 "$TIMEOUT_S" "$APPIMAGE" -c "$QUIT_CMD" >> "$log" 2>&1 < /dev/null
    } 2> /dev/null
    rc=$?
    end="$(date +%s)"

    cache_roots "$cache" "$config" "$nv" | list_files "$after"
    local changes
    changes="$(diff "$before" "$after" | grep -c '^[<>]')"
    diff "$before" "$after" > "$OUT/listings/$tag.diff"

    local cls exposed mismatch stale
    cls="$(classify "$rc" "$log")"
    if grep -q '^\[startup-gl-matrix\] exposed' "$log"; then exposed=yes; else exposed=no; fi
    mismatch="$(grep -c 'MISMATCH' "$log")"
    stale="$(grep -c 'QT-STALE' "$log")"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$SEQ" "$cfg" "$run" "$rc" "$cls" "$((end - start))" "$exposed" "$mismatch" "$stale" "$changes" >> "$RESULTS"
    printf '  #%-3s %-22s run %-2s %-16s exit=%-4s %3ss exposed=%-3s mismatch=%s qt-stale=%s cache-changes=%s\n' \
        "$SEQ" "$cfg" "$run" "$cls" "$rc" "$((end - start))" "$exposed" "$mismatch" "$stale" "$changes" | tee -a "$SUMMARY"
    LAST_CLASS="$cls"
}

DIAG_ENV=(NATRON_DIAG_GL=1 "QT_LOGGING_RULES=qt.rhi.*=true;qt.opengl.diskcache=true")

# name|VAR=value ... ; @FRESH_CACHE@ becomes a new empty directory per launch.
MATRIX_CONFIGS=(
    "baseline|"
    "skip-gl-probe|NATRON_DIAG_SKIP_GL_PROBE=1"
    "skip-vk-enum|NATRON_DIAG_SKIP_VK_ENUM=1"
    "gpu-init-sync|NATRON_DIAG_GPU_INIT_SYNC=1"
    "no-splash-pump|NATRON_DIAG_NO_SPLASH_PUMP=1"
    "force-probe-context|NATRON_DIAG_GL_FORCE_PROBE_CONTEXT=1"
    "gpu-off|NATRON_GPU=off"
    "qt-no-diskcache|QT_DISABLE_SHADER_DISK_CACHE=1"
    "nv-no-diskcache|__GL_SHADER_DISK_CACHE=0"
    "fresh-xdg-cache|XDG_CACHE_HOME=@FRESH_CACHE@"
)

config_env() {
    local spec="$1" fresh="$2" word
    local rest="${spec#*|}"
    for word in $rest; do
        printf '%s\n' "${word//@FRESH_CACHE@/$fresh}"
    done
}

print_table() {
    say ""
    say "config                  launches  clean  crash  shader-fail  timeout  other  mismatch-launches  qt-stale-launches"
    awk -F '\t' 'NR > 1 {
            n[$2]++
            if ($5 == "clean") c[$2]++
            else if ($5 == "crash") k[$2]++
            else if ($5 ~ /^shader-fail/) s[$2]++
            else if ($5 == "timeout") t[$2]++
            else o[$2]++
            if ($8 > 0) m[$2]++
            if ($9 > 0) q[$2]++
            if (!($2 in seen)) { seen[$2] = 1; order[++cnt] = $2 }
        }
        END {
            for (i = 1; i <= cnt; i++) {
                x = order[i]
                printf "%-23s %9d %6d %6d %12d %8d %6d %18d %18d\n", x, n[x], c[x], k[x], s[x], t[x], o[x], m[x], q[x]
            }
        }' "$RESULTS" | tee -a "$SUMMARY"
}

write_host_info() {
    {
        echo "date: $(date '+%Y-%m-%d %H:%M:%S %z')"
        echo "mode: $MODE runs: $RUNS timeout: ${TIMEOUT_S}s settle: ${SETTLE_MS}ms"
        echo "appimage: $APPIMAGE"
        echo "appimage sha1: $(sha1sum < "$APPIMAGE" | cut -c1-40)"
        echo "uname: $(uname -a)"
        echo "cache root: $CACHE_ROOT"
        echo "config root: $CONFIG_ROOT"
        echo "__GL_SHADER_DISK_CACHE_PATH: ${NV_PATH:-unset}"
        echo "XDG_SESSION_TYPE: ${XDG_SESSION_TYPE:-unset} DISPLAY: ${DISPLAY:-unset} WAYLAND_DISPLAY: ${WAYLAND_DISPLAY:-unset}"
        if command -v nvidia-smi > /dev/null 2>&1; then
            nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>&1
        fi
        if command -v glxinfo > /dev/null 2>&1; then
            glxinfo -B 2>&1 | head -n 20
        fi
        if [ -f "$HOME/.Natron/initGui.py" ]; then
            echo "~/.Natron/initGui.py present"
        else
            echo "~/.Natron/initGui.py absent"
        fi
    } > "$OUT/host-info.txt" 2>&1
}

finish() {
    print_table
    local tarball="$PWD/$(basename "$OUT").tar.gz"
    tar -czf "$tarball" -C "$(dirname "$OUT")" "$(basename "$OUT")"
    echo ""
    echo "results: $OUT"
    echo "tarball: $tarball"
}

run_capture() {
    local roots_bytes free i snap
    say "capture: up to $RUNS plain launches; stops at the first failing one"
    for i in $(seq 1 "$RUNS"); do
        roots_bytes="$(dir_bytes "$CACHE_ROOT/INRIA" "$CACHE_ROOT/nvidia" "$CACHE_ROOT/mesa_shader_cache" "$CACHE_ROOT/mesa_shader_cache_db" "$CONFIG_ROOT/INRIA" ${NV_PATH:+"$NV_PATH"} "$HOME/.nv")"
        free="$(free_bytes "$OUT")"
        if [ -n "$free" ] && [ "$free" -lt $((2 * roots_bytes + 100 * 1024 * 1024)) ]; then
            say "stopping: a snapshot needs about $roots_bytes bytes and only $free are free in $OUT"
            break
        fi
        snap="$OUT/snapshots/pre-$i"
        take_snapshot "$snap"
        run_launch "plain" "$i" "$CACHE_ROOT" "$CONFIG_ROOT" "$NV_PATH" NATRON_DIAG_GL=1
        case "$LAST_CLASS" in
            clean)
                rm -rf "${OUT:?}/snapshots/pre-$i"
                ;;
            *)
                mv "$snap" "$OUT/pre-crash-snapshot"
                say "launch $i failed ($LAST_CLASS); the state from just before it is in $OUT/pre-crash-snapshot"
                say "replay it with: $0 $APPIMAGE --mode replay <the tarball printed below>"
                return
                ;;
        esac
    done
    say "no failing launch in this capture; no snapshot was kept"
}

run_matrix() {
    local round spec cfg fresh
    local -a extra
    say "matrix: $RUNS launches per configuration, interleaved by round, against your real caches"
    for round in $(seq 1 "$RUNS"); do
        for spec in "${MATRIX_CONFIGS[@]}"; do
            cfg="${spec%%|*}"
            fresh="$OUT/fresh-cache/$cfg-$round"
            mkdir -p "$fresh"
            mapfile -t extra < <(config_env "$spec" "$fresh")
            if [ "$cfg" = "fresh-xdg-cache" ]; then
                run_launch "$cfg" "$round" "$fresh" "$CONFIG_ROOT" "$NV_PATH" "${DIAG_ENV[@]}" ${extra[@]+"${extra[@]}"}
            else
                run_launch "$cfg" "$round" "$CACHE_ROOT" "$CONFIG_ROOT" "$NV_PATH" "${DIAG_ENV[@]}" ${extra[@]+"${extra[@]}"}
            fi
        done
    done
}

# restore_store <snapshot> <private-root> <store>
restore_store() {
    local snap="$1" priv="$2" store="$3" d
    case "$store" in
        qt)
            for d in "$snap"/cache/INRIA/Natron/qtshadercache-*; do
                [ -e "$d" ] || continue
                copy_if_present "$d" "$priv/cache/INRIA/Natron/$(basename "$d")"
            done
            ;;
        natron)
            if [ -d "$snap/cache/INRIA" ]; then
                mkdir -p "$priv/cache/INRIA"
                for d in "$snap"/cache/INRIA/* "$snap"/cache/INRIA/.[!.]*; do
                    [ -e "$d" ] || continue
                    if [ "$(basename "$d")" = "Natron" ]; then
                        mkdir -p "$priv/cache/INRIA/Natron"
                        local e
                        for e in "$d"/* "$d"/.[!.]*; do
                            [ -e "$e" ] || continue
                            case "$(basename "$e")" in
                                qtshadercache-*) ;;
                                *) copy_if_present "$e" "$priv/cache/INRIA/Natron/$(basename "$e")" ;;
                            esac
                        done
                    else
                        copy_if_present "$d" "$priv/cache/INRIA/$(basename "$d")"
                    fi
                done
            fi
            ;;
        driver)
            if [ -d "$snap/nv-path" ]; then
                copy_if_present "$snap/nv-path" "$priv/cache/nvidia"
            else
                copy_if_present "$snap/cache/nvidia" "$priv/cache/nvidia"
            fi
            copy_if_present "$snap/cache/mesa_shader_cache" "$priv/cache/mesa_shader_cache"
            copy_if_present "$snap/cache/mesa_shader_cache_db" "$priv/cache/mesa_shader_cache_db"
            ;;
        settings)
            copy_if_present "$snap/config/INRIA" "$priv/config/INRIA"
            ;;
    esac
}

resolve_snapshot() {
    local in="$1" found
    if [ -d "$in" ]; then
        (cd "$in" && pwd)
        return
    fi
    if [ -f "$in" ]; then
        mkdir -p "$OUT/input"
        tar -xzf "$in" -C "$OUT/input" || return 1
        found="$(find "$OUT/input" -maxdepth 3 -type d -name pre-crash-snapshot | head -n 1)"
        if [ -n "$found" ]; then
            printf '%s\n' "$found"
            return
        fi
    fi
    return 1
}

run_replay() {
    local snap round spec cfg priv stores
    local -a extra
    snap="$(resolve_snapshot "$SNAPSHOT_IN")" || { say "cannot find a snapshot in $SNAPSHOT_IN"; return; }
    say "replay: snapshot $snap, $RUNS launches per configuration, interleaved by round"
    local -a configs=()
    for spec in "${MATRIX_CONFIGS[@]}"; do
        case "${spec%%|*}" in
            fresh-xdg-cache) ;;
            *) configs+=("$spec|qt natron driver settings") ;;
        esac
    done
    configs+=("only-qt-cache||qt" "only-natron-cache||natron" "only-driver-cache||driver" "only-settings||settings" "nothing-restored||")
    for round in $(seq 1 "$RUNS"); do
        for spec in "${configs[@]}"; do
            cfg="${spec%%|*}"
            stores="${spec##*|}"
            priv="$OUT/replay-state/$cfg-$round"
            mkdir -p "$priv/cache" "$priv/config" "$priv/cache/nvidia"
            local store
            for store in $stores; do
                restore_store "$snap" "$priv" "$store"
            done
            if [ ! -e "$priv/config/INRIA/Natron.conf" ]; then
                mkdir -p "$priv/config/INRIA"
                printf '[General]\ncheckForUpdates=false\n' > "$priv/config/INRIA/Natron.conf"
            fi
            local middle="${spec#*|}"
            mapfile -t extra < <(config_env "x|${middle%|*}" "")
            run_launch "$cfg" "$round" "$priv/cache" "$priv/config" "$priv/cache/nvidia" \
                "${DIAG_ENV[@]}" XDG_CACHE_HOME="$priv/cache" XDG_CONFIG_HOME="$priv/config" \
                __GL_SHADER_DISK_CACHE_PATH="$priv/cache/nvidia" ${extra[@]+"${extra[@]}"}
            rm -rf "${OUT:?}/replay-state/$cfg-$round"
        done
    done
}

write_host_info
say "output: $OUT"
case "$MODE" in
    capture) run_capture ;;
    matrix) run_matrix ;;
    replay) run_replay ;;
esac
finish
