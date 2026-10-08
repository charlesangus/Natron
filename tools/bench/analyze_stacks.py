#!/usr/bin/env python3
"""Summarise eu-stack samples written by sample_stacks.sh.

Every thread in every sample is classified as:
  idle     blocked, with no render frames on its stack (an idle pool thread, the main loop)
  blocked  blocked in a wait, but inside a render (waiting on other threads, a lock, a future)
  busy     running; split into plugin actions, Natron engine code, allocation and so on

Usage: analyze_stacks.py SAMPLES [--top N]
"""
import collections
import re
import sys

FRAME = re.compile(r"^#(\d+)\s+0x[0-9a-f]+\s+(.*?)(?:\s+-\s+(\S+))?$")
# libgomp workers parked between parallel regions spin or futex-wait in the barrier; they are not working.
BLOCKING = ("futex", "__GI___futex", "pthread_cond_", "__pthread_cond", "poll", "epoll_wait",
            "nanosleep", "clock_nanosleep", "select", "__select", "read", "__libc_read",
            "sem_wait", "__new_sem_wait", "do_futex_wait", "__lll_lock_wait", "syscall",
            "QWaitCondition::wait", "QSemaphore::acquire", "gomp_barrier_wait_end",
            "gomp_team_barrier_wait_end", "gomp_thread_start")
BLOCKING_RE = re.compile(r"futex|pthread_cond_|^(__)?poll$|^__GI___poll|epoll_wait|nanosleep|^(__)?select$|"
                         r"^(__libc_)?read$|sem_wait|__lll_lock_wait|^syscall$|QWaitCondition::wait|"
                         r"QSemaphore::acquire|^ppoll|^__ppoll|^gomp_(team_)?barrier_wait|^gomp_thread_start")
RENDER_MARKERS = ("renderRoI", "renderViewer", "tiledRenderingFunctor", "renderHandler",
                  "RenderThreadTask", "DefaultScheduler::processFrame", "renderFrame",
                  "treeRecurseFunctor", "computeRequestPass", "getImage")
OFX_ACTIONS = (("render", "OfxEffectInstance::render"),
               ("rod", "OfxEffectInstance::getRegionOfDefinition"),
               ("roi", "OfxEffectInstance::getRegionsOfInterest"),
               ("identity", "OfxEffectInstance::isIdentity"),
               ("framesNeeded", "OfxEffectInstance::getFramesNeeded"),
               ("components", "OfxEffectInstance::getComponentsNeededAndProduced"),
               ("metadata", "OfxEffectInstance::getPreferredMetadata"),
               ("beginEnd", "beginSequenceRender"),
               ("beginEnd", "endSequenceRender"))
ENGINE_PHASES = (("request pass", "computeRequestPass"),
                 ("render-args setup", "ParallelRenderArgsSetter"),
                 ("cache lookup", "getImageFromCacheAndConvertIfNeeded"),
                 ("cache insert/alloc", "Cache<"),
                 ("image alloc/fill", "Image::"),
                 ("hash", "Hash"),
                 ("metadata", "Metadata"),
                 ("identity", "isIdentity_public"),
                 ("rod", "getRegionOfDefinition_public"),
                 ("components", "getComponentsNeeded"),
                 ("writer", "WriteNode"),
                 ("python", "Py"))


def short(fn):
    fn = re.sub(r"\(.*", "", fn)
    return fn[:110]


def parse(path):
    samples = []
    cur = None
    thread = None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("=== SAMPLE"):
                cur = []
                samples.append(cur)
                thread = None
            elif line.startswith("TID "):
                thread = []
                if cur is not None:
                    cur.append(thread)
            else:
                m = FRAME.match(line.strip())
                if m and thread is not None:
                    thread.append((m.group(2) or "??", m.group(3) or ""))
    return samples


def classify(stack):
    names = [fn for fn, _ in stack]
    in_render = any(any(k in fn for k in RENDER_MARKERS) for fn in names)
    top = names[0] if names else "??"
    if not names:
        return "unknown", None
    blocked = BLOCKING_RE.search(top) is not None
    if blocked:
        if not in_render:
            return "idle", None
        # The first engine frame below the wait says what it is waiting for.
        for fn in names[1:]:
            if "Natron" in fn or "EffectInstance" in fn or "Cache" in fn or "Image" in fn:
                return "blocked", short(fn)
        return "blocked", short(names[min(3, len(names) - 1)])
    for fn, mod in stack:
        if mod.endswith(".ofx"):
            for label, marker in OFX_ACTIONS:
                if any(marker in n for n in names):
                    return "busy", "plugin:" + label
            return "busy", "plugin:other"
    if top.startswith(("malloc", "free", "_int_", "memset", "__memset", "memcpy", "__memmove",
                       "__memcpy", "operator new", "operator delete", "cfree")):
        return "busy", "libc alloc/copy"
    for label, marker in ENGINE_PHASES:
        if any(marker in n for n in names[:12]):
            return "busy", "engine:" + label
    return "busy", "engine:other"


def main():
    path = sys.argv[1]
    top_n = int(sys.argv[sys.argv.index("--top") + 1]) if "--top" in sys.argv else 25
    samples = parse(path)
    state = collections.Counter()
    detail = collections.Counter()
    blocked_on = collections.Counter()
    inclusive = collections.Counter()
    self_fn = collections.Counter()
    busy_per_sample = []
    for sample in samples:
        busy = 0
        for stack in sample:
            kind, what = classify(stack)
            state[kind] += 1
            if kind == "busy":
                busy += 1
                detail[what] += 1
                self_fn[short(stack[0][0]) + "  [" + stack[0][1].split("/")[-1] + "]"] += 1
                for fn in set(short(fn) for fn, _ in stack):
                    inclusive[fn] += 1
            elif kind == "blocked":
                blocked_on[what] += 1
        busy_per_sample.append(busy)
    n = len(samples)
    print("samples: %d, threads/sample: %.1f" % (n, sum(len(s) for s in samples) / max(n, 1)))
    print("mean busy threads per sample: %.2f" % (sum(busy_per_sample) / max(n, 1)))
    total = sum(state.values())
    for k, v in state.most_common():
        print("  %-8s %6d  %5.1f%%" % (k, v, 100.0 * v / total))
    busy_total = state["busy"] or 1
    print("\nbusy thread-samples by activity:")
    for k, v in detail.most_common():
        print("  %-34s %6d  %5.1f%%" % (k, v, 100.0 * v / busy_total))
    print("\nblocked-inside-render, innermost engine frame:")
    for k, v in blocked_on.most_common(12):
        print("  %6d  %s" % (v, k))
    print("\nbusy: self (top frame):")
    for k, v in self_fn.most_common(top_n):
        print("  %5.1f%%  %s" % (100.0 * v / busy_total, k))
    print("\nbusy: inclusive:")
    for k, v in inclusive.most_common(top_n * 2):
        print("  %5.1f%%  %s" % (100.0 * v / busy_total, k))


main()
