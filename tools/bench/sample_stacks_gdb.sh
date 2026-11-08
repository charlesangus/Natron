#!/usr/bin/env bash
# gdb fallback for sample_stacks.sh where eu-stack is not installed. Same arguments, same output
# format (=== SAMPLE / TID n: / #N 0xADDR function - module) so analyze_stacks.py reads it unchanged:
#   tools/bench/sample_stacks_gdb.sh PID COUNT INTERVAL OUT
# Needs ptrace rights on PID (same user, kernel.yama.ptrace_scope 0 or 1 for a child, else root).
set -u
pid=$1
count=$2
interval=$3
out=$4
: > "$out"

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

for ((i = 0; i < count; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    echo "=== SAMPLE $i $(date +%s.%N)" >> "$out"
    gdb -p "$pid" -batch -ex "set pagination off" -ex "set print frame-arguments none" \
        -ex "thread apply all bt 80" 2>/dev/null | convert >> "$out"
    sleep "$interval"
done
