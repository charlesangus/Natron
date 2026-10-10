#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "GpuHarness.h"
#include "GradeFixture.h"
#include "grade_spirv.h"

using namespace gpu;

namespace {

using gradefix::GradeCase;
using gradefix::GradeParams;

constexpr gradefix::Bounds kGpuBounds { gradefix::kUlpBound, true };

class GradeGpuTest : public gputest::GpuSuite {
protected:
    // Runs every case through the kernel and returns each case's output.
    static std::vector<std::vector<float>> run(const std::vector<GradeCase>& cases)
    {
        std::vector<std::vector<float>> dst;
        for (const GradeCase& c : cases)
            dst.emplace_back(c.src.size(), -123.0f);
        auto kernel = makeKernel(grade_spirv, grade_spirv_words, "main", 2, sizeof(GradeParams),
                                 grade_group_size);
        auto transfer = makeTransfer();
        if (!kernel || !transfer)
            return dst;

        std::vector<TransferFrame> frames;
        for (size_t i = 0; i < cases.size(); ++i)
            frames.push_back({ cases[i].src.data(), dst[i].data(), cases[i].src.size() * sizeof(float) });

        process(*transfer, *kernel, frames, 256,
                [&](VkCommandBuffer cmd, const ComputeBinding& b, size_t k) {
                    const GradeParams& p = cases[k].p;
                    const VkBuffer bufs[] = { b.input, b.output };
                    return kernel->record(cmd, bufs, std::as_bytes(std::span(&p, 1)), { p.width, p.height, 1 });
                });
        return dst;
    }
};

} // namespace

TEST_F(GradeGpuTest, MatchesReferenceOverSweep)
{
    const std::vector<GradeCase> cases = gradefix::makeCases();
    const std::vector<std::vector<float>> dst = run(cases);

    gradefix::GradeStats stats;
    for (size_t k = 0; k < cases.size(); ++k)
        gradefix::checkCase(stats, k, cases[k], dst[k].data(), kGpuBounds);

    std::printf("grade gpu (%s) vs reference\n", dev_->info().name.c_str());
    gradefix::printStats("  all cases", stats);
    if (stats.err.compared > 0) {
        const GradeCase& w = cases[stats.err.worst.group];
        std::printf("  worst case: reverse=%d nComps=%u flags=%u channelMask=%u input=%.9g\n", w.reverse,
                    w.p.nComps, w.p.flags, w.p.channelMask, (double)w.src[stats.err.worst.index]);
    }

    for (const std::string& f : stats.failures)
        ADD_FAILURE() << f;
    EXPECT_GT(stats.err.compared, 1000000u);
    EXPECT_GT(stats.err.nonFinite, 0u);
    EXPECT_GT(stats.subnormalInputs, 0u);
    EXPECT_EQ(stats.violations, 0u);
}

// pow/exp quality is the driver difference that matters for gamma, so isolate
// it: forward grade with gamma != 1 and no clamps, input in the display range.
TEST_F(GradeGpuTest, GammaPrecision)
{
    std::vector<GradeCase> cases;
    for (float gamma : { 0.45f, 1.0f / 2.2f, 1.8f, 2.2f, 2.4f, 4.0f }) {
        for (bool reverse : { false, true }) {
            GradeCase c {};
            c.p.width = 4097;
            c.p.height = 1;
            c.p.nComps = 1;
            c.p.channelMask = 0xF;
            c.p.flags = reverse ? gradefix::kReverse : 0;
            c.reverse = reverse;
            for (int i = 0; i < 4; ++i) {
                c.p.a[i] = 1.0f;
                c.p.b[i] = 0.0f;
                c.p.gamma[i] = gamma;
                c.p.invGamma[i] = static_cast<float>(1.0 / double(gamma));
            }
            for (uint32_t i = 0; i < c.p.width; ++i)
                c.src.push_back(float(i + 1) / float(c.p.width));
            cases.push_back(std::move(c));
        }
    }
    const std::vector<std::vector<float>> dst = run(cases);

    for (size_t k = 0; k < cases.size(); ++k) {
        gradefix::GradeStats stats;
        gradefix::checkCase(stats, k, cases[k], dst[k].data(), kGpuBounds);
        char label[64];
        std::snprintf(label, sizeof label, "  gamma=%.4f %s", cases[k].p.gamma[3],
                      cases[k].reverse ? "reverse" : "forward");
        gradefix::printStats(label, stats);
        for (const std::string& f : stats.failures)
            ADD_FAILURE() << label << ": " << f;
        EXPECT_EQ(stats.violations, 0u) << label;
    }
}
