/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewerInstance.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GlInterop.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuPipelineCache.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/ViewerGpuProcess.h"
#endif

NATRON_NAMESPACE_USING

namespace {

namespace OCIO = OCIO_NAMESPACE;
namespace fs = std::filesystem;

const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kWorkingSpace = "ACEScg";
const char* const kDisplay = "sRGB - Display";
const char* const kSDRView = "ACES 2.0 - SDR 100 nits (Rec.709)";

// The input's pixels, and the texture rectangle that runs 8 columns past their right edge and 4
// rows past their top.
const RectI kBounds(-5, 3, 35, 27);
const RectI kRect(-5, 3, 43, 31);
const int kPadFloats = 7;
const int kRowPadFloats = 3;
const float kPadValue = 1e6f;

#ifdef NATRON_HAVE_GPU

const DisplayChannelsEnum kChannels[] = {
    eDisplayChannelsRGB,
    eDisplayChannelsR,
    eDisplayChannelsG,
    eDisplayChannelsB,
    eDisplayChannelsA,
    eDisplayChannelsY,
    eDisplayChannelsMatte,
};

const char*
channelsName(DisplayChannelsEnum channels)
{
    switch (channels) {
    case eDisplayChannelsRGB:
        return "RGB";
    case eDisplayChannelsR:
        return "R";
    case eDisplayChannelsG:
        return "G";
    case eDisplayChannelsB:
        return "B";
    case eDisplayChannelsA:
        return "A";
    case eDisplayChannelsY:
        return "Y";
    case eDisplayChannelsMatte:
        return "Matte";
    }

    return "?";
}

float
inputValue(int x,
           int y,
           int c)
{
    const int row = y - kBounds.y1;
    const float t = float(x - kBounds.x1) / float(kBounds.width() - 1);
    const float phase = t + 0.17f * float(c) + 0.043f * float(row);
    float v = -0.3f + 2.6f * (phase - std::floor(phase));
    if (row % 7 == 3) {
        v *= 6.f;
    }

    return v;
}

typedef float (*InputFn)(int x, int y, int c);

// Values either side of gamma 0's threshold at 1.0, the closest a float gets to it included.
float
nearOneValue(int x,
             int y,
             int c)
{
    static const float values[] = {
        1.f,
        std::nextafter(1.f, 0.f),
        0.999995f,
        0.99999f,
        0.9999f,
        std::nextafter(1.f, 2.f),
        1.00001f,
        0.5f,
        0.f,
    };
    const int n = int(sizeof(values) / sizeof(values[0]));

    return values[((x - kBounds.x1) + 2 * (y - kBounds.y1) + c) % n];
}

const ImageLayerDesc&
layerFor(int nComps)
{
    switch (nComps) {
    case 1:
        return ImageLayerDesc::getAlphaComponents();
    case 2:
        return ImageLayerDesc::getXYComponents();
    case 3:
        return ImageLayerDesc::getRGBComponents();
    default:
        return ImageLayerDesc::getRGBAComponents();
    }
}

ImagePtr
makeImage(int nComps,
          InputFn value = inputValue)
{
    const RectD rod(kBounds.x1, kBounds.y1, kBounds.x2, kBounds.y2);
    ImagePtr image = std::make_shared<Image>(layerFor(nComps), rod, kBounds, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
    Image::WriteAccess w = image->getWriteRights();
    for (int y = kBounds.y1; y < kBounds.y2; ++y) {
        float* row = (float*)w.pixelAt(kBounds.x1, y);
        for (int x = kBounds.x1; x < kBounds.x2; ++x) {
            for (int c = 0; c < nComps; ++c) {
                row[(x - kBounds.x1) * nComps + c] = value(x, y, c);
            }
        }
    }

    return image;
}

// The same pixels as makeImage(), laid out with an offset and padded rows so that the kernel must
// use the view's layout.
std::vector<float>
packedInput(int nComps,
            InputFn value)
{
    const int stride = kBounds.width() * nComps + kRowPadFloats;
    std::vector<float> packed(std::size_t(kPadFloats) + std::size_t(stride) * kBounds.height() + 16, kPadValue);
    for (int y = kBounds.y1; y < kBounds.y2; ++y) {
        for (int x = kBounds.x1; x < kBounds.x2; ++x) {
            for (int c = 0; c < nComps; ++c) {
                packed[kPadFloats + (y - kBounds.y1) * stride + (x - kBounds.x1) * nComps + c] = value(x, y, c);
            }
        }
    }

    return packed;
}

struct Case {
    DisplayChannelsEnum channels = eDisplayChannelsRGB;
    int alphaChannelIndex = -1;
    bool matte = false;
    double gain = 1.;
    double offset = 0.;
    double gamma = 1.;
    ProjectColorManagement::DisplayProcessorPtr processor;
};

Case
makeCase(DisplayChannelsEnum channels,
         int nComps)
{
    Case c;
    c.channels = channels;
    if (channels == eDisplayChannelsMatte) {
        c.matte = true;
        c.alphaChannelIndex = (nComps == 4) ? 3 : 0;
    } else if (channels == eDisplayChannelsA) {
        c.alphaChannelIndex = (nComps == 4) ? -1 : 0;
    }

    return c;
}

std::string
describe(const char* processorName,
         int nComps,
         const Case& c)
{
    std::ostringstream s;
    s << processorName << " nComps " << nComps << " " << channelsName(c.channels) << " gain " << c.gain << " offset " << c.offset
      << " gamma " << c.gamma;

    return s.str();
}

// The CPU path in the pieces it can render: the rows of a rect whose bottom-left corner is outside
// the image read as no pixel, but a rect that starts inside must stay inside.
template <typename T>
std::vector<T>
cpuReference(const ImagePtr& image,
             const Case& c,
             ImageBitDepthEnum depth,
             int elementsPerPixel)
{
    std::vector<T> full(std::size_t(kRect.area()) * elementsPerPixel, T(0));
    const RectI pieces[] = {
        kBounds,
        RectI(kBounds.x2, kRect.y1, kRect.x2, kBounds.y2),
        RectI(kRect.x1, kBounds.y2, kRect.x2, kRect.y2),
    };
    for (const RectI& piece : pieces) {
        std::vector<T> part(std::size_t(piece.area()) * elementsPerPixel, T(0));
        viewerProcessRectOnCpu(image, c.matte ? ImageConstPtr(image) : ImageConstPtr(), c.channels, depth, c.gain, c.gamma, c.offset,
                               c.processor, c.alphaChannelIndex, piece, part.data());
        for (int y = piece.y1; y < piece.y2; ++y) {
            std::copy_n(part.begin() + std::size_t((y - piece.y1) * piece.width()) * elementsPerPixel,
                        std::size_t(piece.width()) * elementsPerPixel,
                        full.begin() + (std::size_t(y - kRect.y1) * kRect.width() + (piece.x1 - kRect.x1)) * elementsPerPixel);
        }
    }

    return full;
}

// Records ViewerGpuProcess::record() into a command buffer of its own, as GlInterop::produce()
// would, and waits for it.
class Dispatcher {
public:
    explicit Dispatcher(gpu::GpuRuntime& runtime)
        : _runtime(runtime)
    {
        gpu::GpuDevice& dev = runtime.device();
        VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.queueFamilyIndex = dev.queueFamily(gpu::QueueKind::Compute);
        EXPECT_EQ(VK_SUCCESS, vkCreateCommandPool(dev.device(), &pci, nullptr, &_pool));
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        EXPECT_EQ(VK_SUCCESS, vkCreateFence(dev.device(), &fi, nullptr, &_fence));
        const gpu::GpuStatus s = gpu::GpuBuffer::createStaging(dev, VkDeviceSize(kRect.area()) * 16, gpu::kGpuBufferDefaultUsage, _out);
        EXPECT_TRUE(bool(s)) << s.message;
    }

    ~Dispatcher()
    {
        _runtime.waitIdle();
        _out.reset();
        _in.reset();
        if (_fence) {
            vkDestroyFence(_runtime.device().device(), _fence, nullptr);
        }
        if (_pool) {
            vkDestroyCommandPool(_runtime.device().device(), _pool, nullptr);
        }
    }

    bool ready() const { return _pool && _fence && _out; }

    void setInput(int nComps,
                  InputFn value = inputValue)
    {
        _in.reset();
        const std::vector<float> packed = packedInput(nComps, value);
        const gpu::GpuStatus s = gpu::GpuBuffer::createStaging(_runtime.device(), packed.size() * sizeof(float), gpu::kGpuBufferDefaultUsage, _in);
        ASSERT_TRUE(bool(s)) << s.message;
        std::memcpy(_in.mapped(), packed.data(), packed.size() * sizeof(float));
        _view = GpuImageView();
        _view.buffer = &_in;
        _view.byteOffset = kPadFloats * sizeof(float);
        _view.bounds = kBounds;
        _view.rowStrideFloats = kBounds.width() * nComps + kRowPadFloats;
        _view.nComps = nComps;
    }

    gpu::GpuStatus run(ViewerGpuProcess& process,
                       gpu::GlPixelFormat format,
                       const Case& c,
                       std::vector<std::byte>* result)
    {
        gpu::GpuDevice& dev = _runtime.device();
        const gpu::GlFrame frame { (uint32_t)kRect.width(), (uint32_t)kRect.height(), format };
        std::memset(_out.mapped(), 0xA5, std::size_t(frame.bytes()));

        EXPECT_EQ(VK_SUCCESS, vkResetCommandPool(dev.device(), _pool, 0));
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = _pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (_cmd == VK_NULL_HANDLE) {
            EXPECT_EQ(VK_SUCCESS, vkAllocateCommandBuffers(dev.device(), &ai, &_cmd));
        }
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        EXPECT_EQ(VK_SUCCESS, vkBeginCommandBuffer(_cmd, &bi));

        ViewerGpuProcessParams params;
        params.rect = kRect;
        params.channels = c.channels;
        params.alphaChannelIndex = c.alphaChannelIndex;
        params.matteOverlay = c.matte;
        params.gain = c.gain;
        params.offset = c.offset;
        params.gamma = c.gamma;
        params.displayProcessor = c.processor;
        const gpu::GpuStatus recorded = process.record(_cmd, _view, _out.buffer(), frame, params);

        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(_cmd, &dep);
        EXPECT_EQ(VK_SUCCESS, vkEndCommandBuffer(_cmd));
        if (!recorded) {
            return recorded;
        }

        VkCommandBufferSubmitInfo ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        ci.commandBuffer = _cmd;
        VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &ci;
        gpu::GpuStatus s = dev.submit(gpu::QueueKind::Compute, std::span<const VkSubmitInfo2>(&submit, 1), _fence);
        if (!s) {
            return s;
        }
        EXPECT_EQ(VK_SUCCESS, vkWaitForFences(dev.device(), 1, &_fence, VK_TRUE, 30'000'000'000ull));
        EXPECT_EQ(VK_SUCCESS, vkResetFences(dev.device(), 1, &_fence));
        process.releaseDescriptors();

        result->resize(std::size_t(frame.bytes()));
        std::memcpy(result->data(), _out.mapped(), result->size());

        return s;
    }

private:
    gpu::GpuRuntime& _runtime;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;
    gpu::GpuBuffer _in;
    gpu::GpuBuffer _out;
    GpuImageView _view;
};

struct EightBitStats {
    std::size_t codes = 0;
    std::size_t offByOne = 0;
    std::size_t thresholdBand = 0;
    int worst = 0;
};

// Under OCIO the kernel thresholds gamma 0 at this value rather than the CPU's 1.0, because OCIO's
// GPU renderer lands clamped white a few ulps under 1.0. A channel the CPU transform lands in
// between is black on the CPU and may be white on the GPU.
const float kGpuOcioThreshold = 0.99999f;

// The float path computes luminance in double where the 8-bit path uses float, so its value may sit
// a few ulps either side of the one the 8-bit path thresholds.
bool
inThresholdBand(float v)
{
    return (v >= kGpuOcioThreshold) && (v <= 1.f + (1.f - kGpuOcioThreshold));
}

// Each channel's value after the CPU display transform and before the gamma 0 threshold, laid out
// as RGBA over kRect.
std::vector<float>
cpuDisplayValues(const ImagePtr& image,
                 const Case& c)
{
    Case plain = c;
    plain.matte = false;
    if (plain.channels == eDisplayChannelsMatte) {
        plain.channels = eDisplayChannelsRGB;
    }
    std::vector<float> values = cpuReference<float>(image, plain, eImageBitDepthFloat, 4);
    applyViewerDisplayTransform(*c.processor, values.data(), int(kRect.area()), c.gain, c.offset, 1., eViewerDisplayOutputExact);

    return values;
}

void
compareEightBit(const std::vector<std::byte>& gpuBytes,
                const std::vector<U32>& cpu,
                const std::vector<float>* thresholdInputs,
                const std::string& label,
                EightBitStats* stats)
{
    ASSERT_EQ(cpu.size() * sizeof(U32), gpuBytes.size());
    std::vector<U32> gpu(cpu.size());
    std::memcpy(gpu.data(), gpuBytes.data(), gpuBytes.size());

    std::size_t mismatches = 0;
    std::string first;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const int shifts[4] = { 24, 16, 8, 0 };
        for (int k = 0; k < 4; ++k) {
            const int expected = int((cpu[i] >> shifts[k]) & 0xffu);
            const int got = int((gpu[i] >> shifts[k]) & 0xffu);
            const int diff = std::abs(expected - got);
            const int tolerance = (k == 0) ? 0 : 1;
            if ((k > 0) && thresholdInputs && inThresholdBand((*thresholdInputs)[i * 4 + std::size_t(k - 1)])) {
                ++stats->thresholdBand;
                continue;
            }
            if (k > 0) {
                ++stats->codes;
                stats->offByOne += (diff == 1) ? 1 : 0;
                stats->worst = std::max(stats->worst, diff);
            }
            if (diff > tolerance) {
                if (mismatches == 0) {
                    const int x = kRect.x1 + int(i % std::size_t(kRect.width()));
                    const int y = kRect.y1 + int(i / std::size_t(kRect.width()));
                    std::ostringstream s;
                    s << "pixel (" << x << ", " << y << ") byte " << "ARGB"[k] << ": cpu " << expected << ", gpu " << got;
                    if (thresholdInputs && (k > 0)) {
                        s << std::setprecision(9) << " (cpu display value " << (*thresholdInputs)[i * 4 + std::size_t(k - 1)] << ")";
                    }
                    first = s.str();
                }
                ++mismatches;
            }
        }
    }
    EXPECT_EQ(0u, mismatches) << label << ": first " << first;
}

void
compareFloat(const std::vector<std::byte>& gpuBytes,
             const std::vector<float>& cpu,
             const std::string& label)
{
    ASSERT_EQ(cpu.size() * sizeof(float), gpuBytes.size());
    std::vector<float> gpu(cpu.size());
    std::memcpy(gpu.data(), gpuBytes.data(), gpuBytes.size());
    std::size_t mismatches = 0;
    std::string first;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const double tolerance = 1e-6 * std::max(1., std::fabs(double(cpu[i])));
        if (!(std::fabs(double(gpu[i]) - double(cpu[i])) <= tolerance)) {
            if (mismatches == 0) {
                std::ostringstream s;
                s << "float " << i << " (pixel " << i / 4 << " channel " << i % 4 << "): cpu " << cpu[i] << ", gpu " << gpu[i];
                first = s.str();
            }
            ++mismatches;
        }
    }
    EXPECT_EQ(0u, mismatches) << label << ": first " << first;
}

void
writeCube(const fs::path& path,
          int dimensions,
          int size)
{
    std::ofstream out(path);
    out << std::setprecision(9);
    out << "LUT_" << dimensions << "D_SIZE " << size << "\n";
    const float last = float(size - 1);
    if (dimensions == 1) {
        for (int i = 0; i < size; ++i) {
            const float x = float(i) / last;
            out << std::pow(x, 1.f / 2.2f) << " " << std::pow(x, 1.f / 2.0f) << " " << std::pow(x, 1.f / 2.4f) << "\n";
        }

        return;
    }
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const float fr = float(r) / last;
                const float fg = float(g) / last;
                const float fb = float(b) / last;
                out << 0.85f * fr + 0.1f * fg * fb + 0.02f << " " << 0.8f * fg + 0.15f * fr * fr + 0.05f * fb << " "
                    << 0.9f * fb + 0.1f * fr * fg << "\n";
            }
        }
    }
}

