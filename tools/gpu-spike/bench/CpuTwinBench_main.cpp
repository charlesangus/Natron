// CPU benchmark of the Slang C++-target twins (Grade, FIR Blur) against the engine's
// hand-written CPU code, RGBA float, all threads.
//
// The native Blur is the engine's BlurKernels.cpp compiled from source. The native Grade is
// GradeKernel::processRow from Engine/Nodes/Color/Grade.cpp copied verbatim (that file cannot be
// linked here). The native Blur runs on planar buffers like the Blur node's passes and excludes
// the node's interleave and write-back copies; the twins work on interleaved data directly.
//
//   CpuTwinBench [--sizes uhd,8k] [--threads N] [--reps N] [--sigmas 3,25,100]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "slang-cpp-prelude.h"

#include "../ref/BlurRef.h"
#include "../tests/BlurFixture.h"
#include "../tests/GradeFixture.h"
#include "BlurKernels.h"

extern "C" void grade_main(ComputeVaryingInput*, void* entryPointParams, void* globalParams);
extern "C" void blurPass(ComputeVaryingInput*, void* entryPointParams, void* globalParams);

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kChannels = 4;
constexpr uint32_t kBlurGroup = 128;
constexpr uint32_t kGradeTile = 8;

struct GradeGlobals
{
    StructuredBuffer<float> srcBuf;
    RWStructuredBuffer<float> dstBuf;
    gradefix::GradeParams* params;
};

struct BlurGlobals
{
    StructuredBuffer<float> srcBuf;
    RWStructuredBuffer<float> dstBuf;
    StructuredBuffer<float> weights;
    blurfix::BlurParams* params;
};

int gThreads = 16;

