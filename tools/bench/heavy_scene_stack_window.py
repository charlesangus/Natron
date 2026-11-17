"""Folds the stack samples of a time window into the top stacks of the main thread and the render threads.

  heavy_scene_stack_window.py SAMPLES [--results RESULTS.jsonl --label LABEL --scenario NAME [--action]]
                              [--from EPOCH] [--to EPOCH] [--main-tid TID] [--render-match REGEX]
                              [--top N] [--depth N] [--busy] [--symbols DIR]

SAMPLES is heavy_scene_gpu_host.sh's out/<label>.stacks with SAMPLE=1 (sample_stacks.sh's format:
"=== SAMPLE i epoch-seconds [ms taken]", "TID n:", "#k 0xaddr function - module"). The window is the scenario's
record in RESULTS (its started_at to ended_at; with --action from action_started_at, after the wait for
pending loads), the last such record when there are several; --from and --to override either end.
The main thread is MAIN_TID, else the pid in SAMPLES.pid, else the lowest thread id. Render threads are
the other threads with a frame matching REGEX in any sample (default the scene viewer's render thread,
startRenderThread); when none matches, the threads that run Hydra (HdEngine::Execute, a symbol the USD
libraries export, so it resolves without Natron's symbols), and failing that the busiest other threads.
--symbols DIR names every frame in a module from DIR/<module>.syms, an "nm -n -S -C"
listing of the unstripped file (stage-bundle.sh's NATRON_STAGE_SYMBOLS_DIR writes one per non-PIE
executable). Addresses are taken as absolute, which holds only for non-PIE executables.
Stacks are innermost first, DEPTH frames deep (default 12). --busy leaves out samples whose innermost
frame is a wait. Standard library only, and safe under python3 -I.
"""

import argparse
import bisect
import collections
import json
import os
import re
import sys

WAIT_RE = re.compile(r"futex|pthread_cond_|^(__)?poll$|^__GI___poll|epoll_wait|nanosleep|^(__)?select$|"
                     r"^(__libc_)?read$|sem_wait|__lll_lock_wait|^syscall$|QWaitCondition::wait|"
                     r"QSemaphore::acquire|^ppoll|^__ppoll|^gomp_(team_)?barrier_wait|^gomp_thread_start|"
                     r"^__GI___clock_nanosleep|^g_main_context_iteration|^g_poll")
FRAME_RE = re.compile(r"^#(\d+)\s+(?:0x([0-9a-fA-F]+)\s*)?(.*)$")
HYDRA_RE = r"HdEngine::Execute"


class Symbols:
    """Address to function name for the modules that have a DIR/<module>.syms listing."""

    def __init__(self, directory):
        self.modules = {}
        for entry in sorted(os.listdir(directory)):
            if entry.endswith(".syms"):
                self.modules[entry[:-len(".syms")]] = self._load(os.path.join(directory, entry))

    @staticmethod
    def _load(path):
        starts, ends, names = [], [], []
        with open(path, errors="replace") as f:
            for line in f:
                parts = line.rstrip("\n").split(" ", 3)
                if len(parts) == 4 and len(parts[2]) == 1:
                    address, size, kind, name = int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]
                elif len(parts) >= 3 and len(parts[1]) == 1:
                    address, size, kind, name = int(parts[0], 16), None, parts[1], " ".join(parts[2:])
                else:
                    continue
                if kind not in "tTwWiI":
                    continue
                starts.append(address)
                ends.append(address + size if size else None)
                names.append(name)
        return starts, ends, names

    def name(self, module, address):
        table = self.modules.get(module)
        if not table:
            return None
        starts, ends, names = table
        i = bisect.bisect_right(starts, address) - 1
        if i < 0 or (ends[i] is not None and address >= ends[i]):
            return None
        return names[i]


def frame_name(rest, address=None, symbols=None, outer=False):
    function, module = rest, ""
    cut = rest.rfind(" - ")
    if cut >= 0:
        function, module = rest[:cut], rest[cut + 3:]
    elif rest.startswith("- "):
        function, module = "", rest[2:]
    function = re.sub(r"\s+", " ", function).strip()
    module = os.path.basename(module.strip()) or "?"
    # eu-stack names a frame of a stripped executable after the nearest exported symbol before it, so a
    # module with a symbol listing is resolved from the address whatever eu-stack printed.
    if symbols is not None and address is not None and module in symbols.modules:
        # An outer frame's address is a return address, which can already be past its caller's end.
        found = symbols.name(module, address - 1 if outer else address)
        if found:
            return found
    if not function or function == "??":
        return "??@" + module
    return function


def parse(path, symbols=None):
    """[(epoch, {tid: [innermost frame, ...]})]."""
    samples = []
    threads = None
    stack = None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("=== SAMPLE"):
                parts = line.split()
                threads = {}
                samples.append((float(parts[3]) if len(parts) > 3 else 0.0, threads))
                stack = None
            elif line.startswith("TID ") and threads is not None:
                stack = []
                threads[int(line.split()[1].rstrip(":"))] = stack
            elif stack is not None:
                m = FRAME_RE.match(line.strip())
                if m:
                    address = int(m.group(2), 16) if m.group(2) else None
                    stack.append(frame_name(m.group(3), address, symbols, m.group(1) != "0"))
    return samples


