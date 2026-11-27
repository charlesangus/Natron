// Runs every snippet through a hand-written C++ reference, the batched interpreter at each lane
// width, and the LLVM JIT, over the same paged detail, and prints runtime, compile latency and
// agreement with the reference.

#include "gk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <thread>

#ifndef GK_BUILTINS_BC
#define GK_BUILTINS_BC "builtins.bc"
#endif
#ifndef GK_INTERP_ARCH
#define GK_INTERP_ARCH "x86-64"
#endif

using namespace gk;
using Clock = std::chrono::steady_clock;

namespace {

double
msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

long
rssKb()
{
    std::ifstream f("/proc/self/status");
    std::string k;
    while (f >> k) {
        if (k == "VmRSS:") {
            long v;
            f >> v;
            return v;
        }
    }
    return 0;
}

struct Snippet {
    const char* name;
    const char* source;
};

const Snippet kSnippets[] = {
    { "add", R"(@P.y += 1;)" },
    { "maths", R"(vector p = @P;
float r = length(p);
float s = 1.0 + 0.1 * sin(r * 10.0 + @ptnum * 0.001);
@P = p * s;)" },
    { "noise", R"(float n = noise(@P * 4.0);
@P += @N * (n - 0.5) * 0.2;)" },
    { "nearpoint", R"(int j = nearpoint(1, @P);
vector q = point(1, "P", j);
@Cd = q - @P;)" },
    { "branchy", R"(float h = @P.y;
if (h > 0 && @ptnum % 3 == 0) {
    @P.y = h * 2.0 + sin(@P.x * 5.0);
} else {
    @P.x = -@P.x;
    @P.z += 0.5;
})" },
};

// Hand-written C++ for each snippet, in the same operation order, as the speed ceiling and the
// correctness reference.
class NativeKernel : public Kernel {
public:
    using Body = std::function<void(const Batch&, const Input1&)>;
    explicit NativeKernel(Body b)
        : _b(std::move(b))
    {
    }
    std::unique_ptr<void, void (*)(void*)> makeThreadState() const override
    {
        return { nullptr, [](void*) { } };
    }
    void run(void*, const Batch& b, const Input1& in1, const Uniforms&) const override { _b(b, in1); }

private:
    Body _b;
};

std::unique_ptr<Kernel>
nativeFor(const std::string& name)
{
    if (name == "add") {
        return std::make_unique<NativeKernel>([](const Batch& b, const Input1&) {
            const float* p = b.in[0];
            float* o = b.out[0];
            for (std::int64_t i = 0; i < b.count; ++i) {
                o[3 * i] = p[3 * i];
                o[3 * i + 1] = p[3 * i + 1] + 1.f;
                o[3 * i + 2] = p[3 * i + 2];
            }
        });
    }
    if (name == "maths") {
        return std::make_unique<NativeKernel>([](const Batch& b, const Input1&) {
            const float* p = b.in[0];
            float* o = b.out[0];
            for (std::int64_t i = 0; i < b.count; ++i) {
                const float x = p[3 * i], y = p[3 * i + 1], z = p[3 * i + 2];
                float d = x * x;
                d = y * y + d;
                d = z * z + d;
                const float r = std::sqrt(d);
                const float s = 1.0f + 0.1f * std::sin(r * 10.0f + float(std::int32_t(b.base + i)) * 0.001f);
                o[3 * i] = x * s;
                o[3 * i + 1] = y * s;
                o[3 * i + 2] = z * s;
            }
        });
    }
    if (name == "noise") {
        return std::make_unique<NativeKernel>([](const Batch& b, const Input1&) {
            const float* p = b.in[0];
            const float* nn = b.in[1];
            float* o = b.out[0];
            for (std::int64_t i = 0; i < b.count; ++i) {
                const float n = gkNoise3(p[3 * i] * 4.0f, p[3 * i + 1] * 4.0f, p[3 * i + 2] * 4.0f);
                for (int k = 0; k < 3; ++k) {
                    o[3 * i + k] = p[3 * i + k] + nn[3 * i + k] * (n - 0.5f) * 0.2f;
                }
            }
        });
    }
    if (name == "nearpoint") {
        return std::make_unique<NativeKernel>([](const Batch& b, const Input1& in1) {
            const float* p = b.in[0];
            float* cd = b.out[2];
            for (std::int64_t i = 0; i < b.count; ++i) {
                const std::int64_t j = nk_nearpoint(in1.grid, p[3 * i], p[3 * i + 1], p[3 * i + 2]);
                for (int k = 0; k < 3; ++k) {
                    float q = 0.f;
                    if (std::uint64_t(j) < std::uint64_t(in1.count)) {
                        q = in1.pageTable[j >> kPageBits][(j & (kPageSize - 1)) * 3 + k];
                    }
                    cd[3 * i + k] = q - p[3 * i + k];
                }
            }
        });
    }
    if (name == "branchy") {
        return std::make_unique<NativeKernel>([](const Batch& b, const Input1&) {
            const float* p = b.in[0];
            float* o = b.out[0];
            for (std::int64_t i = 0; i < b.count; ++i) {
                float x = p[3 * i], y = p[3 * i + 1], z = p[3 * i + 2];
                const float h = y;
                if (h > 0.f && std::int32_t(b.base + i) % 3 == 0) {
                    y = h * 2.0f + std::sin(x * 5.0f);
                } else {
                    x = -x;
                    z = z + 0.5f;
                }
                o[3 * i] = x;
                o[3 * i + 1] = y;
                o[3 * i + 2] = z;
            }
        });
    }
    return nullptr;
}

std::shared_ptr<Page>
densePage()
{
    auto p = std::make_shared<Page>();
    p->data.reset(new float[kPageSize * 3]);
    return p;
}

void
parallelFor(int threads, std::size_t count, const std::function<void(int, std::size_t)>& body)
{
    std::atomic<std::size_t> next { 0 };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (std::size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count;) {
                body(t, i);
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
}

Detail
makeDetail(std::size_t count, bool withN, std::uint64_t seed, int threads)
{
    Detail d;
    d.count = count;
    const std::size_t pages = (count + kPageSize - 1) / kPageSize;
    for (int a = 0; a < (withN ? 2 : 1); ++a) {
        auto col = std::make_shared<Column>();
        col->size = count;
        col->pages.resize(pages);
        parallelFor(threads, pages, [&](int, std::size_t pg) {
            auto page = densePage();
            std::mt19937_64 rng(seed * 1000003u + a * 7919u + pg);
            std::uniform_real_distribution<float> u(-1.f, 1.f);
            for (std::size_t e = 0; e < kPageSize; ++e) {
                float v[3] = { u(rng), u(rng), u(rng) };
                if (a == 1) {
                    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-6f;
                    v[0] /= l, v[1] /= l, v[2] /= l;
                }
                const bool live = pg * kPageSize + e < count;
                for (int k = 0; k < 3; ++k) {
                    page->data[e * 3 + k] = live ? v[k] : 0.f;
                }
            }
            col->pages[pg] = page;
        });
        d.attr[a] = col;
    }
    return d;
}

struct Grid {
    NearGrid g;
    std::vector<std::int32_t> cells;
};

Grid
makeGrid(const Detail& in1, int dim)
{
    Grid grid;
    grid.cells.assign(std::size_t(dim) * dim * dim, 0);
    grid.g = NearGrid { { -1.f, -1.f, -1.f }, dim / 2.f, dim, nullptr };
    for (std::size_t i = 0; i < in1.count; ++i) {
        const float* p = in1.attr[0]->pages[i >> kPageBits]->data.get() + (i & (kPageSize - 1)) * 3;
        grid.g.cells = grid.cells.data();
        std::int32_t c[3];
        for (int k = 0; k < 3; ++k) {
            c[k] = std::clamp(int((p[k] + 1.f) * grid.g.invCell), 0, dim - 1);
        }
        grid.cells[(std::size_t(c[2]) * dim + c[1]) * dim + c[0]] = std::int32_t(i);
    }
    grid.g.cells = grid.cells.data();
    return grid;
}

struct RunSetup {
    bool bound[kAttrCount] = {};
    bool written[kAttrCount] = {};
};

// One evaluation over the whole detail. When pool is given, its pages are reused as output
// pages (already allocated and touched); otherwise every written page is freshly allocated,
// as a copy-on-write detach would.
double
runKernel(const Kernel& k, const RunSetup& rs, const Detail& in, Detail& out, const Input1& in1, const Uniforms& u, int threads,
          std::vector<std::shared_ptr<Page>>* pool)
{
    const auto t0 = Clock::now();
    const std::size_t pages = (in.count + kPageSize - 1) / kPageSize;
    out = Detail {};
    out.count = in.count;
    std::shared_ptr<Column> src[kAttrCount];
    for (int a = 0; a < kAttrCount; ++a) {
        out.attr[a] = in.attr[a];
        src[a] = in.attr[a];
        if (rs.bound[a] && !src[a]) {
            auto zero = std::make_shared<Page>();
            zero->constant = true;
            zero->data.reset(new float[3] { 0.f, 0.f, 0.f });
            auto col = std::make_shared<Column>();
            col->size = in.count;
            col->pages.assign(pages, zero);
            src[a] = col;
        }
        if (rs.written[a]) {
            auto col = std::make_shared<Column>();
            col->size = in.count;
            col->pages = src[a]->pages;
            out.attr[a] = col;
        }
    }
    std::atomic<std::size_t> nextPage { 0 };
    parallelFor(threads, threads, [&](int, std::size_t) {
        auto state = k.makeThreadState();
        std::vector<float> expanded[kAttrCount];
        for (std::size_t pg; (pg = nextPage.fetch_add(1, std::memory_order_relaxed)) < pages;) {
            Batch b;
            b.base = std::int64_t(pg * kPageSize);
            b.count = std::int64_t(std::min(kPageSize, in.count - pg * kPageSize));
            for (int a = 0; a < kAttrCount; ++a) {
                if (!rs.bound[a]) {
                    continue;
                }
                const Page& p = *src[a]->pages[pg];
                if (p.constant) {
                    if (expanded[a].empty()) {
                        expanded[a].resize(kPageSize * 3);
                        for (std::size_t e = 0; e < kPageSize; ++e) {
                            std::memcpy(&expanded[a][e * 3], p.data.get(), sizeof(float) * 3);
                        }
                    }
                    b.in[a] = expanded[a].data();
                } else {
                    b.in[a] = p.data.get();
                }
                if (rs.written[a]) {
                    std::shared_ptr<Page> np = pool ? pool[a][pg] : densePage();
                    if (std::size_t(b.count) < kPageSize) {
                        std::fill(np->data.get() + b.count * 3, np->data.get() + kPageSize * 3, 0.f);
                    }
                    b.out[a] = np->data.get();
                    out.attr[a]->pages[pg] = std::move(np);
                }
            }
            k.run(state.get(), b, in1, u);
        }
    });
    return msSince(t0);
}

struct Diff {
    double maxAbs = 0;
    std::size_t notBitEqual = 0;
};

Diff
compare(const Detail& a, const Detail& b, const RunSetup& rs)
{
    Diff d;
    for (int at = 0; at < kAttrCount; ++at) {
        if (!rs.written[at]) {
            continue;
        }
        for (std::size_t i = 0; i < a.count; ++i) {
            const float* x = a.attr[at]->pages[i >> kPageBits]->data.get() + (i & (kPageSize - 1)) * 3;
            const float* y = b.attr[at]->pages[i >> kPageBits]->data.get() + (i & (kPageSize - 1)) * 3;
            for (int k = 0; k < 3; ++k) {
                if (std::memcmp(&x[k], &y[k], 4) != 0) {
                    ++d.notBitEqual;
                    const double e = std::fabs(double(x[k]) - double(y[k]));
                    d.maxAbs = std::isnan(e) ? INFINITY : std::max(d.maxAbs, e);
                }
            }
        }
    }
    return d;
}

std::vector<int>
parseList(const char* s)
{
    std::vector<int> out;
    for (const char* p = s; *p;) {
        out.push_back(std::atoi(p));
        p = std::strchr(p, ',');
        if (!p) {
            break;
        }
        ++p;
    }
    return out;
}

double
median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int
main(int argc, char** argv)
{
    std::size_t points = 10'000'000;
    std::size_t points1 = 1'000'000;
    int threads = int(std::thread::hardware_concurrency());
    int reps = 5;
    int edits = 20;
    int jitOpt = 2;
    std::vector<int> lanes = { 64, 256, 1024 };
    std::string only;
    std::string bc = GK_BUILTINS_BC;
    bool noJit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--points")
            points = std::strtoull(next(), nullptr, 10);
        else if (a == "--input1")
            points1 = std::strtoull(next(), nullptr, 10);
        else if (a == "--threads")
            threads = std::atoi(next());
        else if (a == "--reps")
            reps = std::atoi(next());
        else if (a == "--edits")
            edits = std::atoi(next());
        else if (a == "--jit-opt")
            jitOpt = std::atoi(next());
        else if (a == "--lanes")
            lanes = parseList(next());
        else if (a == "--snippet")
            only = next();
        else if (a == "--bc")
            bc = next();
        else if (a == "--no-jit")
            noJit = true;
        else {
            std::fprintf(stderr,
                         "usage: %s [--points N] [--input1 N] [--threads T] [--reps R] [--edits K] [--jit-opt 0-3]\n"
                         "          [--lanes 64,256,1024] [--snippet NAME] [--bc builtins.bc] [--no-jit]\n",
                         argv[0]);
            return 2;
        }
    }

    std::printf("# geo-kernel spike: %zu points, input 1 %zu points, %d threads, %d reps, interpreter built for %s\n", points, points1,
                threads, reps, GK_INTERP_ARCH);

    std::unique_ptr<JitEngine> jit;
    double jitInitMs = 0;
    long jitRssKb = 0;
    if (!noJit) {
        const long r0 = rssKb();
        const auto t0 = Clock::now();
        jit = makeJitEngine(jitOpt, bc);
        jitInitMs = msSince(t0);
        jitRssKb = rssKb() - r0;
        if (jit) {
            std::printf("# JIT: %s; engine start %.1f ms, +%ld KB RSS\n", jit->describe().c_str(), jitInitMs, jitRssKb);
        }
    }

    {
        Program p;
        CompileError e;
        const char* bad = "float a = 1;\n@P.y += frobnicate(a);";
        if (compileSource(bad, p, e)) {
            std::printf("# front end accepted an unsupported function\n");
            return 1;
        }
        std::printf("# error check: %d:%d %s\n", e.line, e.col, e.message.c_str());
    }

    const auto tGen = Clock::now();
    Detail in = makeDetail(points, true, 1, threads);
    Detail in1d = makeDetail(points1, false, 2, threads);
    Grid grid = makeGrid(in1d, 64);
    Input1 in1;
    for (const auto& pg : in1d.attr[0]->pages) {
        in1.pageTable.push_back(pg->data.get());
    }
    in1.count = std::int64_t(in1d.count);
    in1.grid = &grid.g;
    Uniforms u { 1.f, std::int32_t(points) };
    std::printf("# data generated in %.0f ms\n\n", msSince(tGen));

    std::printf("%-10s %-14s %9s %10s %10s %10s %8s %12s %10s\n", "snippet", "backend", "compile", "e2e min", "e2e med",
                "warm min", "ns/pt", "not-bit-eq", "max|diff|");
    std::printf("%-10s %-14s %9s %10s %10s %10s %8s %12s %10s\n", "", "", "ms", "ms", "ms", "ms", "warm", "vs native", "");

    struct Latency {
        std::string snippet;
        std::vector<double> interp, jit;
    };
    std::vector<Latency> latencies;
    const std::size_t pageCount = (points + kPageSize - 1) / kPageSize;

    for (const Snippet& s : kSnippets) {
        if (!only.empty() && only != s.name) {
            continue;
        }
        Program prog;
        CompileError err;
        if (!compileSource(s.source, prog, err)) {
            std::printf("%s: compile error %d:%d %s\n", s.name, err.line, err.col, err.message.c_str());
            return 1;
        }
        RunSetup rs;
        for (int a = 0; a < kAttrCount; ++a) {
            rs.bound[a] = prog.attrRead[a] || prog.attrWritten[a];
            rs.written[a] = prog.attrWritten[a];
        }
        std::vector<std::shared_ptr<Page>> pool[kAttrCount];
        for (int a = 0; a < kAttrCount; ++a) {
            if (rs.written[a]) {
                pool[a].resize(pageCount);
                parallelFor(threads, pageCount, [&](int, std::size_t pg) {
                    pool[a][pg] = densePage();
                    std::fill(pool[a][pg]->data.get(), pool[a][pg]->data.get() + kPageSize * 3, 0.f);
                });
            }
        }

        Detail ref;
        struct Entry {
            std::string name;
            std::function<std::unique_ptr<Kernel>()> make;
        };
        std::vector<Entry> entries;
        entries.push_back({ "native C++", [&] { return nativeFor(s.name); } });
        for (int l : lanes) {
            entries.push_back({ "interp/" + std::to_string(l), [&, l] { return compileInterpreter(prog, l); } });
        }
        if (jit) {
            entries.push_back({ "jit/O" + std::to_string(jitOpt), [&] { return jit->compile(prog); } });
        }
        for (Entry& en : entries) {
            const auto tc = Clock::now();
            std::unique_ptr<Kernel> k = en.make();
            const double compileMs = msSince(tc);
            if (!k) {
                std::printf("%-10s %-14s failed to compile\n", s.name, en.name.c_str());
                continue;
            }
            std::vector<double> e2e, warm;
            Detail out;
            for (int r = 0; r < reps; ++r) {
                out = Detail {};
                e2e.push_back(runKernel(*k, rs, in, out, in1, u, threads, nullptr));
            }
            for (int r = 0; r < reps; ++r) {
                Detail w;
                warm.push_back(runKernel(*k, rs, in, w, in1, u, threads, pool));
            }
            const double warmMin = *std::min_element(warm.begin(), warm.end());
            if (en.name == "native C++") {
                ref = std::move(out);
                std::printf("%-10s %-14s %9s %10.2f %10.2f %10.2f %8.2f %12s %10s\n", s.name, en.name.c_str(), "-",
                            *std::min_element(e2e.begin(), e2e.end()), median(e2e), warmMin, warmMin * 1e6 / double(points), "-",
                            "-");
                continue;
            }
            const Diff d = compare(ref, out, rs);
            std::printf("%-10s %-14s %9.2f %10.2f %10.2f %10.2f %8.2f %12zu %10.3g\n", s.name, en.name.c_str(), compileMs,
                        *std::min_element(e2e.begin(), e2e.end()), median(e2e), warmMin, warmMin * 1e6 / double(points),
                        d.notBitEqual, d.maxAbs);
        }
        std::printf("%-10s %-14s %zu bytecode instructions per batch\n\n", "", "", interpreterInstructionCount(prog));

        Latency lat { s.name, {}, {} };
        for (int e = 0; e < edits; ++e) {
            const std::string src = std::string(s.source) + "\nfloat edit_ = " + std::to_string(e) + ".5;\n";
            {
                const auto t0 = Clock::now();
                Program p;
                CompileError ce;
                compileSource(src, p, ce);
                auto k = compileInterpreter(p, 256);
                lat.interp.push_back(msSince(t0));
            }
            if (jit) {
                const auto t0 = Clock::now();
                Program p;
                CompileError ce;
                compileSource(src, p, ce);
                auto k = jit->compile(p);
                lat.jit.push_back(msSince(t0));
            }
        }
        latencies.push_back(std::move(lat));
    }

    if (edits <= 0) {
        return 0;
    }
    std::printf("compile latency per edit (front end + backend, %d edits each), ms:\n", edits);
    std::printf("%-10s %12s %12s %12s %12s\n", "snippet", "interp med", "interp max", "jit med", "jit max");
    for (const Latency& l : latencies) {
        auto mx = [](const std::vector<double>& v) { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); };
        std::printf("%-10s %12.3f %12.3f %12.2f %12.2f\n", l.snippet.c_str(), median(l.interp), mx(l.interp),
                    l.jit.empty() ? 0.0 : median(l.jit), mx(l.jit));
    }
    return 0;
}