// Runs fn(unit) for unit in [0, units) over gThreads workers pulling `chunk` units at a time.
void parallelFor(int units, int chunk, const std::function<void(int, int, int)>& fn)
{
    std::atomic<int> next{0};
    auto worker = [&](int id) {
        for (;;) {
            const int first = next.fetch_add(chunk);
            if (first >= units)
                return;
            fn(first, std::min(units, first + chunk), id);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < gThreads; ++t)
        pool.emplace_back(worker, t);
    worker(0);
    for (auto& th : pool)
        th.join();
}

double medianMs(const std::function<void()>& prepare, const std::function<void()>& run, int reps)
{
    std::vector<double> ms;
    for (int i = 0; i < reps + 1; ++i) {
        prepare();
        const auto t0 = Clock::now();
        run();
        const double d = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (i > 0 || d > 3000.0 || reps == 1)
            ms.push_back(d);
        if (d > 3000.0 && i == 0)
            break;
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

// The knobs of a typical grade: multiply 1.2, offset 0.02, gamma 1.8, clamp black, RGB only.
constexpr double kMul = 1.2, kOff = 0.02, kGamma = 1.8;

struct GradeChannel
{
    double a, b, gamma;
};

// GradeKernel::processRow with the PixelKernel plumbing reduced to the arguments it reads.
struct NativeGrade
{
    GradeChannel channels[4];
    bool reverse = false, clampBlack = true, clampWhite = false;

    void processRow(const float* src, float* dst, int width, int nComps, const bool chan[4]) const
    {
        int bits[4] = {-1, -1, -1, -1};
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            const int bit = (nComps == 1) ? 3 : c;
            bits[c] = chan[bit] ? bit : -1;
        }
        for (int x = 0; x < width; ++x, src += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (bits[c] < 0)
                    continue;
                const GradeChannel& ch = channels[bits[c]];
                double v = src[c];
                v = reverse ? invgrade(v, ch) : grade(v, ch);
                if (clampBlack)
                    v = (std::max)(0., v);
                if (clampWhite)
                    v = (std::min)(1., v);
                dst[c] = (float)v;
            }
        }
    }

    static double grade(double v, const GradeChannel& ch)
    {
        const double x = (ch.a * v) + ch.b;
        if (ch.gamma <= 0) {
            if (x < 1.)
                return 0.;
            else if (x == 1.)
                return 1.;
            return std::numeric_limits<double>::infinity();
        }
        if (ch.gamma == 1.)
            return x;
        if (x <= 0)
            return x;
        return std::pow(x, 1. / ch.gamma);
    }

    static double invgrade(double v, const GradeChannel& ch)
    {
        if ((ch.gamma != 1.) && (v > 0))
            v = std::pow(v, ch.gamma);
        v = v - ch.b;
        if (ch.a != 0)
            v /= ch.a;
        return v;
    }
};

struct Frame
{
    int w, h;
    std::vector<float> rgba;
};

Frame makeFrame(int w, int h)
{
    Frame f{w, h, std::vector<float>(size_t(w) * h * kChannels)};
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> d(-0.1f, 1.5f);
    for (float& v : f.rgba)
        v = d(rng);
    return f;
}

void runGradeTwin(const Frame& in, std::vector<float>& out)
{
    gradefix::GradeParams p{};
    for (int i = 0; i < 4; ++i) {
        p.a[i] = float(kMul);
        p.b[i] = float(kOff);
        p.gamma[i] = float(kGamma);
        p.invGamma[i] = float(1.0 / kGamma);
    }
    p.width = in.w;
    p.height = in.h;
    p.nComps = kChannels;
    p.channelMask = 7;
    p.flags = gradefix::kClampBlack;
    GradeGlobals g{{const_cast<float*>(in.rgba.data()), in.rgba.size()}, {out.data(), out.size()}, &p};
    const uint32_t gx = (in.w + kGradeTile - 1) / kGradeTile;
    const uint32_t gy = (in.h + kGradeTile - 1) / kGradeTile;
    parallelFor(int(gy), 1, [&](int first, int end, int) {
        ComputeVaryingInput vi = {};
        vi.startGroupID = {0, uint32_t(first), 0};
        vi.endGroupID = {gx, uint32_t(end), 1};
        grade_main(&vi, nullptr, &g);
    });
}

void runGradeNative(const Frame& in, std::vector<float>& out)
{
    NativeGrade k;
    for (int i = 0; i < 4; ++i)
        k.channels[i] = {kMul, kOff, kGamma};
    const bool chan[4] = {true, true, true, false};
    parallelFor(in.h, 4, [&](int first, int end, int) {
        for (int y = first; y < end; ++y)
            k.processRow(&in.rgba[size_t(y) * in.w * kChannels], &out[size_t(y) * in.w * kChannels], in.w, kChannels, chan);
    });
}

void runBlurTwin(const Frame& in, std::vector<float>& tmp, std::vector<float>& out, double sigma, bool neumann)
{
    const std::vector<double> wd = blurref::makeWeights(sigma);
    const std::vector<float> wf(wd.begin(), wd.end());
    blurfix::BlurParams p{uint32_t(in.w), uint32_t(in.h), uint32_t(kChannels), uint32_t(wd.size() / 2), 0, neumann ? 1u : 0u};
    for (int pass = 0; pass < 2; ++pass) {
        p.vertical = pass;
        const std::vector<float>& src = pass ? tmp : in.rgba;
        std::vector<float>& dst = pass ? out : tmp;
        BlurGlobals g{{const_cast<float*>(src.data()), src.size()},
                      {dst.data(), dst.size()},
                      {const_cast<float*>(wf.data()), wf.size()},
                      &p};
        const uint32_t len = pass ? in.h : in.w;
        const uint32_t lines = pass ? in.w : in.h;
        const uint32_t gx = (len + kBlurGroup - 1) / kBlurGroup;
        parallelFor(int(lines), pass ? 8 : 2, [&](int first, int end, int) {
            ComputeVaryingInput vi = {};
            vi.startGroupID = {0, uint32_t(first), 0};
            vi.endGroupID = {gx, uint32_t(end), 1};
            blurPass(&vi, nullptr, &g);
        });
    }
}

using BlurKernels::LineFilter;
using BlurKernels::LineScratch;

void runBlurNative(std::vector<float>& planes, int w, int h, const LineFilter& f, bool fir)
{
    const size_t planeSize = size_t(w) * h;
    parallelFor(kChannels * h, 4, [&](int first, int end, int) {
        LineScratch s;
        for (int line = first; line < end; ++line)
            f.apply(&planes[size_t(line / h) * planeSize + size_t(line % h) * w], w, 1, s);
    });
    if (fir) {
        const int block = LineFilter::kColumnBlock;
        const int perPlane = (w + block - 1) / block;
        parallelFor(kChannels * perPlane, 1, [&](int first, int end, int) {
            LineScratch s;
            for (int line = first; line < end; ++line) {
                const int p = line / perPlane;
                const int col = (line % perPlane) * block;
                f.applyColumns(&planes[size_t(p) * planeSize + col], h, w, std::min(block, w - col), s);
            }
        });
    } else {
        parallelFor(kChannels * w, 16, [&](int first, int end, int) {
            LineScratch s;
            for (int line = first; line < end; ++line)
                f.apply(&planes[size_t(line / w) * planeSize + line % w], h, w, s);
        });
    }
}

void toPlanar(const Frame& in, std::vector<float>& planes)
{
    const size_t n = size_t(in.w) * in.h;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < kChannels; ++c)
            planes[c * n + i] = in.rgba[i * kChannels + c];
}