def window_from_results(path, label, scenario, action):
    found = None
    with open(path) as f:
        for line in f:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("label") == label and record.get("scenario") == scenario and record.get("started_at") is not None:
                found = record
    if found is None:
        return None, None
    start = found.get("action_started_at") if action and found.get("action_started_at") is not None else found["started_at"]
    return float(start), float(found.get("ended_at") or 0.0) or None


def short(name, width):
    return name if len(name) <= width else name[:width - 3] + "..."


def waiting(stack):
    return bool(stack) and bool(WAIT_RE.search(stack[0]))


def fold(stacks, depth, top, indent="    "):
    total = len(stacks)
    if not total:
        print(indent + "no samples")
        return
    waits = sum(1 for s in stacks if waiting(s))
    print("%s%d samples, %.0f%% with a wait innermost" % (indent, total, 100.0 * waits / total))
    whole = collections.Counter(" <- ".join(short(fn, 90) for fn in s[:depth]) or "(no frames)" for s in stacks)
    print(indent + "top stacks (innermost first):")
    for key, n in whole.most_common(top):
        print("%s  %5.1f%% %4d  %s" % (indent, 100.0 * n / total, n, key))
    inclusive = collections.Counter()
    for s in stacks:
        inclusive.update(set(s))
    print(indent + "top functions anywhere on the stack:")
    for name, n in inclusive.most_common(top):
        print("%s  %5.1f%% %4d  %s" % (indent, 100.0 * n / total, n, short(name, 140)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("samples")
    parser.add_argument("--results")
    parser.add_argument("--label")
    parser.add_argument("--scenario")
    parser.add_argument("--action", action="store_true")
    parser.add_argument("--from", dest="start", type=float)
    parser.add_argument("--to", dest="end", type=float)
    parser.add_argument("--main-tid", type=int)
    parser.add_argument("--render-match", default="startRenderThread")
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--depth", type=int, default=12)
    parser.add_argument("--busy", action="store_true")
    parser.add_argument("--symbols")
    args = parser.parse_args()

    start, end = None, None
    if args.results:
        if not (args.label and args.scenario):
            parser.error("--results needs --label and --scenario")
        start, end = window_from_results(args.results, args.label, args.scenario, args.action)
        if start is None:
            print("no %s record of %s with started_at in %s" % (args.scenario, args.label, args.results), file=sys.stderr)
            return 1
    if args.start is not None:
        start = args.start
    if args.end is not None:
        end = args.end

    symbols = Symbols(args.symbols) if args.symbols else None
    if symbols is not None:
        print("symbols for %s from %s" % (", ".join(sorted(symbols.modules)) or "no module", args.symbols))
    samples = parse(args.samples, symbols)
    if not samples:
        print("no samples in %s" % args.samples, file=sys.stderr)
        return 1
    chosen = [(t, threads) for t, threads in samples if (start is None or t >= start) and (end is None or t <= end)]
    print("samples %s: %d of %d in the window %s to %s (%s s)" % (
        args.samples, len(chosen), len(samples), "%.3f" % start if start is not None else "start",
        "%.3f" % end if end is not None else "end", "%.1f" % (end - start) if start is not None and end is not None else "?"))
    if len(chosen) > 1:
        gaps = [b[0] - a[0] for a, b in zip(chosen, chosen[1:])]
        print("sample period: median %.3f s, longest %.3f s" % (sorted(gaps)[len(gaps) // 2], max(gaps)))

    main_tid = args.main_tid
    if main_tid is None and os.path.isfile(args.samples + ".pid"):
        with open(args.samples + ".pid") as f:
            text = f.read().strip()
        main_tid = int(text) if text.isdigit() else None
    if main_tid is None:
        main_tid = min(tid for _, threads in samples for tid in threads)

    def keep(stack):
        return not (args.busy and waiting(stack))

    def matching(pattern):
        found = re.compile(pattern)
        return sorted(set(tid for _, threads in samples for tid, stack in threads.items()
                          if tid != main_tid and any(found.search(fn) for fn in stack)))

    render_match = args.render_match
    render_tids = matching(render_match)
    if not render_tids and render_match != HYDRA_RE:
        print("\nno thread has a frame matching %r (stripped symbols, or another name); trying %r" % (render_match, HYDRA_RE))
        render_match = HYDRA_RE
        render_tids = matching(render_match)

    print("\nmain thread %d:" % main_tid)
    fold([threads[main_tid] for _, threads in chosen if main_tid in threads and keep(threads[main_tid])], args.depth, args.top)

    if render_tids:
        print("\nrender threads %s (a frame matches %r):" % (", ".join(map(str, render_tids)), render_match))
        for tid in render_tids:
            print("  thread %d:" % tid)
            fold([threads[tid] for _, threads in chosen if tid in threads and keep(threads[tid])], args.depth, args.top)
    else:
        print("\nno thread has a frame matching %r; the busiest other threads:" % render_match)
        busy = collections.Counter(tid for _, threads in chosen for tid, stack in threads.items()
                                   if tid != main_tid and stack and not waiting(stack))
        for tid, _ in busy.most_common(3):
            print("  thread %d:" % tid)
            fold([threads[tid] for _, threads in chosen if tid in threads and keep(threads[tid])], args.depth, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
