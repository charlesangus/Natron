#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "GpuBuffer.h"
#include "GpuPipelineCache.h"
#include "GpuTestDevice.h"
#include "GpuTexture.h"

using namespace gpu;

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kWidth = 37;
constexpr uint32_t kHeight = 19;
constexpr float kTolerance = 1e-5f;

// Mirrors the std140 block `Params` of the shader below: vec3 shares a 16-byte slot with the float
// that follows it, and the block is padded to a multiple of 16.
struct Params {
    float gain[4];
    float tint[3];
    float strength;
    float offset;
    float pad[3];
};
static_assert(sizeof(Params) == 48);
static_assert(offsetof(Params, tint) == 16);
static_assert(offsetof(Params, strength) == 28);
static_assert(offsetof(Params, offset) == 32);

struct PushConstants {
    uint32_t width;
    uint32_t height;
};

struct Reference {
    std::vector<float> values;
    std::vector<float> bound;
};

struct Lut {
    uint32_t dimensions = 3;
    std::array<uint32_t, 3> size { 1, 1, 1 };
    std::vector<float> texels;
};

// Analytic and not separable, so a swapped axis or channel changes the result.
Lut
makeLut(uint32_t dimensions, std::array<uint32_t, 3> size)
{
    Lut lut;
    lut.dimensions = dimensions;
    lut.size = size;
    lut.texels.reserve(size_t(size[0]) * size[1] * size[2] * 4);
    for (uint32_t z = 0; z < size[2]; ++z) {
        for (uint32_t y = 0; y < size[1]; ++y) {
            for (uint32_t x = 0; x < size[0]; ++x) {
                const float r = size[0] > 1 ? float(x) / float(size[0] - 1) : 0.f;
                const float g = size[1] > 1 ? float(y) / float(size[1] - 1) : 0.f;
                const float b = size[2] > 1 ? float(z) / float(size[2] - 1) : 0.f;
                lut.texels.push_back(r * g + 0.1f * b + 0.05f * r);
                lut.texels.push_back(0.5f * g * g + 0.3f * r + 0.1f);
                lut.texels.push_back(0.8f * b * b + 0.2f * r * g + 0.15f * g);
                lut.texels.push_back(0.25f + 0.5f * r);
            }
        }
    }
    return lut;
}

std::string
shaderSource(const Lut& lut, const std::string& tag)
{
    static const char* const samplerType[] = { "", "sampler1D", "sampler2D", "sampler3D" };
    static const char* const swizzle[] = { "", "r", "rg", "rgb" };
    const std::string size = "vec3(" + std::to_string(lut.size[0]) + ".0, " + std::to_string(lut.size[1]) + ".0, "
        + std::to_string(lut.size[2]) + ".0)";
    // The comment makes each test's source unique so neither the process-wide compile memo nor an
    // earlier run's disk entries satisfy another test.
    return "#version 460\n// " + tag + "\n"
                                       "layout(local_size_x = 8, local_size_y = 8) in;\n"
                                       "layout(std430, set = 0, binding = 0) readonly buffer InBuf { vec4 px[]; } inBuf;\n"
                                       "layout(std430, set = 0, binding = 1) writeonly buffer OutBuf { vec4 px[]; } outBuf;\n"
                                       "layout(std140, set = 0, binding = 2) uniform Params {\n"
                                       "    vec4 gain;\n"
                                       "    vec3 tint;\n"
                                       "    float strength;\n"
                                       "    float offset;\n"
                                       "};\n"
                                       "layout(set = 0, binding = 3) uniform "
        + std::string(samplerType[lut.dimensions])
        + " lut;\n"
          "layout(push_constant) uniform Push { uint width; uint height; } pc;\n"
          "const vec3 lutSize = "
        + size
        + ";\n"
          "void main() {\n"
          "    if (gl_GlobalInvocationID.x >= pc.width || gl_GlobalInvocationID.y >= pc.height) return;\n"
          "    uint i = gl_GlobalInvocationID.y * pc.width + gl_GlobalInvocationID.x;\n"
          "    vec4 p = inBuf.px[i];\n"
          "    vec3 c = clamp(p.rgb * gain.rgb + vec3(offset), 0.0, 1.0);\n"
          "    vec3 coord = (c * (lutSize - 1.0) + 0.5) / lutSize;\n"
          "    vec3 l = texture(lut, coord."
        + swizzle[lut.dimensions]
        + ").rgb;\n"
          "    outBuf.px[i] = vec4(mix(c, l, strength) * tint, p.a * gain.a);\n"
          "}\n";
}

class GpuRuntimeShaderTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
        dir = (fs::temp_directory_path() / ("natron-gpu-runtime-shader-test-" + std::to_string(::getpid()) + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name()))
                  .string();
        fs::remove_all(dir);
        GpuPipelineCacheOptions o;
        o.directory = dir;
        cache = std::make_unique<GpuPipelineCache>(*dev, o);
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(dev->physicalDevice(), &props);
        subTexelPrecisionBits = props.limits.subTexelPrecisionBits;
    }

    void TearDown() override
    {
        if (dev) {
            dev->waitIdle();
            if (pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(dev->device(), pool, nullptr);
            }
        }
        kernel.reset();
        cache.reset();
        dev.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
        check.expectClean("the test");
    }

    // The sample values are multiples of 1/256, which keeps the fp32 reference exact. A device may
    // round linear-filter weights to subTexelPrecisionBits fractional bits, and that rounding sets
    // the per-sample bound in reference().
    std::vector<float> makeInput(float scale) const
    {
        std::vector<float> in(size_t(kWidth) * kHeight * 4);
        for (size_t i = 0; i < in.size(); ++i) {
            uint32_t x = uint32_t(i) * 2654435761u;
            x ^= x >> 15;
            in[i] = (float(x % 200u) + scale) / 256.f;
        }
        return in;
    }

    bool run(const Lut& lut, GpuTextureFilter filter, const std::string& tag, const std::vector<float>& input,
             const Params& params, std::vector<float>& out)
    {
        GpuTextureDesc td;
        td.dimensions = lut.dimensions;
        td.width = lut.size[0];
        td.height = lut.size[1];
        td.depth = lut.size[2];
        td.channels = 4;
        td.filter = filter;
        std::unique_ptr<GpuTexture> tex;
        GpuStatus s = GpuTexture::create(*dev, td, lut.texels, tex);
        EXPECT_TRUE(s.ok()) << s.message;
        if (!s.ok()) {
            return false;
        }

        s = cache->get(shaderSource(lut, tag), ShaderStage::Compute, "main", kernel);
        EXPECT_TRUE(s.ok()) << s.message;
        if (!s.ok()) {
            return false;
        }
        const GpuKernelBinding* block = kernel->findBinding("Params");
        EXPECT_NE(block, nullptr);
        if (!block) {
            return false;
        }
        EXPECT_EQ(block->type, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        EXPECT_GE(block->blockBytes, offsetof(Params, offset) + sizeof(float));
        EXPECT_LE(block->blockBytes, sizeof(Params));
        const GpuKernelBinding* sampler = kernel->findBinding("lut");
        EXPECT_NE(sampler, nullptr);
        if (!sampler) {
            return false;
        }
        EXPECT_EQ(sampler->type, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        EXPECT_EQ(sampler->viewType, tex->viewType());
        EXPECT_EQ(kernel->desc().pushConstantBytes, sizeof(PushConstants));

        const VkDeviceSize bytes = input.size() * sizeof(float);
        GpuBuffer in, outBuf;
        s = GpuBuffer::createStaging(*dev, bytes, kGpuBufferDefaultUsage, in);
        EXPECT_TRUE(s.ok()) << s.message;
        s = GpuBuffer::createStaging(*dev, bytes, kGpuBufferDefaultUsage, outBuf);
        EXPECT_TRUE(s.ok()) << s.message;
        if (!in || !outBuf) {
            return false;
        }
        std::memcpy(in.mapped(), input.data(), bytes);
        std::memset(outBuf.mapped(), 0xFF, bytes);

        GpuUniformBuffer ubo;
        s = GpuUniformBuffer::create(*dev, std::as_bytes(std::span(&params, 1)), ubo);
        EXPECT_TRUE(s.ok()) << s.message;
        if (!ubo) {
            return false;
        }

        if (pool == VK_NULL_HANDLE) {
            VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.queueFamilyIndex = dev->queueFamily(QueueKind::Compute);
            EXPECT_EQ(vkCreateCommandPool(dev->device(), &pci, nullptr, &pool), VK_SUCCESS);
        }
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        EXPECT_EQ(vkAllocateCommandBuffers(dev->device(), &ai, &cmd), VK_SUCCESS);
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        EXPECT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS);

        const GpuBinding bindings[] = {
            GpuBinding::storage(0, in),
            GpuBinding::storage(1, outBuf),
            GpuBinding::uniform(block->binding, ubo),
            GpuBinding::texture(sampler->binding, *tex),
        };
        const PushConstants push { kWidth, kHeight };
        s = kernel->recordBindings(cmd, bindings, std::as_bytes(std::span(&push, 1)),
                                   kernel->groupCount({ kWidth, kHeight, 1 }));
        EXPECT_TRUE(s.ok()) << s.message;

        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        EXPECT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS);

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        EXPECT_EQ(vkCreateFence(dev->device(), &fi, nullptr, &fence), VK_SUCCESS);
        VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        ci.commandBuffer = cmd;
        VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &ci;
        s = dev->submit(QueueKind::Compute, std::span<const VkSubmitInfo2>(&submit, 1), fence);
        EXPECT_TRUE(s.ok()) << s.message;
        if (s.ok()) {
            EXPECT_EQ(vkWaitForFences(dev->device(), 1, &fence, VK_TRUE, 30'000'000'000ull), VK_SUCCESS);
        }
        vkDestroyFence(dev->device(), fence, nullptr);
        kernel->releaseDescriptors();

        out.resize(input.size());
        std::memcpy(out.data(), outBuf.mapped(), bytes);
        return s.ok();
    }

    // The texel-centre convention of the shader: coord = (v * (N - 1) + 0.5) / N per axis, clamped to edge.
    static void sampleLut(const Lut& lut, GpuTextureFilter filter, const float coord[3], float out[3],
                          float sensitivity[3])
    {
        float pos[3];
        int32_t lo[3], hi[3];
        float frac[3];
        for (uint32_t a = 0; a < 3; ++a) {
            const float n = float(lut.size[a]);
            pos[a] = std::clamp(coord[a] * n - 0.5f, 0.f, n - 1.f);
            if (filter == GpuTextureFilter::Nearest) {
                lo[a] = hi[a] = int32_t(std::floor(pos[a] + 0.5f));
                frac[a] = 0.f;
            } else {
                lo[a] = int32_t(std::floor(pos[a]));
                hi[a] = std::min(lo[a] + 1, int32_t(lut.size[a]) - 1);
                frac[a] = pos[a] - float(lo[a]);
            }
        }
        auto texel = [&](int32_t x, int32_t y, int32_t z, uint32_t ch) {
            return lut.texels[((size_t(z) * lut.size[1] + y) * lut.size[0] + x) * 4 + ch];
        };
        for (uint32_t ch = 0; ch < 3; ++ch) {
            float v = 0.f;
            for (int corner = 0; corner < 8; ++corner) {
                const int cx = corner & 1, cy = (corner >> 1) & 1, cz = (corner >> 2) & 1;
                const float w = (cx ? frac[0] : 1.f - frac[0]) * (cy ? frac[1] : 1.f - frac[1])
                    * (cz ? frac[2] : 1.f - frac[2]);
                v += w * texel(cx ? hi[0] : lo[0], cy ? hi[1] : lo[1], cz ? hi[2] : lo[2], ch);
            }
            out[ch] = v;
            // A weight error of e on one axis moves the result by e times the value change across
            // that axis's cell, which differs per corner of the other two axes.
            float sum = 0.f;
            for (uint32_t a = 0; a < 3; ++a) {
                float widest = 0.f;
                for (int corner = 0; corner < 4; ++corner) {
                    int32_t lo3[3] = { lo[0], lo[1], lo[2] };
                    int32_t hi3[3] = { lo[0], lo[1], lo[2] };
                    uint32_t bit = 0;
                    for (uint32_t b = 0; b < 3; ++b) {
                        if (b == a) {
                            hi3[b] = hi[b];
                        } else {
                            const int32_t at = ((corner >> bit++) & 1) ? hi[b] : lo[b];
                            lo3[b] = hi3[b] = at;
                        }
                    }
                    widest = std::max(widest,
                                      std::fabs(texel(hi3[0], hi3[1], hi3[2], ch) - texel(lo3[0], lo3[1], lo3[2], ch)));
                }
                sum += widest;
            }
            sensitivity[ch] = sum;
        }
    }

    // Axes a LUT does not have have size 1 and so always sample texel 0.
    Reference reference(const Lut& lut, GpuTextureFilter filter, const std::vector<float>& input,
                        const Params& p) const
    {
        Reference ref;
        ref.values.resize(input.size());
        ref.bound.assign(input.size(), kTolerance);
        const float weightError = std::ldexp(1.f, -int(subTexelPrecisionBits));
        for (size_t i = 0; i < input.size() / 4; ++i) {
            float c[3], coord[3], l[3], sensitivity[3];
            for (int ch = 0; ch < 3; ++ch) {
                c[ch] = std::clamp(input[i * 4 + ch] * p.gain[ch] + p.offset, 0.f, 1.f);
                coord[ch] = (c[ch] * float(lut.size[ch] - 1) + 0.5f) / float(lut.size[ch]);
            }
            sampleLut(lut, filter, coord, l, sensitivity);
            for (int ch = 0; ch < 3; ++ch) {
                ref.values[i * 4 + ch] = (c[ch] + (l[ch] - c[ch]) * p.strength) * p.tint[ch];
                if (filter == GpuTextureFilter::Linear) {
                    ref.bound[i * 4 + ch] += std::fabs(p.strength * p.tint[ch]) * weightError * sensitivity[ch];
                }
            }
            ref.values[i * 4 + 3] = input[i * 4 + 3] * p.gain[3];
        }
        return ref;
    }

    void expectMatches(const std::vector<float>& got, const Reference& ref)
    {
        ASSERT_EQ(got.size(), ref.values.size());
        float worstRatio = 0.f;
        size_t worstAt = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            const float ratio = std::fabs(got[i] - ref.values[i]) / ref.bound[i];
            if (!(ratio <= worstRatio)) {
                worstRatio = ratio;
                worstAt = i;
            }
        }
        EXPECT_LE(worstRatio, 1.f) << "worst at element " << worstAt << ": got " << got[worstAt] << ", expected "
                                   << ref.values[worstAt] << ", bound " << ref.bound[worstAt]
                                   << " (error/bound " << worstRatio << ", subTexelPrecisionBits "
                                   << subTexelPrecisionBits << ")";
    }

    uint32_t subTexelPrecisionBits = 0;
    std::unique_ptr<GpuDevice> dev;
    gputest::ValidationCheck check;
    std::string dir;
    std::unique_ptr<GpuPipelineCache> cache;
    std::shared_ptr<GpuKernel> kernel;
    VkCommandPool pool = VK_NULL_HANDLE;
};

