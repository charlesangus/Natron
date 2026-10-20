#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "GpuBuffer.h"
#include "GpuRecord.h"
#include "GpuRuntime.h"
#include "GpuTestDevice.h"
#include "GpuWrap.h"
#include "compositestep_halveadd_spirv.h"
#include "pixelwrap_identity_spirv.h"
#include "pixelwrap_scale_spirv.h"

using namespace gpu;

namespace {

// Mirrors of the Slang C++ prelude's ComputeVaryingInput and StructuredBuffer, so the prelude's
// global type names stay out of a file that also includes the Vulkan headers.
struct TwinVarying {
    uint32_t startGroup[3];
    uint32_t endGroup[3];
};

struct TwinBuffer {
    void* data;
    size_t count;
};

struct TwinGlobals {
    TwinBuffer src;
    TwinBuffer dst;
    TwinBuffer mask;
    TwinBuffer aux;
    GpuWrapParams* params;
};

// The C++ target lays out an importing module's globals before those it imports.
struct TwinStepGlobals {
    TwinBuffer input;
    TwinBuffer src;
    TwinBuffer dst;
    TwinBuffer mask;
    TwinBuffer aux;
    GpuWrapParams* params;
};

using TwinEntry = void (*)(TwinVarying*, void*, void*);

} // namespace

extern "C" void pixelwrap_identity(TwinVarying*, void*, void*);
extern "C" void pixelwrap_scale(TwinVarying*, void*, void*);
extern "C" void wrap_divide(TwinVarying*, void*, void*);
extern "C" void wrap_finish(TwinVarying*, void*, void*);
extern "C" void compositestep_halveadd(TwinVarying*, void*, void*);

namespace {

enum class Kernel {
    Identity,
    Scale,
    Divide,
    Finish
};

const char*
kernelName(Kernel k)
{
    switch (k) {
    case Kernel::Identity:
        return "pixelwrap_identity";
    case Kernel::Scale:
        return "pixelwrap_scale";
    case Kernel::Divide:
        return "wrap_divide";
    case Kernel::Finish:
        return "wrap_finish";
    }
    return "?";
}

TwinEntry
twinEntry(Kernel k)
{
    switch (k) {
    case Kernel::Identity:
        return pixelwrap_identity;
    case Kernel::Scale:
        return pixelwrap_scale;
    case Kernel::Divide:
        return wrap_divide;
    case Kernel::Finish:
        return wrap_finish;
    }
    return nullptr;
}

int
channelBit(int nComps, int c)
{
    return nComps == 1 ? 3 : c;
}

struct HostView {
    std::vector<float> data;
    GpuWrapRect bounds;
    uint64_t offset = 0;
    uint32_t stride = 0;
    uint32_t nComps = 0;

    bool inside(int x, int y) const { return x >= bounds.x1 && x < bounds.x2 && y >= bounds.y1 && y < bounds.y2; }

    size_t index(int x, int y, int c) const
    {
        return size_t(offset + uint64_t(y - bounds.y1) * stride + uint64_t(x - bounds.x1) * nComps + c);
    }

    GpuWrapView view(const GpuBuffer* buffer) const { return { buffer, offset, bounds, stride, nComps }; }
};

HostView
makeView(GpuWrapRect bounds, uint32_t nComps, uint64_t offset, uint32_t padPixels, float fill)
{
    HostView v;
    v.bounds = bounds;
    v.nComps = nComps;
    v.offset = offset;
    const uint32_t w = bounds.empty() ? 0 : uint32_t(bounds.x2 - bounds.x1);
    const uint32_t h = bounds.empty() ? 0 : uint32_t(bounds.y2 - bounds.y1);
    v.stride = (w + padPixels) * nComps;
    v.data.assign(size_t(offset + uint64_t(h) * v.stride + 3), fill);
    return v;
}

struct Case {
    std::string label;
    uint32_t nComps = 4;
    uint32_t channelMask = 0xF;
    GpuWrapRect output;
    HostView src;
    HostView dst;
    bool applyMask = false;
    HostView mask;
    uint32_t maskChannel = 0;
    bool maskInvert = false;
    float mix = 1.0f;
    int32_t divisorChannel = -1;
    int32_t skipChannel = -1;
    HostView filtered;

