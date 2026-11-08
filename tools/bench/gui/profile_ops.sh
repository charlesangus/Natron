#!/usr/bin/env bash
# Profiles single GUI-bench ops with wall-clock stack samples.
#
#   tools/bench/gui/profile_ops.sh [options]
#
# For each op, runs `run.sh --xvfb` (natron-dev container, local build/release, not an AppImage) on
# one topology at one size, one rep, that op only, and while the op runs samples every Natron thread
# with tools/bench/sample_stacks.sh (docker exec -u root --privileged), then summarises the samples
# with analyze_stacks.py. Output goes to build/bench/profile-<UTC>/ (or --out):
#
#   <op>/                      run.sh result dir (jsonl with step_wall/step_cpu/loop_pass, logs)
#   <op>.stacks                raw samples            <op>.top.txt   analyze_stacks.py --top 25
#   <op>.status                ok | timeout | error ... and what was sampled
#   summary.txt                the one-line-per-op summary that is also printed at the end
#
# Options:
#   --ops OP[,OP...]     default: pan,hover,zoom_step,drag_move_1,select_all,knob_change_fanout,undo,redo
#   --topology T         default comp          --size N     default 3000        --reps N   default 1
#   --interval S         seconds between samples (default 0.25)
#   --lead S             seconds to wait after the graph is generated before sampling, to skip the
#                        save, panel close and fit that precede the op (default 6)
#   --op-cap S           kill the run if the op has not finished S seconds after the graph was
#                        generated, and record a timeout (default 600)
#   --load-cap S         cap on the whole run, startup and graph generation included (default 3600)
#   --out DIR            default build/bench/profile-<UTC>
#   --no-forced-repaint  passed through to run.sh
#   --dry-run            print what would run and exit
#
# Host-display mode (for a machine with a real display and GPU, no container):
#   --host --appimage PATH   runs each op via `run.sh --host --appimage PATH` (real $DISPLAY, throwaway
#                            HOME), finds the Natron pid on the host and samples with eu-stack when it is
#                            installed, else with gdb (sample_stacks_gdb.sh, same output format). Needs
#                            ptrace rights on the Natron process (same user is enough unless
#                            kernel.yama.ptrace_scope is 2 or more). Default is the container mode above.
#
# Environment: NATRON_DEV_CONTAINER (default natron-dev), PROFILE_RUN_SH (default run.sh beside this
# file), PROFILE_POLL_S (poll period, default 1). Must run from the host, not inside the container.
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../../.." && pwd)
host_mode=0
appimage=""
sampler="$repo/tools/bench/sample_stacks.sh"
container=${NATRON_DEV_CONTAINER:-natron-dev}
run_sh=${PROFILE_RUN_SH:-$here/run.sh}
poll=${PROFILE_POLL_S:-1}

# Runs a command in the container as root with ptrace rights. PROFILE_EXEC overrides the prefix
# (empty string = run locally), which is how the PID logic is tested without Docker.
cexec() {
    if [ "$host_mode" = 1 ]; then
        "$@"
    elif [ "${PROFILE_EXEC+set}" = set ]; then
        # shellcheck disable=SC2086
        $PROFILE_EXEC "$@"
    else
        docker exec -u root --privileged -w "$repo" "$container" "$@"
    fi
}

usage() {
    sed -n '2,38p' "${BASH_SOURCE[0]}" >&2
    exit 2
}

