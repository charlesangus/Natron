#!/usr/bin/env bash
# gdb fallback for sample_stacks.sh where eu-stack is not installed. Same arguments, same output
# format (=== SAMPLE / TID n: / #N 0xADDR function - module) so analyze_stacks.py reads it unchanged:
#   tools/bench/sample_stacks_gdb.sh PID COUNT INTERVAL OUT
# Needs ptrace rights on PID: same user, and kernel.yama.ptrace_scope 0, or 1 with PID allowing the
# tracer through prctl(PR_SET_PTRACER), else root. Exits 3 with gdb's reason and the remedy on stderr
# (also in OUT.err) when the first sample gets no stacks; later failures go to OUT.err only.
set -u
pid=$1
count=$2
interval=$3
out=$4
: > "$out"
: > "$out.err"
# shellcheck source=ptrace_remedy.sh
. "$(dirname "${BASH_SOURCE[0]}")/ptrace_remedy.sh"
if ! command -v gdb > /dev/null 2>&1; then
    echo "sample_stacks_gdb.sh: gdb is not installed (sudo apt install gdb, or elfutils for eu-stack)" | tee -a "$out.err" >&2
    exit 2
fi
if ! kill -0 "$pid" 2> /dev/null; then
    echo "sample_stacks_gdb.sh: no process $pid to sample (or no permission to signal it)" | tee -a "$out.err" >&2
    exit 2
fi

convert() {
    python3 -I -c '
import re, sys
frame = re.compile(r"^#(\d+)\s+(?:(0x[0-9a-f]+)\s+in\s+)?(.*)$")
thread = re.compile(r"^Thread \d+ .*\(LWP (\d+)\)")
for line in sys.stdin:
    line = line.rstrip("\n")
    m = thread.match(line)
    if m:
        print("TID %s:" % m.group(1))
        continue
    m = frame.match(line)
    if not m:
        continue
    num, addr, rest = m.groups()
    mod = ""
    f = re.search(r" from (\S+)$", rest)
    if f:
        mod, rest = f.group(1), rest[:f.start()]
    else:
        a = re.search(r" at \S+$", rest)
        if a:
            rest = rest[:a.start()]
    if rest.endswith(")"):
        depth = 0
        for i in range(len(rest) - 1, -1, -1):
            depth += (rest[i] == ")") - (rest[i] == "(")
            if depth == 0:
                rest = rest[:i].rstrip()
                break
    print("#%s  %s %s%s" % (num, addr or "0x0", rest or "??", " - " + mod if mod else ""))
'
}

sample_err=$(mktemp)
trap 'rm -f "$sample_err"' EXIT
for ((i = 0; i < count; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    echo "=== SAMPLE $i $(date +%s.%N)" >> "$out"
    before=$(grep -c '^TID ' "$out")
    gdb -p "$pid" -batch -ex "set pagination off" -ex "set print frame-arguments none" \
        -ex "thread apply all bt 80" 2> "$sample_err" | convert >> "$out"
    if [ "$(grep -c '^TID ' "$out")" = "$before" ] && kill -0 "$pid" 2> /dev/null; then
        {
            echo "sample $i of pid $pid got no stacks; gdb said:"
            grep -iv '^warning: \|^$' "$sample_err" | head -n 5
            ptrace_remedy
        } >> "$out.err"
        if [ "$i" = 0 ]; then
            cat "$out.err" >&2
            exit 3
        fi
    fi
    sleep "$interval"
done