// The source space is scene-referred, not data: OCIO skips the whole display transform for a data
// source, which would leave no LUT to bind.
OCIO::ConstProcessorRcPtr
makeLutProcessor(const fs::path& dir,
                 std::string* error)
{
    try {
        writeCube(dir / "shaper.cube", 1, 256);
        writeCube(dir / "grade.cube", 3, 33);
        OCIO::ConfigRcPtr config = OCIO::Config::CreateRaw()->createEditableCopy();
        config->setSearchPath(dir.string().c_str());
        OCIO::ColorSpaceRcPtr source = OCIO::ColorSpace::Create();
        source->setName("scenesrc");
        source->setIsData(false);
        config->addColorSpace(source);
        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        OCIO::FileTransformRcPtr shaper = OCIO::FileTransform::Create();
        shaper->setSrc("shaper.cube");
        shaper->setInterpolation(OCIO::INTERP_LINEAR);
        group->appendTransform(shaper);
        OCIO::FileTransformRcPtr grade = OCIO::FileTransform::Create();
        grade->setSrc("grade.cube");
        grade->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
        group->appendTransform(grade);
        OCIO::ColorSpaceRcPtr space = OCIO::ColorSpace::Create();
        space->setName("lutspace");
        space->setIsData(false);
        space->setTransform(group, OCIO::COLORSPACE_DIR_FROM_REFERENCE);
        config->addColorSpace(space);
        config->addDisplayView("LutDisplay", "LutView", "lutspace", "");
        config->validate();
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc("scenesrc");
        transform->setDisplay("LutDisplay");
        transform->setView("LutView");

        return config->getProcessor(transform);
    } catch (const std::exception& e) {
        *error = e.what();

        return OCIO::ConstProcessorRcPtr();
    }
}