# find_pid DIR: the PID of the Natron process whose NATRON_GUI_TEST_OUT lies under DIR. Matching on
# the environment keeps another Natron (a stale run, a second lane) from being sampled by mistake.
find_pid() {
    local dir=$1 p
    for p in $(cexec pgrep -x "${PROFILE_PROC_NAME:-Natron}" 2> /dev/null); do
        if cexec cat "/proc/$p/environ" 2> /dev/null | tr '\0' '\n' | grep -q "^NATRON_GUI_TEST_OUT=$dir/"; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

# first_file DIR NAME: the first file called NAME under DIR (the run's per-topology dir).
first_file() {
    find "$1" -name "$2" -type f 2> /dev/null | head -n 1
}

# summarize OP OPDIR STATUS TOPFILE: one line, the op's per-event medians and its top 3 self frames.
summarize() {
    python3 -I - "$@" << 'PY'
import glob, json, os, re, sys
op, opdir, status, top = sys.argv[1:5]
rec = None
for path in glob.glob(os.path.join(opdir, "*.jsonl")):
    for line in open(path, errors="replace"):
        try:
            r = json.loads(line)
        except ValueError:
            continue
        if r.get("type") == "op" and r.get("op") == op:
            rec = r
def f(key):
    v = rec.get(key) if rec else None
    return "-" if v is None else "%.2f" % v
frames = []
if os.path.exists(top):
    on = False
    for line in open(top, errors="replace"):
        if line.startswith("busy: self"):
            on = True
            continue
        if on:
            m = re.match(r"\s+([\d.]+%)\s+(.*)", line)
            if m:
                frames.append("%s %s" % (m.group(1), re.sub(r"\s+\[.*\]$", "", m.group(2)).strip()))
            if len(frames) == 3 or not line.strip():
                break
ev = rec.get("events_per_rep") if rec else None
shown = status
if rec and status == "ok":
    shown = rec.get("status", "ok")
print("%-20s %-8s events=%s wall=%sms cpu=%sms loop=%sms | %s" % (
    op, shown, ev if ev is not None else "-",
    f("step_wall_median_ms"), f("step_cpu_median_ms"), f("loop_pass_median_ms"),
    "; ".join(frames) if frames else "no frames"))
PY
}

profile_op() {
    local op=$1 opdir="$out/$1" stacks="$out/$1.stacks" status=ok note="" start_log rc
    local result donefile pid="" sampler_pid="" sampler_started=0 graph_seen="" res_file now
    local start run_deadline op_deadline="" count lead_until="" sampled_for=""
    mkdir -p "$opdir"
    start_log="$opdir/start.log"
    echo "== $op: starting"
    local extra=()
    [ "$forced_repaint" = 0 ] && extra+=(--no-forced-repaint)
    local mode_args=(--xvfb) run_pid="" w
    [ "$host_mode" = 1 ] && mode_args=(--host --appimage "$appimage")
    if [ "$host_mode" = 1 ]; then
        # --host runs in the foreground: start it in the background and wait for its result line.
        GUI_BENCH_TIMEOUT=$load_cap setsid "$run_sh" "${mode_args[@]}" --topologies "$topology" --sizes "$size" \
            --reps "$reps" --ops "$op" --out "$opdir" "${extra[@]}" > "$start_log" 2>&1 < /dev/null &
        run_pid=$!
        rc=0
        for ((w = 0; w < 300; w++)); do
            grep -q '^result ' "$start_log" 2> /dev/null && break
            kill -0 "$run_pid" 2> /dev/null || { wait "$run_pid"; rc=$?; [ "$rc" -eq 0 ] && rc=1; break; }
            sleep 1
        done
    else
        GUI_BENCH_TIMEOUT=$load_cap "$run_sh" "${mode_args[@]}" --topologies "$topology" --sizes "$size" --reps "$reps" \
            --ops "$op" --out "$opdir" "${extra[@]}" > "$start_log" 2>&1
        rc=$?
    fi
    result=$(sed -n 's/^result \([^ ]*\.jsonl\).*/\1/p' "$start_log" | head -n 1)
    if [ "$rc" -ne 0 ] || [ -z "$result" ]; then
        status="error: run.sh did not start (exit $rc, see $start_log)"
        echo "$status" > "$out/$op.status"
        summarize "$op" "$opdir" "$status" /nonexistent | tee -a "$out/summary.txt"
        return
    fi
    donefile="$result.done"
    start=$(date +%s)
    run_deadline=$((start + load_cap + 90))
    count=$(awk -v c="$max_sample_s" -v i="$interval" 'BEGIN { printf "%d", c / i + 1 }')
    while [ ! -f "$donefile" ]; do
        now=$(date +%s)
        res_file=$(first_file "$opdir" results.txt)
        if [ -z "$graph_seen" ] && [ -n "$res_file" ] && grep -q '^GRAPH ' "$res_file" 2> /dev/null; then
            graph_seen=$now
            op_deadline=$((now + op_cap))
            lead_until=$((now + lead))
            echo "   graph loaded after $((now - start)) s"
        fi
        if [ "$sampler_started" = 0 ] && [ -n "$graph_seen" ] && [ "$now" -ge "$lead_until" ]; then
            pid=$(find_pid "$opdir")
            if [ -n "$pid" ]; then
                sampler_started=$now
                cexec timeout -s TERM "$max_sample_s" "$sampler" "$pid" "$count" "$interval" "$stacks" \
                    > /dev/null 2>&1 &
                sampler_pid=$!
                echo "   sampling pid $pid every ${interval}s"
            elif [ $((now - lead_until)) -gt 60 ]; then
                note="no Natron pid found under $opdir"
                sampler_started=-1
            fi
        fi
        if [ -n "$sampler_pid" ] && [ -n "$res_file" ] && grep -q '^RESULT ' "$res_file" 2> /dev/null; then
            sampled_for=$((now - sampler_started))
            cexec pkill -f "[s]ample_stacks(_gdb)?\.sh $pid " > /dev/null 2>&1
            wait "$sampler_pid" 2> /dev/null
            sampler_pid=""
        fi
        if { [ -n "$op_deadline" ] && [ "$now" -gt "$op_deadline" ]; } || [ "$now" -gt "$run_deadline" ]; then
            status="timeout"
            note="killed after $((now - start)) s (op cap ${op_cap}s from graph load, run cap ${load_cap}s)"
            echo "   $note"
            [ -z "$pid" ] && pid=$(find_pid "$opdir")
            if [ -n "$pid" ]; then
                cexec kill -TERM "$pid" > /dev/null 2>&1
                sleep 5
                cexec kill -KILL "$pid" > /dev/null 2>&1
            fi
            break
        fi
        sleep "$poll"
    done
    if [ -n "$sampler_pid" ]; then
        sampled_for=$(($(date +%s) - sampler_started))
        cexec pkill -f "[s]ample_stacks(_gdb)?\.sh $pid " > /dev/null 2>&1
        wait "$sampler_pid" 2> /dev/null
    fi
    # After a kill, give run.sh a moment to write .done; otherwise carry on.
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -f "$donefile" ] && break
        sleep 1
    done
    [ -n "$run_pid" ] && wait "$run_pid" 2> /dev/null
    if [ "$status" = ok ] && [ -f "$donefile" ]; then
        rc=$(cat "$donefile")
        case $rc in
            0) ;;
            124) status="timeout"; note="the run cap (${load_cap}s) ended the run" ;;
            3) status="timeout"; note="a single event never returned (exit 3)" ;;
            *) status="error: exit $rc" ;;
        esac
    fi
    if [ -s "$stacks" ]; then
        python3 -I "$here/../analyze_stacks.py" "$stacks" --top 25 > "$out/$op.top.txt" 2>&1 || note="$note; analyze_stacks.py failed"
    else
        rm -f "$out/$op.top.txt"
        note="$note; no stack samples"
    fi
    {
        echo "$status"
        echo "sampled_s=${sampled_for:-?} samples=$(grep -c '^=== SAMPLE' "$stacks" 2> /dev/null || echo 0) ${note}"
    } > "$out/$op.status"
    summarize "$op" "$opdir" "$status" "$out/$op.top.txt" | tee -a "$out/summary.txt"
}