    GpuWrap wrap() const
    {
        GpuWrap w;
        w.src = src.view(nullptr);
        w.dst = dst.view(nullptr);
        w.output = output;
        w.nComps = nComps;
        w.channelMask = channelMask;
        w.applyMask = applyMask;
        w.mask = mask.view(nullptr);
        w.maskChannel = maskChannel;
        w.maskInvert = maskInvert;
        w.mix = mix;
        w.divisorChannel = divisorChannel;
        w.skipChannel = skipChannel;
        return w;
    }
};

constexpr float kSentinel = -777.0f;

// A divisor value per pixel covering every branch of Image::unPremultDivisorIsUsable.
float
divisorSample(int x, int y, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u(0.05f, 1.5f);
    switch (((x * 7 + y * 3) % 10 + 10) % 10) {
    case 0:
        return 0.0f;
    case 1:
        return -0.5f;
    case 2:
        return std::numeric_limits<float>::quiet_NaN();
    case 3:
        return 1e-8f;
    case 4:
        return FLT_EPSILON;
    case 5:
        return 2.5e-7f;
    default:
        return u(rng);
    }
}

void
fillSource(HostView& src, int divisorChannel, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u(-0.5f, 1.5f);
    for (float& v : src.data) {
        v = u(rng);
    }
    if (divisorChannel < 0 || src.bounds.empty()) {
        return;
    }
    for (int y = src.bounds.y1; y < src.bounds.y2; ++y) {
        for (int x = src.bounds.x1; x < src.bounds.x2; ++x) {
            src.data[src.index(x, y, divisorChannel)] = divisorSample(x, y, rng);
        }
    }
}

void
fillMask(HostView& mask, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (float& v : mask.data) {
        v = u(rng);
    }
    for (int y = mask.bounds.y1; y < mask.bounds.y2; ++y) {
        for (int x = mask.bounds.x1; x < mask.bounds.x2; ++x) {
            const int k = ((x + 2 * y) % 5 + 5) % 5;
            if (k == 0) {
                mask.data[mask.index(x, y, 1)] = 0.0f;
            } else if (k == 1) {
                mask.data[mask.index(x, y, 1)] = 1.0f;
            }
        }
    }
}

// Channel counts and subsets, no / partial / inverted mask, mix 0, 0.5 and 1, no divisor or one
// with and without a skip channel, over a source smaller than the output, one covering it and
// none at all, in views with nonzero offsets and padded strides.
std::vector<Case>
makeCases(Kernel kernel)
{
    const GpuWrapRect output { -5, 3, 32, 22 };
    const bool blends = kernel != Kernel::Divide;
    std::vector<Case> cases;
    std::mt19937 rng(1234 + int(kernel));
    for (uint32_t n : { 1u, 3u, 4u }) {
        std::vector<uint32_t> subsets;
        if (n == 1) {
            subsets = { 0x8, 0x7 };
        } else if (n == 3) {
            subsets = { 0x7, 0x2, 0xF };
        } else {
            subsets = { 0xF, 0x7, 0x8, 0x5 };
        }
        if (!blends) {
            subsets.resize(1);
        }
        const int32_t div = n == 1 ? 0 : (n == 3 ? 1 : 3);
        const int32_t divisors[3][2] = { { -1, -1 }, { div, div }, { div, -1 } };
        for (uint32_t subset : subsets) {
            for (int maskMode = 0; maskMode < (blends ? 3 : 1); ++maskMode) {
                for (float mix : { 0.0f, 0.5f, 1.0f }) {
                    if (!blends && mix != 1.0f) {
                        continue;
                    }
                    for (const auto& dv : divisors) {
                        const size_t index = cases.size();
                        Case c;
                        c.nComps = n;
                        c.channelMask = subset;
                        c.output = output;
                        c.mix = mix;
                        c.divisorChannel = dv[0];
                        c.skipChannel = dv[1];
                        const int layout = index % 7 == 6 ? 2 : int(index % 2);
                        if (layout == 0) {
                            c.src = makeView({ 0, 6, 20, 17 }, n, 11, 3, 0.0f);
                        } else if (layout == 1) {
                            c.src = makeView({ -9, 0, 40, 30 }, n, 0, 0, 0.0f);
                        } else {
                            c.src = makeView({}, n, 0, 0, 0.0f);
                        }
                        fillSource(c.src, c.divisorChannel, rng);
                        c.dst = makeView({ -7, 1, 35, 25 }, n, 5, 2, kSentinel);
                        if (maskMode > 0) {
                            c.applyMask = true;
                            c.maskInvert = maskMode == 2;
                            c.mask = makeView({ 2, -1, 25, 15 }, 2, 3, 2, 0.0f);
                            c.maskChannel = 1;
                            fillMask(c.mask, rng);
                        }
                        if (kernel == Kernel::Finish) {
                            c.filtered = makeView({ -6, 2, 33, 23 }, n, 9, 1, 0.0f);
                            std::uniform_real_distribution<float> u(-1.0f, 2.0f);
                            for (float& v : c.filtered.data) {
                                v = u(rng);
                            }
                        }
                        char label[160];
                        std::snprintf(label, sizeof label,
                                      "%s n=%u channels=0x%x mask=%d mix=%g divisor=%d skip=%d source=%d",
                                      kernelName(kernel), n, subset, maskMode, double(mix), dv[0], dv[1], layout);
                        c.label = label;
                        cases.push_back(std::move(c));
                    }
                }
            }
        }
    }
    return cases;
}

struct Expected {
    std::vector<float> value;
    // Elements whose value went through a division, which Vulkan allows 2.5 ULP.
    std::vector<uint8_t> divided;
    // The largest term of the element's arithmetic, against which the division's error is relative.
    std::vector<double> magnitude;
};

// A transcription of NativeImageEffect::render()'s per-pixel wrapping, with readSourceRow's and
// readChannelRow's fills outside the images.
Expected
reference(Kernel kernel, const Case& c)
{
    Expected e;
    e.value = c.dst.data;
    e.divided.assign(e.value.size(), 0);
    e.magnitude.assign(e.value.size(), 0.0);
    const int n = int(c.nComps);
    for (int y = c.output.y1; y < c.output.y2; ++y) {
        for (int x = c.output.x1; x < c.output.x2; ++x) {
            float sourcePix[4] = { 0, 0, 0, 0 };
            if (c.src.inside(x, y)) {
                for (int ch = 0; ch < n; ++ch) {
                    sourcePix[ch] = c.src.data[c.src.index(x, y, ch)];
                }
            }
            float dividedPix[4];
            std::copy(sourcePix, sourcePix + 4, dividedPix);
            float d = 1.f;
            bool divides = false;
            if (c.divisorChannel >= 0) {
                d = c.src.inside(x, y) ? sourcePix[c.divisorChannel] : 1.f;
                if (!(d <= (float)FLT_EPSILON)) {
                    divides = true;
                    for (int ch = 0; ch < n; ++ch) {
                        if (ch != c.skipChannel) {
                            dividedPix[ch] = dividedPix[ch] / d;
                        }
                    }
                }
            }
            if (kernel == Kernel::Divide) {
                for (int ch = 0; ch < n; ++ch) {
                    const size_t i = c.dst.index(x, y, ch);
                    e.value[i] = dividedPix[ch];
                    e.divided[i] = divides && ch != c.skipChannel;
                    e.magnitude[i] = std::fabs(dividedPix[ch]);
                }
                continue;
            }

            float kernelPix[4] = { 0, 0, 0, 0 };
            for (int ch = 0; ch < n; ++ch) {
                if (kernel == Kernel::Finish) {
                    kernelPix[ch] = c.filtered.data[c.filtered.index(x, y, ch)];
                } else if (c.channelMask & (1u << channelBit(n, ch))) {
                    kernelPix[ch] = kernel == Kernel::Scale ? dividedPix[ch] * 2.f : dividedPix[ch];
                }
            }

            float alpha = c.mix;
            if (c.applyMask) {
                const float m = c.mask.inside(x, y) ? c.mask.data[c.mask.index(x, y, int(c.maskChannel))] : 0.f;
                const float maskScale = c.maskInvert ? (1.f - m) : m;
                alpha = maskScale * c.mix;
            }
            for (int ch = 0; ch < n; ++ch) {
                const size_t i = c.dst.index(x, y, ch);
                const float srcV = sourcePix[ch];
                if (!(c.channelMask & (1u << channelBit(n, ch)))) {
                    e.value[i] = srcV;
                    continue;
                }
                float v = kernelPix[ch];
                if (c.divisorChannel >= 0 && ch != c.skipChannel) {
                    v = v * std::max(0.f, d);
                }
                const float unblended = v;
                if (alpha == 0.f) {
                    v = srcV;
                } else if (alpha != 1.f) {
                    v = v * alpha + (1.f - alpha) * srcV;
                }
                e.value[i] = v;
                e.divided[i] = kernel != Kernel::Finish && divides && ch != c.skipChannel && alpha != 0.f;
                e.magnitude[i] = std::max({ double(std::fabs(v)), std::fabs(double(unblended) * alpha),
                                            std::fabs(double(1.f - alpha) * srcV) });
            }
        }
    }
    return e;
}

// Without a division, equal as floats (the sign of zero is not compared, Vulkan not guaranteeing
// it without float controls) with any NaN matching any NaN; where one divides, within
// 3 * FLT_EPSILON of the largest term.
struct Comparison {
    uint64_t exactCompared = 0;
    uint64_t dividedCompared = 0;
    uint64_t nanCompared = 0;
    double worstRelative = 0;
    uint64_t failures = 0;