// The viewer's 8-bit LUT approximation is itself up to a code value away from the exact
// transform, which would leave the dither no room in a one-code tolerance; the GPU runs the exact
// transform, so it is compared with the exact CPU transform.
ProjectColorManagement::DisplayProcessorPtr
exactProcessor(const OCIO::ConstProcessorRcPtr& processor)
{
    auto p = std::make_shared<ProjectColorManagement::DisplayProcessor>();
    p->processor = processor;
    p->cpu = processor->getDefaultCPUProcessor();
    p->cacheID = processor->getCacheID();
    p->eightBitLutAllowed = false;

    return p;
}

// OCIO optimises the identity matrix away, which leaves a processor that is a no-op.
OCIO::ConstProcessorRcPtr
makeIdentityProcessor()
{
    return OCIO::Config::CreateRaw()->getProcessor(OCIO::MatrixTransform::Create());
}

struct NamedProcessor {
    const char* name;
    ProjectColorManagement::DisplayProcessorPtr processor;
};

#endif // NATRON_HAVE_GPU

} // namespace

TEST(ViewerGpuProcess, EightBitMatchesTheCpuInEveryDisplayMode)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    gpu::GpuRuntime& runtime = holder->runtime();

    ProjectColorManagement colorManagement;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, colorManagement.load(kStudioURI, std::string(), &error)) << error;
    colorManagement.setWorkingSpace(kWorkingSpace);
    ProjectColorManagement::DisplayProcessorPtr studio = colorManagement.getDisplayProcessor(kWorkingSpace, kDisplay, kSDRView, std::string(), &error);
    ASSERT_TRUE(studio && studio->processor) << error;

    const fs::path dir = fs::temp_directory_path() / ("natron-viewer-gpu-luts-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    OCIO::ConstProcessorRcPtr lut = makeLutProcessor(dir, &error);
    fs::remove_all(dir, ec);
    ASSERT_TRUE(bool(lut)) << error;
    const OCIO::ConstProcessorRcPtr identity = makeIdentityProcessor();
    ASSERT_TRUE(identity && identity->isNoOp());

    const NamedProcessor processors[] = {
        { "no-ocio", ProjectColorManagement::DisplayProcessorPtr() },
        { "identity", exactProcessor(identity) },
        { "studio-sdr", exactProcessor(studio->processor) },
        { "luts", exactProcessor(lut) },
    };
    const int processorCount = int(sizeof(processors) / sizeof(processors[0]));

    ViewerGpuProcess process(runtime);
    for (const NamedProcessor& p : processors) {
        const gpu::GpuStatus s = process.prepare(p.processor, gpu::GlPixelFormat::Bgra8);
        ASSERT_TRUE(bool(s)) << p.name << ": " << s.message;
    }

    Dispatcher dispatcher(runtime);
    ASSERT_TRUE(dispatcher.ready());
    const double grades[][2] = { { 1., 0. }, { 0.5, -0.2 }, { 0.5, 0.2 }, { 4., -0.2 }, { 4., 0.2 } };
    const double gammas[] = { 1., 2.2, 0. };
    EightBitStats stats[processorCount];
    for (int nComps = 1; nComps <= 4; ++nComps) {
        const ImagePtr image = makeImage(nComps);
        dispatcher.setInput(nComps);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
        for (int pi = 0; pi < processorCount; ++pi) {
            for (DisplayChannelsEnum channels : kChannels) {
                for (const auto& grade : grades) {
                    for (double gamma : gammas) {
                        Case c = makeCase(channels, nComps);
                        c.gain = grade[0];
                        c.offset = grade[1];
                        c.gamma = gamma;
                        c.processor = processors[pi].processor;
                        const std::string label = describe(processors[pi].name, nComps, c);
                        std::vector<std::byte> got;
                        const gpu::GpuStatus s = dispatcher.run(process, gpu::GlPixelFormat::Bgra8, c, &got);
                        ASSERT_TRUE(bool(s)) << label << ": " << s.message;
                        std::vector<float> thresholdInputs;
                        if ((gamma <= 0.) && c.processor && !c.processor->processor->isNoOp()) {
                            thresholdInputs = cpuDisplayValues(image, c);
                        }
                        compareEightBit(got, cpuReference<U32>(image, c, eImageBitDepthByte, 1), thresholdInputs.empty() ? nullptr : &thresholdInputs,
                                        label, &stats[pi]);
                        if (::testing::Test::HasFatalFailure()) {
                            return;
                        }
                    }
                }
            }
        }
    }
    for (int pi = 0; pi < processorCount; ++pi) {
        std::printf("[viewer-gpu] %-10s %zu codes, %zu one code off, worst %d, %zu in the gamma 0 threshold band\n", processors[pi].name,
                    stats[pi].codes, stats[pi].offByOne, stats[pi].worst, stats[pi].thresholdBand);
    }
#endif
}

