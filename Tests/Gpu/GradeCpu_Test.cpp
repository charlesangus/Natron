#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "grade_spirv.h"

#include "slang-cpp-prelude.h"

#include "GradeFixture.h"

extern "C" void grade_main(ComputeVaryingInput*, void* entryPointParams, void* globalParams);

namespace {

using gradefix::GradeParams;

struct GradeGlobals {
    StructuredBuffer<float> srcBuf;
    RWStructuredBuffer<float> dstBuf;
    GradeParams* params;
};

} // namespace

TEST(GradeCpu, SpirvHasMagic)
{
    ASSERT_GT(grade_spirv_words, 5u);
    EXPECT_EQ(grade_spirv[0], 0x07230203u);
}

TEST(GradeCpu, MatchesReferenceOverSweep)
{
    std::vector<gradefix::GradeCase> cases = gradefix::makeCases();
    gradefix::GradeStats stats;

    for (size_t k = 0; k < cases.size(); ++k) {
        gradefix::GradeCase& c = cases[k];
        GradeParams p = c.p;
        std::vector<float> dst(c.src.size(), -123.0f);

        GradeGlobals g { { c.src.data(), c.src.size() }, { dst.data(), dst.size() }, &p };
        ComputeVaryingInput vi = {};
        vi.startGroupID = { 0, 0, 0 };
        vi.endGroupID = { (p.width + 7) / 8, (p.height + 7) / 8, 1 };
        grade_main(&vi, nullptr, &g);

        gradefix::checkCase(stats, k, c, dst.data());
    }

    gradefix::printStats("grade cpu twin vs reference", stats);
    for (const std::string& f : stats.failures)
        ADD_FAILURE() << f;

    EXPECT_GT(stats.err.compared, 1000000u);
    EXPECT_GT(stats.err.nonFinite, 0u);
    EXPECT_EQ(stats.violations, 0u);
}