    void check(const Case& c, const Expected& e, const std::vector<float>& got)
    {
        if (got.size() != e.value.size()) {
            ADD_FAILURE() << c.label << ": got " << got.size() << " floats, want " << e.value.size();
            ++failures;
            return;
        }
        for (size_t i = 0; i < got.size(); ++i) {
            const float want = e.value[i];
            const float g = got[i];
            bool ok;
            if (std::isnan(want) || std::isnan(g)) {
                ok = std::isnan(want) && std::isnan(g);
                ++nanCompared;
            } else if (!e.divided[i] || std::isinf(want)) {
                ok = g == want;
                ++exactCompared;
            } else {
                const double diff = std::fabs(double(g) - double(want));
                const double bound = 3.0 * FLT_EPSILON * e.magnitude[i];
                ok = diff <= bound;
                if (e.magnitude[i] > 0) {
                    worstRelative = std::max(worstRelative, diff / (FLT_EPSILON * e.magnitude[i]));
                }
                ++dividedCompared;
            }
            if (!ok && ++failures <= 20) {
                ADD_FAILURE() << c.label << ": element " << i << " got " << g << " want " << want
                              << (e.divided[i] ? " (divided)" : " (exact)");
            }
        }
    }

    void print(const char* what) const
    {
        std::printf("%s: exact=%llu divided=%llu nan=%llu worst divided error=%.3g FLT_EPSILON x magnitude\n", what,
                    (unsigned long long)exactCompared, (unsigned long long)dividedCompared,
                    (unsigned long long)nanCompared, worstRelative);
    }
};

std::vector<float>
runTwin(Kernel kernel, const Case& c)
{
    GpuWrapParams p;
    const GpuWrapView filtered = c.filtered.view(nullptr);
    const GpuStatus s = wrapParams(c.wrap(), p, kernel == Kernel::Finish ? &filtered : nullptr);
    EXPECT_TRUE(s.ok()) << c.label << ": " << s.message;
    std::vector<float> dst = c.dst.data;
    std::vector<float> unused(16, 0.0f);
    std::vector<float> src = c.src.data.empty() ? unused : c.src.data;
    std::vector<float> mask = c.mask.data.empty() ? unused : c.mask.data;
    std::vector<float> aux = kernel == Kernel::Finish ? c.filtered.data : unused;
    TwinGlobals g { { src.data(), src.size() }, { dst.data(), dst.size() }, { mask.data(), mask.size() }, { aux.data(), aux.size() }, &p };
    TwinVarying vi { { 0, 0, 0 }, { (p.width + 7) / 8, (p.height + 7) / 8, 1 } };
    twinEntry(kernel)(&vi, nullptr, &g);
    return dst;
}

void
checkTwin(Kernel kernel)
{
    Comparison cmp;
    for (const Case& c : makeCases(kernel)) {
        cmp.check(c, reference(kernel, c), runTwin(kernel, c));
    }
    cmp.print((std::string(kernelName(kernel)) + " cpu twin vs reference").c_str());
    EXPECT_EQ(cmp.failures, 0u);
    EXPECT_GT(cmp.nanCompared, 0u);
    if (kernel != Kernel::Finish) {
        EXPECT_GT(cmp.dividedCompared, 0u);
    }
}

// A Composite op's fold under the halve-and-add step, and its finish: base is the result the fold
// starts from, inputs are folded into it in order, and the Case's src is the blend input.
struct FoldCase {
    Case finish;
    HostView base;
    std::vector<HostView> inputs;