double maxDiffPlanar(const std::vector<float>& planes, const std::vector<float>& inter, size_t n)
{
    double worst = 0;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < kChannels; ++c)
            worst = std::max(worst, double(std::fabs(planes[c * n + i] - inter[i * kChannels + c])));
    return worst;
}

void row(const char* size, const char* work, const char* impl, double ms, double mpx)
{
    std::printf("%-5s %-14s %-22s %10.1f ms %9.2f ms/Mpx\n", size, work, impl, ms, ms / mpx);
    std::fflush(stdout);
}

std::vector<double> parseList(const std::string& s)
{
    std::vector<double> v;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ','))
        v.push_back(std::atof(t.c_str()));
    return v;
}

} // namespace

int main(int argc, char** argv)
{
    std::string sizes = "uhd,8k";
    std::vector<double> sigmas = {3, 25, 100};
    int reps = 3;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--sizes")
            sizes = argv[i + 1];
        else if (a == "--threads")
            gThreads = std::atoi(argv[i + 1]);
        else if (a == "--reps")
            reps = std::atoi(argv[i + 1]);
        else if (a == "--sigmas")
            sigmas = parseList(argv[i + 1]);
    }
    std::printf("threads %d, reps %d (median)\n", gThreads, reps);

    {
        const Frame f = makeFrame(517, 389);
        std::vector<float> a(f.rgba.size()), b(f.rgba.size()), tmp(f.rgba.size());
        runGradeTwin(f, a);
        runGradeNative(f, b);
        double gd = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if ((i % kChannels) != 3 && std::isfinite(a[i]) && std::isfinite(b[i]))
                gd = std::max(gd, double(std::fabs(a[i] - b[i])));
        runBlurTwin(f, tmp, a, 3.0, true);
        std::vector<float> planes(f.rgba.size());
        toPlanar(f, planes);
        runBlurNative(planes, f.w, f.h, LineFilter::firGaussian(3.0, 0, true), true);
        std::printf("check 517x389: grade max|twin-native| %.2e, blur sigma 3 max|twin-native| %.2e\n", gd,
                    maxDiffPlanar(planes, a, size_t(f.w) * f.h));
    }

    std::stringstream ss(sizes);
    std::string name;
    while (std::getline(ss, name, ',')) {
        const int w = name == "8k" ? 7680 : 3840, h = name == "8k" ? 4320 : 2160;
        const double mpx = double(w) * h / 1e6;
        const Frame in = makeFrame(w, h);
        std::vector<float> out(in.rgba.size()), tmp(in.rgba.size()), planes(in.rgba.size());
        const auto none = [] {};

        row(name.c_str(), "Grade", "twin", medianMs(none, [&] { runGradeTwin(in, out); }, reps), mpx);
        row(name.c_str(), "Grade", "native", medianMs(none, [&] { runGradeNative(in, out); }, reps), mpx);

        for (double sigma : sigmas) {
            char work[32];
            std::snprintf(work, sizeof work, "Blur s%g", sigma);
            row(name.c_str(), work, "twin FIR", medianMs(none, [&] { runBlurTwin(in, tmp, out, sigma, true); }, reps), mpx);
            struct Native
            {
                const char* label;
                LineFilter f;
                bool fir;
            };
            const Native natives[] = {
                {"native FIR", LineFilter::firGaussian(sigma, 0, true), true},
                {"native IIR (vanVliet)", LineFilter::vanVliet(float(sigma), 0, true), false},
                {"native box (2.4s)", LineFilter::box(float(sigma * 2.4), 0, true, 1), false},
            };
            for (const Native& n : natives)
                row(name.c_str(), work, n.label,
                    medianMs([&] { toPlanar(in, planes); }, [&] { runBlurNative(planes, w, h, n.f, n.fir); }, reps), mpx);
        }
    }
    return 0;
}