TEST(ViewerGpuProcess, GammaZeroThresholdsAtOneWithoutATransform)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    gpu::GpuRuntime& runtime = holder->runtime();

    const OCIO::ConstProcessorRcPtr identity = makeIdentityProcessor();
    ASSERT_TRUE(identity && identity->isNoOp());
    const NamedProcessor processors[] = {
        { "no-ocio", ProjectColorManagement::DisplayProcessorPtr() },
        { "identity", exactProcessor(identity) },
    };

    ViewerGpuProcess process(runtime);
    for (const NamedProcessor& p : processors) {
        const gpu::GpuStatus s = process.prepare(p.processor, gpu::GlPixelFormat::Bgra8);
        ASSERT_TRUE(bool(s)) << p.name << ": " << s.message;
    }

    Dispatcher dispatcher(runtime);
    ASSERT_TRUE(dispatcher.ready());
    for (int nComps = 1; nComps <= 4; ++nComps) {
        const ImagePtr image = makeImage(nComps, nearOneValue);
        dispatcher.setInput(nComps, nearOneValue);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
        for (const NamedProcessor& p : processors) {
            for (DisplayChannelsEnum channels : kChannels) {
                Case c = makeCase(channels, nComps);
                c.gamma = 0.;
                c.processor = p.processor;
                const std::string label = describe(p.name, nComps, c);
                std::vector<std::byte> got;
                const gpu::GpuStatus s = dispatcher.run(process, gpu::GlPixelFormat::Bgra8, c, &got);
                ASSERT_TRUE(bool(s)) << label << ": " << s.message;
                const std::vector<U32> cpu = cpuReference<U32>(image, c, eImageBitDepthByte, 1);
                // The red of the first pixels of the bottom row holds 1, then the floats just under it.
                if ((nComps == 4) && (channels == eDisplayChannelsRGB)) {
                    EXPECT_EQ(0xffu, (cpu[0] >> 16) & 0xffu) << label;
                    EXPECT_EQ(0u, (cpu[1] >> 16) & 0xffu) << label;
                    EXPECT_EQ(0u, (cpu[2] >> 16) & 0xffu) << label;
                    EXPECT_EQ(0u, (cpu[3] >> 16) & 0xffu) << label;
                }
                EightBitStats stats;
                compareEightBit(got, cpu, nullptr, label, &stats);
                if (::testing::Test::HasFatalFailure()) {
                    return;
                }
            }
        }
    }