    std::vector<GpuWrapView> inputViews() const
    {
        std::vector<GpuWrapView> views;
        for (const HostView& v : inputs) {
            views.push_back(v.view(nullptr));
        }
        return views;
    }
};

// Folds of `count` inputs, the base included, over channel counts and subsets, no / partial /
// inverted mask and mix 0, 0.5 and 1, as makeCases(), with a blend input other than the base, and
// a base and inputs that are smaller than the output, cover it, or are missing, the inputs in the
// output's channel count and in others.
std::vector<FoldCase>
makeFoldCases(uint32_t count)
{
    const GpuWrapRect output { -5, 3, 32, 22 };
    const GpuWrapRect inputBounds[3] = { { 0, 6, 20, 17 }, { -9, 0, 40, 30 }, { 4, -2, 27, 12 } };
    std::vector<FoldCase> cases;
    std::mt19937 rng(4321 + int(count));
    for (uint32_t n : { 1u, 3u, 4u }) {
        std::vector<uint32_t> subsets;
        if (n == 1) {
            subsets = { 0x8, 0x7 };
        } else if (n == 3) {
            subsets = { 0x7, 0x2, 0xF };
        } else {
            subsets = { 0xF, 0x7, 0x8, 0x5 };
        }
        for (uint32_t subset : subsets) {
            for (int maskMode = 0; maskMode < 3; ++maskMode) {
                for (float mix : { 0.0f, 0.5f, 1.0f }) {
                    const size_t index = cases.size();
                    FoldCase c;
                    Case& f = c.finish;
                    f.nComps = n;
                    f.channelMask = subset;
                    f.output = output;
                    f.mix = mix;
                    if (index % 5 == 4) {
                        f.src = makeView({}, n, 0, 0, 0.0f);
                    } else {
                        f.src = makeView(index % 2 ? GpuWrapRect { -9, 0, 40, 30 } : GpuWrapRect { 1, 5, 21, 18 }, n,
                                         index % 2 ? 0 : 7, index % 2 ? 0 : 3, 0.0f);
                    }
                    fillSource(f.src, -1, rng);
                    f.dst = makeView({ -7, 1, 35, 25 }, n, 5, 2, kSentinel);
                    if (maskMode > 0) {
                        f.applyMask = true;
                        f.maskInvert = maskMode == 2;
                        f.mask = makeView({ 2, -1, 25, 15 }, 2, 3, 2, 0.0f);
                        f.maskChannel = 1;
                        fillMask(f.mask, rng);
                    }
                    if (index % 7 == 3) {
                        c.base = makeView({}, n, 0, 0, 0.0f);
                    } else {
                        c.base = makeView(inputBounds[index % 3], n, index % 4, uint32_t(index % 3), 0.0f);
                        fillSource(c.base, -1, rng);
                    }
                    for (uint32_t s = 1; s < count; ++s) {
                        const size_t k = index + s;
                        if (k % 7 == 3) {
                            c.inputs.push_back(makeView({}, n, 0, 0, 0.0f));
                            continue;
                        }
                        const uint32_t comps[4] = { n, 4u, 1u, 3u };
                        HostView v = makeView(inputBounds[k % 3], comps[k % 4], 2 * s, s, 0.0f);
                        fillSource(v, -1, rng);
                        c.inputs.push_back(std::move(v));
                    }
                    char label[160];
                    std::snprintf(label, sizeof label, "fold of %u n=%u channels=0x%x mask=%d mix=%g case=%zu", count, n,
                                  subset, maskMode, double(mix), index);
                    f.label = label;
                    cases.push_back(std::move(c));
                }
            }
        }
    }
    return cases;
}

// PassExecutor's channelIndexForBit.
int
channelIndexForBit(int nComps, int bit)
{
    if (nComps == 1) {
        return bit == 3 ? 0 : -1;
    }
    return bit < nComps ? bit : -1;
}

// A view's pixel in the output's channels by channel bit, zero outside the view, as readSourceRow
// reads it.
void
readFoldPixel(const HostView& v, int x, int y, int n, float* out)
{
    for (int ch = 0; ch < 4; ++ch) {
        out[ch] = 0.f;
    }
    if (v.bounds.empty() || !v.inside(x, y)) {
        return;
    }
    for (int ch = 0; ch < n; ++ch) {
        const int i = channelIndexForBit(int(v.nComps), channelBit(n, ch));
        if (i >= 0) {
            out[ch] = v.data[v.index(x, y, i)];
        }
    }
}

// A transcription of runStagesCpu() over a Composite op's fold under the halve-and-add step.
Expected
foldReference(const FoldCase& fc)
{
    const Case& c = fc.finish;
    Expected e;
    e.value = c.dst.data;
    e.divided.assign(e.value.size(), 0);
    e.magnitude.assign(e.value.size(), 0.0);
    const int n = int(c.nComps);
    for (int y = c.output.y1; y < c.output.y2; ++y) {
        for (int x = c.output.x1; x < c.output.x2; ++x) {
            float sourcePix[4];
            readFoldPixel(c.src, x, y, n, sourcePix);
            float result[4];
            readFoldPixel(fc.base, x, y, n, result);
            for (const HostView& input : fc.inputs) {
                float inputPix[4];
                readFoldPixel(input, x, y, n, inputPix);
                for (int ch = 0; ch < n; ++ch) {
                    result[ch] = result[ch] * 0.5f + inputPix[ch];
                }
            }

            float alpha = c.mix;
            if (c.applyMask) {
                const float m = c.mask.inside(x, y) ? c.mask.data[c.mask.index(x, y, int(c.maskChannel))] : 0.f;
                const float maskScale = c.maskInvert ? (1.f - m) : m;
                alpha = maskScale * c.mix;
            }
            for (int ch = 0; ch < n; ++ch) {
                const size_t i = c.dst.index(x, y, ch);
                const float srcV = sourcePix[ch];
                if (!(c.channelMask & (1u << channelBit(n, ch)))) {
                    e.value[i] = srcV;
                    continue;
                }
                float v = result[ch];
                if (alpha == 0.f) {
                    v = srcV;
                } else if (alpha != 1.f) {
                    v = v * alpha + (1.f - alpha) * srcV;
                }
                e.value[i] = v;
            }
        }
    }
    return e;
}

// recordWrapFold()'s dispatches run through the C++ twins: one step per input, alternating between
// two scratch buffers, then wrap_finish.
std::vector<float>
runFoldTwin(const FoldCase& fc)
{
    const Case& c = fc.finish;
    std::vector<float> unused(16, 0.0f);
    const uint32_t n = c.nComps;
    const uint32_t width = uint32_t(c.output.x2 - c.output.x1);
    const uint32_t height = uint32_t(c.output.y2 - c.output.y1);
    HostView scratch[2];
    for (HostView& v : scratch) {
        v = makeView(c.output, n, 0, 0, 0.0f);
    }
    std::vector<float> base = fc.base.data.empty() ? unused : fc.base.data;
    TwinVarying vi { { 0, 0, 0 }, { (width + 7) / 8, (height + 7) / 8, 1 } };
    for (size_t k = 0; k < fc.inputs.size(); ++k) {
        const HostView& resultView = k == 0 ? fc.base : scratch[(k - 1) & 1];
        HostView& dstView = scratch[k & 1];
        GpuWrapStep step;
        step.result = resultView.view(nullptr);
        step.input = fc.inputs[k].view(nullptr);
        step.dst = dstView.view(nullptr);
        step.output = c.output;
        step.nComps = n;
        step.channelMask = c.channelMask;
        GpuWrapParams p;
        const GpuStatus s = wrapStepParams(step, p);
        EXPECT_TRUE(s.ok()) << c.label << ": " << s.message;
        std::vector<float> result = k == 0 ? base : resultView.data;
        std::vector<float> input = fc.inputs[k].data.empty() ? unused : fc.inputs[k].data;
        TwinStepGlobals g { { input.data(), input.size() }, { result.data(), result.size() }, { dstView.data.data(), dstView.data.size() }, { unused.data(), unused.size() }, { unused.data(), unused.size() }, &p };
        compositestep_halveadd(&vi, nullptr, &g);
    }
    HostView& filtered = scratch[(fc.inputs.size() - 1) & 1];
    GpuWrapParams p;
    const GpuWrapView filteredView = filtered.view(nullptr);
    const GpuStatus s = wrapParams(c.wrap(), p, &filteredView);
    EXPECT_TRUE(s.ok()) << c.label << ": " << s.message;
    std::vector<float> dst = c.dst.data;
    std::vector<float> src = c.src.data.empty() ? unused : c.src.data;
    std::vector<float> mask = c.mask.data.empty() ? unused : c.mask.data;
    TwinGlobals g { { src.data(), src.size() }, { dst.data(), dst.size() }, { mask.data(), mask.size() }, { filtered.data.data(), filtered.data.size() }, &p };
    wrap_finish(&vi, nullptr, &g);
    return dst;
}

void
checkFoldTwin(uint32_t count)
{
    Comparison cmp;
    for (const FoldCase& c : makeFoldCases(count)) {
        cmp.check(c.finish, foldReference(c), runFoldTwin(c));
    }
    cmp.print(("fold of " + std::to_string(count) + " cpu twin vs reference").c_str());
    EXPECT_EQ(cmp.failures, 0u);
    EXPECT_GT(cmp.exactCompared, 0u);
}

} // namespace