main() {
    ops=pan,hover,zoom_step,drag_move_1,select_all,knob_change_fanout,undo,redo
    topology=comp size=3000 reps=1 interval=0.25 lead=6 op_cap=600 load_cap=3600
    out="" forced_repaint=1
    local dry=0 v list op
    while [ $# -gt 0 ]; do
        case $1 in
            --ops) ops=${2:?--ops needs a list}; shift ;;
            --topology) topology=${2:?--topology needs a name}; shift ;;
            --size) size=${2:?--size needs a count}; shift ;;
            --reps) reps=${2:?--reps needs a count}; shift ;;
            --interval) interval=${2:?--interval needs seconds}; shift ;;
            --lead) lead=${2:?--lead needs seconds}; shift ;;
            --op-cap) op_cap=${2:?--op-cap needs seconds}; shift ;;
            --load-cap) load_cap=${2:?--load-cap needs seconds}; shift ;;
            --out) out=${2:?--out needs a directory}; shift ;;
            --host) host_mode=1 ;;
            --appimage) appimage=${2:?--appimage needs a path}; shift ;;
            --no-forced-repaint) forced_repaint=0 ;;
            --dry-run) dry=1 ;;
            -h|--help) usage ;;
            *) echo "unknown argument: $1" >&2; usage ;;
        esac
        shift
    done
    for v in size reps op_cap load_cap lead; do
        case ${!v} in ''|*[!0-9]*) echo "--${v//_/-} needs a whole number" >&2; exit 2 ;; esac
    done
    case $interval in ''|*[!0-9.]*) echo "--interval needs a number" >&2; exit 2 ;; esac
    [ "$op_cap" -gt 0 ] || { echo "--op-cap must be positive" >&2; exit 2; }
    max_sample_s=$op_cap
    if [ "$host_mode" = 1 ]; then
        [ -n "$appimage" ] || { echo "--host needs --appimage" >&2; exit 2; }
        case $appimage in /*) ;; *) appimage=$(pwd)/$appimage ;; esac
        if command -v eu-stack > /dev/null 2>&1; then
            sampler="$repo/tools/bench/sample_stacks.sh"
        elif command -v gdb > /dev/null 2>&1; then
            sampler="$repo/tools/bench/sample_stacks_gdb.sh"
        else
            echo "--host needs eu-stack (elfutils) or gdb to sample stacks" >&2
            [ "$dry" = 1 ] || exit 2
        fi
    fi
    [ -n "$out" ] || out=build/bench/profile-$(date -u +%Y%m%dT%H%M%SZ)
    case $out in /*) ;; *) out=$repo/$out ;; esac
    IFS=, read -r -a list <<< "$ops"
    [ "${#list[@]}" -gt 0 ] || usage
    if [ "$dry" = 1 ]; then
        echo "out $out"
        for op in "${list[@]}"; do
            echo "run.sh $([ "$host_mode" = 1 ] && echo "--host --appimage $appimage" || echo --xvfb) --topologies $topology --sizes $size --reps $reps --ops $op --out $out/$op" \
                "$([ "$forced_repaint" = 0 ] && echo --no-forced-repaint)" \
                "| sample every ${interval}s from ${lead}s after graph load, op cap ${op_cap}s, run cap ${load_cap}s"
        done
        exit 0
    fi
    if [ "$host_mode" = 1 ]; then
        [ -f "$appimage" ] || { echo "no AppImage at $appimage" >&2; exit 2; }
        echo "sampler $sampler"
    elif [ ! -x "$repo/build/release/App/Natron" ]; then
        echo "no release build at $repo/build/release/App/Natron" >&2
        exit 2
    fi
    if [ "$host_mode" != 1 ] && [ "${PROFILE_EXEC+set}" != set ] && ! docker ps --format '{{.Names}}' 2> /dev/null | grep -qx "$container"; then
        echo "container $container is not running; start it with tools/ci/local/devshell.sh true" >&2
        exit 2
    fi
    mkdir -p "$out"
    : > "$out/summary.txt"
    echo "output $out"
    for op in "${list[@]}"; do
        profile_op "$op"
    done
    echo
    echo "summary (per-event medians; top 3 self frames of the busy thread-samples), also in $out/summary.txt:"
    cat "$out/summary.txt"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
