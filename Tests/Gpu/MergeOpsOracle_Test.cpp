#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "Engine/Nodes/Merge/MergeOps.h"
#include "MergeOpsGolden.h"
#include "mergeops_oracle_spirv.h"

namespace {

// Mirrors of the Slang C++ prelude's ComputeVaryingInput and StructuredBuffer, as in
// PixelWrapCompose_Test.cpp.
struct TwinVarying {
    uint32_t startGroup[3];
    uint32_t endGroup[3];
};

struct TwinBuffer {
    void* data;
    size_t count;
};

struct TwinGlobals {
    TwinBuffer in;
    TwinBuffer out;
    TwinBuffer ctl;
};

} // namespace

extern "C" void mergeops_oracle(TwinVarying*, void*, void*);

namespace {

using Natron::MergeOps::AlphaSpan;
using Natron::MergeOps::Operator;

// One case of the Slang module: A, a, B and b as float4s in channel-bit order.
struct Case {
    float A[4], a[4], B[4], b[4];
};

std::vector<float>
runSlang(Operator op,
         const std::vector<Case>& cases)
{
    std::vector<float> in(cases.size() * 16);
    for (size_t i = 0; i < cases.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            in[i * 16 + c] = cases[i].A[c];
            in[i * 16 + 4 + c] = cases[i].a[c];
            in[i * 16 + 8 + c] = cases[i].B[c];
            in[i * 16 + 12 + c] = cases[i].b[c];
        }
    }
    std::vector<float> out(cases.size() * 4, -123.f);
    std::vector<uint32_t> ctl { uint32_t(op), uint32_t(cases.size()) };
    TwinGlobals g { { in.data(), in.size() }, { out.data(), out.size() }, { ctl.data(), ctl.size() } };
    TwinVarying vi { { 0, 0, 0 }, { uint32_t((cases.size() + 63) / 64), 1, 1 } };
    mergeops_oracle(&vi, nullptr, &g);

    return out;
}

Operator
op(const char* name)
{
    Operator o = Natron::MergeOps::eOperatorCount;
    EXPECT_TRUE(Natron::MergeOps::operatorFromName(name, &o)) << name;

    return o;
}

// The channel bit of component c of an nComps pixel, as GpuWrap orders them.
int
channelBit(int nComps,
           int c)
{
    return (nComps == 1) ? 3 : c;
}

} // namespace

TEST(MergeOpsOracle, SpirvHasMagic)
{
    EXPECT_EQ(mergeops_oracle_spirv[0], 0x07230203u);
}

TEST(MergeOpsOracle, SeparableGoldens)
{
    for (const mergegolden::SeparableGolden& g : mergegolden::kSeparableGoldens) {
        Case k;
        for (int c = 0; c < 4; ++c) {
            k.A[c] = g.A;
            k.a[c] = g.a;
            k.B[c] = g.B;
            k.b[c] = g.b;
        }
        const std::vector<float> out = runSlang(op(g.op), { k });
        for (int c = 0; c < 4; ++c) {
            EXPECT_TRUE(mergegolden::goldenAccepts(g.expected, out[c]))
                << g.op << " A=" << g.A << " a=" << g.a << " B=" << g.B << " b=" << g.b << " channel " << c
                << ": got " << out[c] << ", want " << g.expected;
        }
    }
}

TEST(MergeOpsOracle, NonSeparableGoldens)
{
    for (const mergegolden::NonSeparableGolden& g : mergegolden::kNonSeparableGoldens) {
        Case k;
        for (int c = 0; c < 4; ++c) {
            k.A[c] = g.A[c];
            k.a[c] = g.a;
            k.B[c] = g.B[c];
            k.b[c] = g.b;
        }
        const std::vector<float> out = runSlang(op(g.op), { k });
        for (int c = 0; c < 4; ++c) {
            EXPECT_TRUE(mergegolden::goldenAccepts(g.expected[c], out[c]))
                << g.op << " a=" << g.a << " b=" << g.b << " channel " << c << ": got " << out[c] << ", want "
                << g.expected[c];
        }
    }
}

// Every operator's CPU row against the Slang module over a random HDR sweep, for each channel count,
// with broadcast and per-channel alphas, within the operator's parity class.
TEST(MergeOpsOracle, CpuRowsMatchSlang)
{
    constexpr int kWidth = 509;
    std::mt19937 rng(101);
    std::uniform_real_distribution<float> value(-1.f, 4.f);
    std::uniform_real_distribution<float> alpha(0.f, 1.25f);
    std::uniform_int_distribution<int> pick(0, 7);
    auto drawAlpha = [&]() {
        const int p = pick(rng);

        return (p == 0) ? 0.f : (p == 1) ? 1.f
                                         : alpha(rng);
    };

    for (int i = 0; i < Natron::MergeOps::eOperatorCount; ++i) {
        const Operator o = Operator(i);
        const char* name = Natron::MergeOps::operatorName(o);
        const double tolerance = mergegolden::operatorTolerance(name);
        for (int nComps = 1; nComps <= 4; ++nComps) {
            for (bool perChannel : { false, true }) {
                const size_t n = size_t(kWidth) * nComps;
                std::vector<float> A(n), B(n), aLayer(n), bLayer(n), aPixel(kWidth), bPixel(kWidth);
                for (size_t j = 0; j < n; ++j) {
                    A[j] = value(rng);
                    B[j] = value(rng);
                    aLayer[j] = drawAlpha();
                    bLayer[j] = drawAlpha();
                }
                for (int x = 0; x < kWidth; ++x) {
                    aPixel[x] = drawAlpha();
                    bPixel[x] = drawAlpha();
                }
                const AlphaSpan a = perChannel ? AlphaSpan { aLayer.data(), nComps, 1 } : AlphaSpan { aPixel.data(), 1, 0 };
                const AlphaSpan b = perChannel ? AlphaSpan { bLayer.data(), nComps, 1 } : AlphaSpan { bPixel.data(), 1, 0 };
                std::vector<float> cpu(n, -123.f);
                Natron::MergeOps::mergeRow(o, nComps, A.data(), a, B.data(), b, kWidth, cpu.data());

                std::vector<Case> cases(kWidth);
                for (int x = 0; x < kWidth; ++x) {
                    Case& k = cases[x];
                    for (int c = 0; c < 4; ++c) {
                        k.A[c] = k.B[c] = 0.f;
                        k.a[c] = k.b[c] = 0.f;
                    }
                    for (int c = 0; c < nComps; ++c) {
                        const int bit = channelBit(nComps, c);
                        k.A[bit] = A[size_t(x) * nComps + c];
                        k.B[bit] = B[size_t(x) * nComps + c];
                        k.a[bit] = a.data[x * a.pixelStride + c * a.channelStride];
                        k.b[bit] = b.data[x * b.pixelStride + c * b.channelStride];
                    }
                }
                const std::vector<float> slang = runSlang(o, cases);

                int failures = 0;
                for (int x = 0; x < kWidth && failures < 5; ++x) {
                    for (int c = 0; c < nComps; ++c) {
                        const float ref = slang[size_t(x) * 4 + channelBit(nComps, c)];
                        const float got = cpu[size_t(x) * nComps + c];
                        if (!mergegolden::parityAccepts(tolerance, ref, got)) {
                            ADD_FAILURE() << name << " nComps=" << nComps << (perChannel ? " per-channel" : " broadcast")
                                          << " x=" << x << " c=" << c << ": cpu " << got << ", slang " << ref;
                            ++failures;
                        }
                    }
                }
            }
        }
    }
}