TEST(PixelWrapLayout, ParamsMatchTheShader)
{
    EXPECT_EQ(pixelwrap_identity_push_bytes, sizeof(GpuWrapParams));
    EXPECT_EQ(pixelwrap_scale_push_bytes, sizeof(GpuWrapParams));
    EXPECT_EQ(pixelwrap_identity_storage_buffers, kGpuWrapBindings);
    EXPECT_EQ(pixelwrap_scale_storage_buffers, kGpuWrapBindings);
    EXPECT_EQ(compositestep_halveadd_push_bytes, sizeof(GpuWrapParams));
    EXPECT_EQ(compositestep_halveadd_storage_buffers, kGpuWrapStepBindings);
}

TEST(PixelWrapLayout, PacksAStep)
{
    HostView result = makeView({ -2, 0, 9, 9 }, 4, 6, 1, 0.0f);
    HostView dst = makeView({ 0, 0, 8, 8 }, 4, 0, 0, 0.0f);
    GpuWrapStep step;
    step.result = result.view(nullptr);
    step.input = makeView({ 1, 2, 5, 6 }, 3, 4, 1, 0.0f).view(nullptr);
    step.dst = dst.view(nullptr);
    step.output = { 0, 0, 8, 8 };
    step.nComps = 4;
    GpuWrapParams p;
    ASSERT_TRUE(wrapStepParams(step, p).ok());
    EXPECT_EQ(p.srcOffset, 6u);
    EXPECT_EQ(p.srcX1, -2);
    EXPECT_EQ(p.maskOffset, 4u);
    EXPECT_EQ(p.maskStride, 15u);
    EXPECT_EQ(p.maskX1, 1);
    EXPECT_EQ(p.maskY2, 6);
    EXPECT_EQ(p.maskComps, 3u);
    EXPECT_EQ(p.flags, 0u);

    GpuWrapStep missing = step;
    missing.input = makeView({}, 3, 0, 0, 0.0f).view(nullptr);
    ASSERT_TRUE(wrapStepParams(missing, p).ok());
    EXPECT_EQ(p.maskComps, 0u);

    GpuWrapStep fiveChannels = step;
    fiveChannels.input.nComps = 5;
    EXPECT_FALSE(wrapStepParams(fiveChannels, p).ok());

    GpuWrapStep otherResult = step;
    otherResult.result.nComps = 3;
    EXPECT_FALSE(wrapStepParams(otherResult, p).ok());

    GpuWrapStep outside = step;
    outside.output = { 0, 0, 9, 8 };
    EXPECT_FALSE(wrapStepParams(outside, p).ok());
}