Params
linearParams()
{
    Params p {};
    p.gain[0] = 0.5f;
    p.gain[1] = 0.75f;
    p.gain[2] = 1.5f;
    p.gain[3] = 0.5f;
    p.tint[0] = 1.1f;
    p.tint[1] = 0.9f;
    p.tint[2] = 1.3f;
    p.strength = 0.75f;
    p.offset = 0.125f;
    return p;
}

} // namespace

GPU_TEST_F(GpuRuntimeShaderTest, Lut3DLinear)
{
    if (!GpuTexture::linearFilterSupported(*dev, 4)) {
        GPUTEST_SKIP() << "device cannot filter RGBA32F linearly";
    }
    const Lut lut = makeLut(3, { 17, 17, 17 });
    const Params p = linearParams();
    const std::vector<float> in = makeInput(0.f);
    std::vector<float> out;
    ASSERT_TRUE(run(lut, GpuTextureFilter::Linear, "lut3d-linear", in, p, out));
    expectMatches(out, reference(lut, GpuTextureFilter::Linear, in, p));
}

GPU_TEST_F(GpuRuntimeShaderTest, Lut3DNearest)
{
    const Lut lut = makeLut(3, { 17, 17, 17 });
    Params p {};
    p.gain[0] = p.gain[1] = p.gain[2] = p.gain[3] = 1.f;
    p.tint[0] = 1.f;
    p.tint[1] = 0.5f;
    p.tint[2] = 2.f;
    p.strength = 1.f;
    // Half-step inputs never land on the rounding tie between two texels.
    const std::vector<float> in = makeInput(0.5f);
    std::vector<float> out;
    ASSERT_TRUE(run(lut, GpuTextureFilter::Nearest, "lut3d-nearest", in, p, out));
    expectMatches(out, reference(lut, GpuTextureFilter::Nearest, in, p));
}

GPU_TEST_F(GpuRuntimeShaderTest, Lut2DLinear)
{
    if (!GpuTexture::linearFilterSupported(*dev, 4)) {
        GPUTEST_SKIP() << "device cannot filter RGBA32F linearly";
    }
    const Lut lut = makeLut(2, { 17, 9, 1 });
    const Params p = linearParams();
    const std::vector<float> in = makeInput(0.f);
    std::vector<float> out;
    ASSERT_TRUE(run(lut, GpuTextureFilter::Linear, "lut2d-linear", in, p, out));
    expectMatches(out, reference(lut, GpuTextureFilter::Linear, in, p));
}

GPU_TEST_F(GpuRuntimeShaderTest, Lut1DLinear)
{
    if (!GpuTexture::linearFilterSupported(*dev, 4)) {
        GPUTEST_SKIP() << "device cannot filter RGBA32F linearly";
    }
    const Lut lut = makeLut(1, { 33, 1, 1 });
    const Params p = linearParams();
    const std::vector<float> in = makeInput(0.f);
    std::vector<float> out;
    ASSERT_TRUE(run(lut, GpuTextureFilter::Linear, "lut1d-linear", in, p, out));
    expectMatches(out, reference(lut, GpuTextureFilter::Linear, in, p));
}
