#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "GpuShaderCompiler.h"

using namespace gpu;

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203;

// The comment makes each test's source unique so the memoised results of one test never satisfy another.
std::string
computeSource(const std::string& tag, const std::string& body = "")
{
    return "#version 460\n// " + tag + "\n"
                                       "layout(local_size_x = 8, local_size_y = 8) in;\n"
                                       "layout(std430, set = 0, binding = 0) buffer Out { float data[]; } outBuf;\n"
                                       "void main() {\n"
                                       "    outBuf.data[gl_GlobalInvocationID.x] = 1.0;\n"
        + body + "}\n";
}

} // namespace

TEST(GpuShaderCompilerTest, CompilesValidComputeShader)
{
    auto r = compileGlsl(computeSource("valid"), ShaderStage::Compute, "main");
    ASSERT_TRUE(r->ok) << r->log;
    ASSERT_FALSE(r->spirv.empty());
    EXPECT_EQ(r->spirv[0], kSpirvMagic);
}

TEST(GpuShaderCompilerTest, RenamesEntryPoint)
{
    auto r = compileGlsl(computeSource("entry"), ShaderStage::Compute, "my_entry");
    ASSERT_TRUE(r->ok) << r->log;
    const std::string bytes(reinterpret_cast<const char*>(r->spirv.data()), r->spirv.size() * sizeof(uint32_t));
    EXPECT_NE(bytes.find("my_entry"), std::string::npos);
}

TEST(GpuShaderCompilerTest, ReportsErrorLine)
{
    // The bad statement is on line 7 of the source.
    auto r = compileGlsl(computeSource("invalid", "    float x = undeclared_symbol;\n"), ShaderStage::Compute);
    EXPECT_FALSE(r->ok);
    EXPECT_TRUE(r->spirv.empty());
    EXPECT_NE(r->log.find("ERROR: 0:7"), std::string::npos) << r->log;
    EXPECT_NE(r->log.find("undeclared_symbol"), std::string::npos) << r->log;
    EXPECT_NE(r->logHead(1).find("ERROR"), std::string::npos);
}

TEST(GpuShaderCompilerTest, ConcurrentCallersShareOneCompile)
{
    const std::string src = computeSource("concurrent");
    const size_t before = glslCompileCountForTesting();

    constexpr int kThreads = 8;
    std::atomic<bool> go { false };
    std::vector<std::shared_ptr<const ShaderCompileResult>> results(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            results[i] = compileGlsl(src, ShaderStage::Compute);
        });
    }
    go = true;
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(glslCompileCountForTesting() - before, 1u);
    for (int i = 0; i < kThreads; ++i) {
        ASSERT_TRUE(results[i]->ok) << results[i]->log;
        EXPECT_EQ(results[i]->spirv, results[0]->spirv);
    }
}

TEST(GpuShaderCompilerTest, MemoisedResultsAreBounded)
{
    const std::string repeated = computeSource("bounded-repeat");
    auto first = compileGlsl(repeated, ShaderStage::Compute);
    ASSERT_TRUE(first->ok) << first->log;
    EXPECT_EQ(first, compileGlsl(repeated, ShaderStage::Compute));

    const size_t before = glslCompileCountForTesting();
    constexpr int kDistinct = 400;
    for (int i = 0; i < kDistinct; ++i) {
        compileGlsl(computeSource("bounded-" + std::to_string(i)), ShaderStage::Compute);
    }
    EXPECT_EQ(glslCompileCountForTesting() - before, size_t(kDistinct));
    EXPECT_LT(glslCompileCacheSizeForTesting(), size_t(kDistinct));

    // The oldest entry was evicted, so asking again compiles again rather than serving a retained result.
    const size_t beforeAgain = glslCompileCountForTesting();
    auto again = compileGlsl(repeated, ShaderStage::Compute);
    ASSERT_TRUE(again->ok);
    EXPECT_EQ(glslCompileCountForTesting() - beforeAgain, 1u);
    EXPECT_EQ(again->spirv, first->spirv);
}