TEST(PixelWrapLayout, RejectsBadGeometry)
{
    HostView dst = makeView({ 0, 0, 8, 8 }, 4, 0, 0, 0.0f);
    GpuWrap w;
    w.dst = dst.view(nullptr);
    w.output = { 0, 0, 8, 8 };
    w.nComps = 4;
    GpuWrapParams p;
    EXPECT_TRUE(wrapParams(w, p).ok());
    EXPECT_EQ(p.dstOffset, 0u);
    EXPECT_EQ(p.dstStride, 32u);

    GpuWrap outside = w;
    outside.output = { 0, 0, 9, 8 };
    EXPECT_FALSE(wrapParams(outside, p).ok());

    GpuWrap fiveChannels = w;
    fiveChannels.nComps = 5;
    EXPECT_FALSE(wrapParams(fiveChannels, p).ok());

    GpuWrap badSkip = w;
    badSkip.divisorChannel = 3;
    badSkip.skipChannel = 4;
    EXPECT_FALSE(wrapParams(badSkip, p).ok());

    GpuWrap badMask = w;
    badMask.applyMask = true;
    badMask.mask = makeView({ 0, 0, 4, 4 }, 1, 0, 0, 0.0f).view(nullptr);
    badMask.maskChannel = 1;
    EXPECT_FALSE(wrapParams(badMask, p).ok());

    GpuWrap otherSource = w;
    otherSource.src = makeView({ 0, 0, 4, 4 }, 3, 0, 0, 0.0f).view(nullptr);
    EXPECT_FALSE(wrapParams(otherSource, p).ok());
}

TEST(PixelWrapCpu, IdentityMatchesReference)
{
    checkTwin(Kernel::Identity);
}

TEST(PixelWrapCpu, ScaleMatchesReference)
{
    checkTwin(Kernel::Scale);
}

TEST(PixelWrapCpu, DivideMatchesReference)
{
    checkTwin(Kernel::Divide);
}

TEST(PixelWrapCpu, FinishMatchesReference)
{
    checkTwin(Kernel::Finish);
}

TEST(PixelWrapCpu, FoldOfTwoMatchesReference)
{
    checkFoldTwin(2);
}

TEST(PixelWrapCpu, FoldOfFiveMatchesReference)
{
    checkFoldTwin(5);
}

