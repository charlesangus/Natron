#!/usr/bin/env python3
"""Syntax-check C/C++ translation units inside the dev container.

usage: syntax-check.py [--root <checkout>] [--base <ref>] [--compdb <path>] [<file> ...]

Each TU is compiled with -fsyntax-only using its command from the main
checkout's build/fast compile DB, with source paths rewritten to --root. Files
are relative to --root. With no files, the TUs changed between --base and the
working tree are checked. A header is checked through the .cpp beside it.

Exit status is non-zero when any TU is FAIL or NO-COMMAND.
"""
import argparse
import filecmp
import json
import os
import re
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
CONTAINER = os.environ.get("NATRON_DEV_CONTAINER") or "natron-dev"
IN_CONTAINER = os.environ.get("NATRON_IN_CONTAINER") == "1"
WORKERS = 3
SOURCE_EXTS = (".cpp", ".cc", ".cxx", ".c")
HEADER_EXTS = (".h", ".hpp", ".hh")
GENERATED_INCLUDE = re.compile(r"^(GpuKernel\w*\.\w+|\w+_spirv\.h)$")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
MOC_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"(moc_[^"]*\.cpp|[^"]*\.moc)"', re.M)

# Mirrors the `fast` preset of build.sh, with tests on so the Tests/Gpu
# kernel headers get rules too.
SYNTAX_CMAKE_ARGS = [
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_FLAGS_RELEASE=-O1 -DNDEBUG",
    "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG",
    "-DCMAKE_PREFIX_PATH=/usr/local",
    "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
    "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DNATRON_BUILD_TESTS=ON",
]
GPU_TRIGGER_PATHS = ["Engine/Gpu/Kernels", "Engine/Gpu/CMakeLists.txt"]


class UsageError(Exception):
    pass


def die(msg, code=2):
    print(f"syntax-check.py: {msg}", file=sys.stderr)
    sys.exit(code)


def git(root, *args, check=True):
    p = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    if check and p.returncode != 0:
        die(f"git {' '.join(args)} failed in {root}: {p.stderr.strip()}")
    return p


def toplevel(path):
    p = git(path, "rev-parse", "--show-toplevel", check=False)
    return p.stdout.strip() if p.returncode == 0 else None


def main_checkout():
    p = git(SCRIPT_DIR, "rev-parse", "--path-format=absolute", "--git-common-dir")
    return os.path.dirname(p.stdout.strip())


def container_exec(argv, cwd):
    """Runs argv in the dev container; paths are identical on both sides of the bind mount."""
    if IN_CONTAINER:
        return subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    return subprocess.run(["docker", "exec", "-w", cwd, CONTAINER, *argv],
                          capture_output=True, text=True)


def container_shell(cmd, cwd):
    return container_exec(["bash", "-lc", cmd], cwd)


def ninja_running():
    p = container_exec(["pgrep", "-x", "ninja"], "/")
    return p.returncode == 0


# --- compile DB --------------------------------------------------------------

class CompDb:
    def __init__(self, path, root):
        self.path = path
        self.root = root
        self.build_dir = os.path.dirname(os.path.abspath(path))
        with open(path) as f:
            self.entries = json.load(f)
        self.src_root = self._source_root()
        self.by_file = {os.path.normpath(e["file"]): e for e in self.entries}

    def _source_root(self):
        cache = os.path.join(self.build_dir, "CMakeCache.txt")
        try:
            with open(cache) as f:
                for line in f:
                    if line.startswith("CMAKE_HOME_DIRECTORY:"):
                        return os.path.normpath(line.split("=", 1)[1].strip())
        except OSError:
            pass
        return os.path.normpath(os.path.join(self.build_dir, "..", ".."))

    def mtime(self):
        return os.path.getmtime(self.path)

    def src(self, rel):
        return os.path.join(self.src_root, rel)

    def lookup(self, rel):
        """Returns (entry, None) or (None, searched-dir) when no sibling can lend flags."""
        src = self.src(rel)
        e = self.by_file.get(src)
        if e is not None:
            return e, None
        parts = rel.split("/")
        cmake_dir = nearest_cmake_dir(self.root, rel)
        # Flags never cross a top-level directory, and each libs/<name> is its
        # own project, so the search stops there.
        stop = 2 if parts[0] == "libs" and len(parts) > 2 else 1
        build_prefix = self.build_dir + os.sep
        searched = None
        for depth in range(len(parts) - 1, stop - 1, -1):
            d = os.path.join(self.src_root, *parts[:depth]) + os.sep
            searched = "/".join(parts[:depth]) + "/"
            # Flags only carry over within one CMake directory: a nested
            # CMakeLists.txt defines its own targets and include dirs.
            cands = [x for x in self.entries
                     if x["file"].startswith(d) and not x["file"].startswith(build_prefix)
                     and x["file"].endswith(SOURCE_EXTS)
                     and nearest_cmake_dir(self.src_root, os.path.relpath(x["file"], self.src_root))
                     == cmake_dir]
            if not cands:
                continue
            counts = {}
            for x in cands:
                counts[target_of(x)] = counts.get(target_of(x), 0) + 1
            best = max(counts, key=counts.get)
            sib = next(x for x in cands if target_of(x) == best)
            borrowed = dict(sib)
            borrowed["file"] = src
            if "command" in borrowed:
                borrowed["command"] = borrowed["command"].replace(sib["file"], src)
            else:
                borrowed["arguments"] = [src if a == sib["file"] else a for a in borrowed["arguments"]]
            return borrowed, None
        return None, searched or "./"

    def stale_cmake(self, rel):
        """The checkout's CMakeLists.txt governing rel when this DB's tree
        lacks it or has a different one, so the DB may lack rel's target."""
        if os.path.normpath(self.src(rel)) in self.by_file:
            return None
        d = nearest_cmake_dir(self.root, rel)
        mine = os.path.join(self.root, d, "CMakeLists.txt")
        theirs = os.path.join(self.src_root, d, "CMakeLists.txt")
        if d == nearest_cmake_dir(self.src_root, rel) and os.path.isfile(theirs) and \
                filecmp.cmp(mine, theirs, shallow=False):
            return None
        return mine

    def argv_for(self, entry):
        args = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
        if self.root != self.src_root:
            keep = os.path.join(self.src_root, "build") + os.sep
            args = [rewrite_root(a, self.src_root + os.sep, self.root + os.sep, keep) for a in args]
        out, skip = [], False
        for a in args:
            if skip:
                skip = False
            elif a == "-o":
                skip = True
            elif a != "-c":
                out.append(a)
        return out + ["-fsyntax-only"]


def nearest_cmake_dir(base, rel):
    """Directory (relative to base) of the closest CMakeLists.txt at or above rel."""
    d = os.path.dirname(rel)
    while d and not os.path.isfile(os.path.join(base, d, "CMakeLists.txt")):
        d = os.path.dirname(d)
    return d


def rewrite_root(arg, old, new, keep):
    marker = "\0KEEP\0"
    return arg.replace(keep, marker).replace(old, new).replace(marker, keep)


def target_of(entry):
    m = re.search(r"CMakeFiles/([^/]+)\.dir/", entry.get("output", "") or entry.get("command", ""))
    return m.group(1) if m else ""


def include_dirs(argv):
    dirs, it = [], iter(argv)
    for a in it:
        for flag in ("-I", "-isystem", "-iquote"):
            if a == flag:
                dirs.append(next(it, ""))
            elif a.startswith(flag) and len(a) > len(flag):
                dirs.append(a[len(flag):])
    return dirs


# --- TU selection -------------------------------------------------------------

def changed_files(root, base):
    files = git(root, "diff", "--name-only", base, "HEAD").stdout.split()
    files += git(root, "diff", "--name-only", "HEAD").stdout.split()
    files += git(root, "ls-files", "--others", "--exclude-standard").stdout.split()
    return files


def select_tus(root, files):
    """Returns ([tu, ...], [(rel, status, detail), ...]) for header skips."""
    tus, skipped = [], []
    for f in dict.fromkeys(files):
        if not os.path.isfile(os.path.join(root, f)):
            continue
        if f.endswith(SOURCE_EXTS):
            tus.append(f)
        elif f.endswith(HEADER_EXTS):
            stem = os.path.splitext(f)[0]
            owner = next((stem + ext for ext in SOURCE_EXTS
                          if os.path.isfile(os.path.join(root, stem + ext))), None)
            if owner:
                tus.append(owner)
            else:
                skipped.append((f, "SKIPPED", "(header, no owner)"))
    return list(dict.fromkeys(tus)), skipped


# --- GPU-era compile DB ---------------------------------------------------------

def generated_includes(root, rel, extra_dirs):
    """Generated kernel headers rel reaches through quoted includes within the checkout."""
    found, seen, stack = set(), set(), [os.path.join(root, rel)]
    search = [root, os.path.join(root, "Engine", "Gpu")] + [
        d for d in extra_dirs if d.startswith(root + os.sep) and "/build/" not in d]
    while stack:
        path = stack.pop()
        if path in seen:
            continue
        seen.add(path)
        try:
            with open(path, errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for inc in INCLUDE_RE.findall(text):
            cand = [os.path.normpath(os.path.join(d, inc))
                    for d in [os.path.dirname(path)] + search]
            hit = next((c for c in cand if os.path.isfile(c)), None)
            if hit:
                if hit.startswith(root + os.sep) and "/libs/" not in hit:
                    stack.append(hit)
            elif GENERATED_INCLUDE.match(os.path.basename(inc)):
                found.add(os.path.basename(inc))
    return found


def newest_gpu_change(root):
    newest = 0.0
    p = git(root, "log", "-1", "--format=%ct", "--", *GPU_TRIGGER_PATHS, check=False)
    if p.stdout.strip():
        newest = float(p.stdout.strip())
    dirty = git(root, "status", "--porcelain", "--untracked-files=all", "--",
                *GPU_TRIGGER_PATHS, check=False).stdout.splitlines()
    for line in dirty:
        path = os.path.join(root, line[3:].split(" -> ")[-1])
        if os.path.exists(path):
            newest = max(newest, os.path.getmtime(path))
        else:
            newest = max(newest, time.time())
    return newest


DEPS = [
    ("SLANG_ROOT", "FetchSlang.cmake", "NATRON_SLANG_VERSION", "slang", "bin/slangc"),
    ("GLSLANG_ROOT", "FetchGlslang.cmake", "NATRON_GLSLANG_VERSION", "glslang", "CMakeLists.txt"),
]


def deps_root_args(root, main):
    args = []
    for var, cmake_file, version_var, prefix, marker in DEPS:
        if os.environ.get(var):
            args.append(f"-D{var}={os.environ[var]}")
            continue
        try:
            with open(os.path.join(root, "cmake", cmake_file)) as f:
                version = re.search(rf'set\({version_var}\s+"([^"]+)"\)', f.read()).group(1)
        except (OSError, AttributeError):
            continue
        name = f"{prefix}-{version}"
        if os.path.isfile(os.path.join(root, ".natron-deps", name, marker)):
            continue
        # A worktree has no .natron-deps of its own; reuse the main checkout's
        # rather than letting the Fetch*.cmake download a second copy.
        shared = os.path.join(main, ".natron-deps", name)
        if os.path.isfile(os.path.join(shared, marker)):
            args.append(f"-D{var}={shared}")
    return args


def ensure_gpu_db(root, main, needed_headers, moc_targets, cmake_files=(), generate=True):
    """Configures build/syntax and generates kernel headers; returns its CompDb.

    The tree is reconfigured when older than the GPU sources or cmake_files."""
    if ninja_running():
        die(f"ninja is already running in the '{CONTAINER}' container; it is single-tenant, "
            "so wait for that build to finish before a GPU syntax check reconfigures.", 1)
    bdir = os.path.join(root, "build", "syntax")
    db_path = os.path.join(bdir, "compile_commands.json")
    newest = max([newest_gpu_change(root), *map(os.path.getmtime, cmake_files)])
    if not os.path.isfile(db_path) or os.path.getmtime(db_path) < newest:
        os.makedirs(bdir, exist_ok=True)
        argv = ["cmake", "-S", root, "-B", bdir, *SYNTAX_CMAKE_ARGS, *deps_root_args(root, main)]
        print(f"configuring {bdir} ...", flush=True)
        t0 = time.time()
        p = container_shell(shlex.join(argv), root)
        if p.returncode != 0:
            print((p.stdout + p.stderr)[-4000:], file=sys.stderr)
            die(f"cmake configure of {bdir} failed", 1)
        print(f"configured in {time.time() - t0:.0f}s", flush=True)
        # CMake leaves an unchanged compile DB untouched, which would make it
        # look stale again on the next run.
        os.utime(db_path)

    if not generate:
        return CompDb(db_path, root)

    p = container_exec(["ninja", "-C", bdir, "-t", "targets", "all"], root)
    if p.returncode != 0:
        die(f"ninja -t targets failed in {bdir}: {p.stderr.strip()}", 1)
    targets = {line.split(":", 1)[0] for line in p.stdout.splitlines() if ":" in line}

    registry = os.path.join(bdir, "Engine", "Gpu", "gen", "GpuKernels.inc")
    wanted = set()
    if os.path.isfile(registry):
        with open(registry) as f:
            for k in re.findall(r'#include "(\w+)_spirv\.h"', f.read()):
                if f"{k}_gen" in targets:
                    wanted.add(f"{k}_gen")
    by_name = {}
    for t in targets:
        by_name.setdefault(os.path.basename(t), t)
    for h in needed_headers:
        if h in by_name:
            wanted.add(by_name[h])
    wanted |= {f"{t}_autogen" for t in moc_targets if f"{t}_autogen" in targets}

    if wanted:
        t0 = time.time()
        p = container_shell(shlex.join(["ninja", "-C", bdir, *sorted(wanted)]), root)
        if p.returncode != 0:
            print((p.stdout + p.stderr)[-4000:], file=sys.stderr)
            die(f"generating kernel headers in {bdir} failed", 1)
        print(f"generated {len(wanted)} target(s) in {time.time() - t0:.0f}s", flush=True)

    missing = []
    for h in sorted(needed_headers):
        path = by_name.get(h)
        if path is None:
            path = os.path.join(bdir, "Engine", "Gpu", "gen", h) if h == "GpuKernels.inc" else None
        if path is not None and not os.path.isabs(path):
            path = os.path.join(bdir, path)
        if path is None:
            missing.append(f"{h} (no target in {bdir} generates it)")
        elif not os.path.isfile(path):
            missing.append(path)
    if missing:
        die("generated kernel header missing after generation: " + ", ".join(missing), 1)
    return CompDb(db_path, root)


# --- main ------------------------------------------------------------------------

def parse_args(argv):
    ap = argparse.ArgumentParser(
        prog="syntax-check.py",
        usage="syntax-check.py [--root <checkout>] [--base <ref>] [--compdb <path>] [<file> ...]")
    ap.add_argument("--root", help="checkout whose sources are checked (default: the current one)")
    ap.add_argument("--base", help="diff base when no files are given (default: merge base with origin/main)")
    ap.add_argument("--compdb", help="compile_commands.json (default: <main checkout>/build/fast/...)")
    ap.add_argument("files", nargs="*")
    args = ap.parse_args(argv)

    main = main_checkout()
    root = args.root or toplevel(os.getcwd()) or toplevel(SCRIPT_DIR)
    root = os.path.realpath(root)
    if not os.path.isdir(root):
        raise UsageError(f"--root {args.root} is not a directory")

    for f in args.files:
        full = os.path.join(root, f)
        looks_like_ref = git(root, "rev-parse", "--verify", "--quiet", f + "^{commit}",
                             check=False).returncode == 0
        if os.path.isdir(f) or os.path.isdir(full) or (not os.path.exists(full) and looks_like_ref):
            raise UsageError(
                f"'{f}' is a directory or git ref, not a file. The checkout and base are options:\n"
                "    syntax-check.py --root <checkout> --base <ref> [<file> ...]")
        if not os.path.isfile(full):
            raise UsageError(f"{f}: no such file under {root}")
    return args, main, root


def main():
    try:
        args, main_root, root = parse_args(sys.argv[1:])
    except UsageError as e:
        print(f"usage: syntax-check.py [--root <checkout>] [--base <ref>] [--compdb <path>] [<file> ...]\n"
              f"syntax-check.py: error: {e}", file=sys.stderr)
        return 2

    compdb = args.compdb or os.path.join(main_root, "build", "fast", "compile_commands.json")
    if not os.path.isfile(compdb):
        die(f"no compile DB at {compdb}; build the fast preset (tools/ci/local/build.sh fast) "
            "or pass --compdb", 1)
    db = CompDb(compdb, root)

    if args.files:
        files = [os.path.normpath(f) for f in args.files]
    else:
        base = args.base
        if not base:
            p = git(root, "merge-base", "origin/main", "HEAD", check=False)
            if p.returncode != 0:
                die("cannot find the merge base with origin/main; pass --base <ref>")
            base = p.stdout.strip()
        files = changed_files(root, base)

    tus, results = select_tus(root, files)
    for rel, st, detail in results:
        print(f"{st:10} {rel} {detail}")
    if not tus:
        print("no translation units to check")
        return 0

    plain, gpu, needed, moc_targets = [], [], set(), set()
    for rel in tus:
        entry, _ = db.lookup(rel)
        dirs = include_dirs(db.argv_for(entry)) if entry else []
        gen = generated_includes(root, rel, dirs)
        if rel.startswith("Engine/Gpu/") or gen:
            gpu.append(rel)
            needed |= gen
            with open(os.path.join(root, rel), errors="replace") as f:
                if MOC_INCLUDE_RE.search(f.read()) and entry:
                    moc_targets.add(target_of(entry))
        else:
            plain.append(rel)

    stale = {rel: db.stale_cmake(rel) for rel in plain}
    jobs = [(rel, db) for rel in plain if not stale[rel]]
    fresh = [rel for rel in plain if stale[rel]]
    if gpu or fresh:
        if gpu and not fresh and db.mtime() >= newest_gpu_change(root):
            gpu_db = db
        else:
            gpu_db = ensure_gpu_db(root, main_root, needed, moc_targets,
                                   [stale[rel] for rel in fresh], generate=bool(gpu))
        jobs += [(rel, gpu_db) for rel in gpu + fresh]

    def run(job):
        rel, d = job
        entry, searched = d.lookup(rel)
        if entry is None:
            return rel, "NO-COMMAND", f"no compile command for it or a sibling under {searched} in {d.path}"
        p = container_exec(d.argv_for(entry), entry["directory"])
        text = (p.stdout + p.stderr).splitlines()
        errs = [l for l in text if "error" in l and not l.startswith("id: ")]
        return rel, "OK" if p.returncode == 0 else "FAIL", "\n".join(errs[:15])

    failed = False
    with ThreadPoolExecutor(WORKERS) as ex:
        for rel, st, detail in ex.map(run, sorted(jobs)):
            print(f"{st:10} {rel}")
            if detail:
                print(detail)
            failed |= st != "OK"
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