#endif
}

TEST(ViewerGpuProcess, FloatMatchesTheCpuInEveryDisplayMode)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    gpu::GpuRuntime& runtime = holder->runtime();
    ViewerGpuProcess process(runtime);
    const gpu::GpuStatus prepared = process.prepare(ProjectColorManagement::DisplayProcessorPtr(), gpu::GlPixelFormat::Rgba32F);
    ASSERT_TRUE(bool(prepared)) << prepared.message;

    Dispatcher dispatcher(runtime);
    ASSERT_TRUE(dispatcher.ready());
    for (int nComps = 1; nComps <= 4; ++nComps) {
        const ImagePtr image = makeImage(nComps);
        dispatcher.setInput(nComps);
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
        for (DisplayChannelsEnum channels : kChannels) {
            // Float frames leave the grade and gamma to the GL shader, on both paths.
            Case c = makeCase(channels, nComps);
            c.gain = 4.;
            c.offset = 0.2;
            c.gamma = 2.2;
            const std::string label = describe("float", nComps, c);
            std::vector<std::byte> got;
            const gpu::GpuStatus s = dispatcher.run(process, gpu::GlPixelFormat::Rgba32F, c, &got);
            ASSERT_TRUE(bool(s)) << label << ": " << s.message;
            compareFloat(got, cpuReference<float>(image, c, eImageBitDepthFloat, 4), label);
            if (::testing::Test::HasFatalFailure()) {
                return;
            }
        }
    }