namespace {

class PixelWrapGpuTest : public gputest::SkippableTest {
protected:
    static void SetUpTestCase()
    {
        GpuRuntimeOptions o;
        o.device = gputest::deviceOptions();
        GpuStatus s = GpuRuntime::create(o, rt_);
        if (!s.ok()) {
            rt_.reset();
            error_ = s.message;
            return;
        }
        identity_ = rt_->registerKernel(desc(pixelwrap_identity_spirv, pixelwrap_identity_spirv_words,
                                             pixelwrap_identity_entry, pixelwrap_identity_storage_buffers,
                                             pixelwrap_identity_push_bytes, pixelwrap_identity_group_size));
        scale_ = rt_->registerKernel(desc(pixelwrap_scale_spirv, pixelwrap_scale_spirv_words, pixelwrap_scale_entry,
                                          pixelwrap_scale_storage_buffers, pixelwrap_scale_push_bytes,
                                          pixelwrap_scale_group_size));
        step_ = rt_->registerKernel(desc(compositestep_halveadd_spirv, compositestep_halveadd_spirv_words,
                                         compositestep_halveadd_entry, compositestep_halveadd_storage_buffers,
                                         compositestep_halveadd_push_bytes, compositestep_halveadd_group_size));
    }

    static void TearDownTestCase() { rt_.reset(); }

    void SetUp() override { GPUTEST_DEVICE_OR_SKIP_TEST(rt_ != nullptr, error_); }

    void TearDown() override
    {
        if (rt_) {
            rt_->waitIdle();
        }
        check_.expectClean("the test");
    }

    static GpuKernelDesc desc(const uint32_t* spirv, size_t words, const char* entry, uint32_t buffers,
                              uint32_t pushBytes, const uint32_t (&group)[3])
    {
        GpuKernelDesc d;
        d.spirv = spirv;
        d.spirvWords = words;
        d.entry = entry;
        d.storageBufferCount = buffers;
        d.pushConstantBytes = pushBytes;
        d.groupSize = { group[0], group[1], group[2] };
        return d;
    }

    static bool upload(const HostView& v, GpuBufferHandle& h)
    {
        const VkDeviceSize bytes = v.data.size() * sizeof(float);
        GpuStatus s = rt_->acquire(bytes, h);
        EXPECT_TRUE(s.ok()) << s.message;
        if (!s.ok()) {
            return false;
        }
        TimelineValue done;
        s = rt_->upload({ const_cast<float*>(v.data.data()), bytes }, h.buffer(), 0, done);
        if (s.ok()) {
            s = rt_->wait(done);
        }
        EXPECT_TRUE(s.ok()) << s.message;
        return s.ok();
    }

    // Batched so one record context holds a bounded number of dispatches.
    std::vector<std::vector<float>> runGpu(Kernel kernel, const std::vector<Case>& cases)
    {
        constexpr size_t kBatch = 32;
        std::vector<std::vector<float>> out(cases.size());
        for (size_t first = 0; first < cases.size(); first += kBatch) {
            const size_t count = std::min(kBatch, cases.size() - first);
            std::vector<GpuBufferHandle> src(count), dst(count), mask(count), filtered(count);
            std::unique_ptr<GpuRecordContext> ctx;
            GpuStatus s = rt_->newRecordContext(ctx);
            if (!s.ok()) {
                ADD_FAILURE() << s.message;
                return out;
            }
            for (size_t k = 0; k < count; ++k) {
                const Case& c = cases[first + k];
                if (!upload(c.dst, dst[k]) || (!c.src.bounds.empty() && !upload(c.src, src[k]))
                    || (c.applyMask && !upload(c.mask, mask[k]))
                    || (kernel == Kernel::Finish && !upload(c.filtered, filtered[k]))) {
                    return out;
                }
                GpuWrap w = c.wrap();
                w.dst.buffer = &dst[k].buffer();
                if (src[k]) {
                    w.src.buffer = &src[k].buffer();
                }
                if (mask[k]) {
                    w.mask.buffer = &mask[k].buffer();
                }
                switch (kernel) {
                case Kernel::Identity:
                    s = recordWrapped(*ctx, identity_, w);
                    break;
                case Kernel::Scale:
                    s = recordWrapped(*ctx, scale_, w);
                    break;
                case Kernel::Divide:
                    s = recordWrapDivide(*ctx, w);
                    break;
                case Kernel::Finish:
                    s = recordWrapFinish(*ctx, w, c.filtered.view(&filtered[k].buffer()));
                    break;
                }
                if (!s.ok()) {
                    ADD_FAILURE() << c.label << ": " << s.message;
                    return out;
                }
            }
            s = ctx->submitAndWait();
            ctx.reset();
            if (!s.ok()) {
                ADD_FAILURE() << s.message;
                return out;
            }
            for (size_t k = 0; k < count; ++k) {
                std::vector<float>& o = out[first + k];
                o.assign(cases[first + k].dst.data.size(), 0.0f);
                TimelineValue done;
                s = rt_->download(dst[k].buffer(), 0, { o.data(), o.size() * sizeof(float) }, done);
                if (s.ok()) {
                    s = rt_->wait(done);
                }
                if (!s.ok()) {
                    ADD_FAILURE() << s.message;
                    return out;
                }
            }
        }
        return out;
    }

    void checkGpu(Kernel kernel)
    {
        const std::vector<Case> cases = makeCases(kernel);
        const std::vector<std::vector<float>> got = runGpu(kernel, cases);
        Comparison cmp;
        for (size_t k = 0; k < cases.size(); ++k) {
            cmp.check(cases[k], reference(kernel, cases[k]), got[k]);
        }
        cmp.print((std::string(kernelName(kernel)) + " gpu vs reference").c_str());
        EXPECT_EQ(cmp.failures, 0u);
        EXPECT_GT(cmp.nanCompared, 0u);
    }