#endif
}

TEST(ViewerGpuProcess, FailuresReturnAStatus)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    gpu::GpuRuntime& runtime = holder->runtime();

    std::shared_ptr<gpu::GpuKernel> kernel;
    const gpu::GpuStatus broken = ViewerGpuProcess::compile(runtime, gpu::GlPixelFormat::Bgra8, "vec4 OCIODisplay(vec4 c) { return c * ; }\n", kernel);
    EXPECT_FALSE(bool(broken));
    EXPECT_FALSE(broken.message.empty());
    EXPECT_EQ(nullptr, kernel.get());

    ViewerGpuProcess process(runtime);
    ASSERT_TRUE(bool(process.prepare(ProjectColorManagement::DisplayProcessorPtr(), gpu::GlPixelFormat::Bgra8)));
    const gpu::GpuPipelineCache::Stats before = runtime.pipelines().stats();
    ASSERT_TRUE(bool(process.prepare(ProjectColorManagement::DisplayProcessorPtr(), gpu::GlPixelFormat::Bgra8)));
    const gpu::GpuPipelineCache::Stats after = runtime.pipelines().stats();
    EXPECT_EQ(before.compiles, after.compiles);
    EXPECT_EQ(before.memoryHits, after.memoryHits);

    gpu::GpuBuffer buffer;
    ASSERT_TRUE(bool(gpu::GpuBuffer::createStaging(runtime.device(), 4096, gpu::kGpuBufferDefaultUsage, buffer)));
    GpuImageView view;
    view.buffer = &buffer;
    view.bounds = RectI(0, 0, 4, 4);
    view.rowStrideFloats = 16;
    view.nComps = 4;
    ViewerGpuProcessParams params;
    params.rect = RectI(0, 0, 4, 4);
    const gpu::GlFrame wrongSize { 5, 4, gpu::GlPixelFormat::Bgra8 };
    EXPECT_FALSE(bool(process.record(VK_NULL_HANDLE, view, buffer.buffer(), wrongSize, params)));
    view.nComps = 5;
    const gpu::GlFrame frame { 4, 4, gpu::GlPixelFormat::Bgra8 };
    EXPECT_FALSE(bool(process.record(VK_NULL_HANDLE, view, buffer.buffer(), frame, params)));
#endif
}