    bool download(const GpuBufferHandle& h, std::vector<float>& o)
    {
        TimelineValue done;
        GpuStatus s = rt_->download(h.buffer(), 0, { o.data(), o.size() * sizeof(float) }, done);
        if (s.ok()) {
            s = rt_->wait(done);
        }
        if (!s.ok()) {
            ADD_FAILURE() << s.message;
        }
        return s.ok();
    }

    // Batched so one record context holds a bounded number of dispatches.
    std::vector<std::vector<float>> runFoldGpu(const std::vector<FoldCase>& cases)
    {
        constexpr size_t kBatch = 16;
        std::vector<std::vector<float>> out(cases.size());
        for (size_t first = 0; first < cases.size(); first += kBatch) {
            const size_t count = std::min(kBatch, cases.size() - first);
            std::vector<GpuBufferHandle> src(count), dst(count), mask(count), base(count);
            std::vector<std::vector<GpuBufferHandle>> inputs(count);
            std::unique_ptr<GpuRecordContext> ctx;
            GpuStatus s = rt_->newRecordContext(ctx);
            if (!s.ok()) {
                ADD_FAILURE() << s.message;
                return out;
            }
            for (size_t k = 0; k < count; ++k) {
                const FoldCase& fc = cases[first + k];
                const Case& c = fc.finish;
                if (!upload(c.dst, dst[k]) || (!c.src.bounds.empty() && !upload(c.src, src[k]))
                    || (c.applyMask && !upload(c.mask, mask[k])) || (!fc.base.bounds.empty() && !upload(fc.base, base[k]))) {
                    return out;
                }
                GpuWrap w = c.wrap();
                w.dst.buffer = &dst[k].buffer();
                if (src[k]) {
                    w.src.buffer = &src[k].buffer();
                }
                if (mask[k]) {
                    w.mask.buffer = &mask[k].buffer();
                }
                GpuWrapView baseView = fc.base.view(base[k] ? &base[k].buffer() : nullptr);
                std::vector<GpuWrapView> views = fc.inputViews();
                inputs[k].resize(fc.inputs.size());
                for (size_t i = 0; i < fc.inputs.size(); ++i) {
                    if (fc.inputs[i].bounds.empty()) {
                        continue;
                    }
                    if (!upload(fc.inputs[i], inputs[k][i])) {
                        return out;
                    }
                    views[i].buffer = &inputs[k][i].buffer();
                }
                s = recordWrapFold(*ctx, step_, w, baseView, views);
                if (!s.ok()) {
                    ADD_FAILURE() << c.label << ": " << s.message;
                    return out;
                }
            }
            s = ctx->submitAndWait();
            ctx.reset();
            if (!s.ok()) {
                ADD_FAILURE() << s.message;
                return out;
            }
            for (size_t k = 0; k < count; ++k) {
                std::vector<float>& o = out[first + k];
                o.assign(cases[first + k].finish.dst.data.size(), 0.0f);
                if (!download(dst[k], o)) {
                    return out;
                }
            }
        }
        return out;
    }

    void checkFoldGpu(uint32_t count)
    {
        const std::vector<FoldCase> cases = makeFoldCases(count);
        const std::vector<std::vector<float>> got = runFoldGpu(cases);
        Comparison cmp;
        for (size_t k = 0; k < cases.size(); ++k) {
            cmp.check(cases[k].finish, foldReference(cases[k]), got[k]);
        }
        cmp.print(("fold of " + std::to_string(count) + " gpu vs reference").c_str());
        EXPECT_EQ(cmp.failures, 0u);
        EXPECT_GT(cmp.exactCompared, 0u);
    }

    gputest::ValidationCheck check_;

    static std::unique_ptr<GpuRuntime> rt_;
    static std::string error_;
    static GpuKernelId identity_;
    static GpuKernelId scale_;
    static GpuKernelId step_;
};

std::unique_ptr<GpuRuntime> PixelWrapGpuTest::rt_;
std::string PixelWrapGpuTest::error_;
GpuKernelId PixelWrapGpuTest::identity_ {};
GpuKernelId PixelWrapGpuTest::scale_ {};
GpuKernelId PixelWrapGpuTest::step_ {};

} // namespace

GPU_TEST_F(PixelWrapGpuTest, IdentityMatchesReference)
{
    checkGpu(Kernel::Identity);
}

GPU_TEST_F(PixelWrapGpuTest, ScaleMatchesReference)
{
    checkGpu(Kernel::Scale);
}

GPU_TEST_F(PixelWrapGpuTest, DivideMatchesReference)
{
    checkGpu(Kernel::Divide);
}

GPU_TEST_F(PixelWrapGpuTest, FinishMatchesReference)
{
    checkGpu(Kernel::Finish);
}

GPU_TEST_F(PixelWrapGpuTest, FoldOfTwoMatchesReference)
{
    checkFoldGpu(2);
}

GPU_TEST_F(PixelWrapGpuTest, FoldOfFiveMatchesReference)
{
    checkFoldGpu(5);
}

GPU_TEST_F(PixelWrapGpuTest, RegistryKernelsUseTheWrapLayout)
{
    for (GpuKernelId id : { GpuKernelId::wrap_divide, GpuKernelId::wrap_finish }) {
        const GpuKernelDesc* d = rt_->registry().desc(id);
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(d->pushConstantBytes, sizeof(GpuWrapParams));
        EXPECT_EQ(d->storageBufferCount, kGpuWrapBindings);
    }
}
