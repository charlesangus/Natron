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

#include <atomic>
#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuPipelineCache.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuShaderCompiler.h"
#include "Engine/GpuContext.h"
#endif

#define kStagedGpuShaderTestOpPluginID "test.natron.built-in.StagedGpuShaderTestOp"

NATRON_NAMESPACE_ENTER

namespace {

const double kTime = 1.;

// A gain of 2 is exact, so a fused multiply-add on the device rounds as the CPU's two operations do.
const float kGain = 2.f;
const float kOffset = 0.25f;

// Mirrors the std140 block `Params` of the shaders below.
struct ShaderParams {
    float gain;
    float offset;
};

std::mutex gShaderMutex;
// Empty keeps the node on its registered-kernel path, which names no kernel.
std::string gShaderSource;

void
setShaderSource(const std::string& source)
{
    std::lock_guard<std::mutex> lock(gShaderMutex);
    gShaderSource = source;
}

// The tag makes the source unique, so the process-wide compile memo and earlier runs' disk entries
// serve no other test.
std::string
shaderSource(const std::string& tag,
             bool broken)
{
    return "#version 460\n// " + tag + "\n"
                                       "layout(local_size_x = 8, local_size_y = 8) in;\n"
                                       "layout(std430, set = 0, binding = 1) readonly buffer Src { float src[]; };\n"
                                       "layout(std430, set = 0, binding = 2) writeonly buffer Dst { float dst[]; };\n"
                                       "layout(std140, set = 0, binding = 0) uniform Params { float gain; float offset; };\n"
                                       "layout(push_constant) uniform Region { int x1; int y1; uint width; uint height; uint nComps; uint channelMask; } region;\n"
                                       "void main() {\n"
                                       "    if (gl_GlobalInvocationID.x >= region.width || gl_GlobalInvocationID.y >= region.height) return;\n"
                                       "    uint base = (gl_GlobalInvocationID.y * region.width + gl_GlobalInvocationID.x) * region.nComps;\n"
                                       "    for (uint c = 0u; c < region.nComps; ++c) {\n"
        + std::string(broken ? "        dst[base + c] = src[base + c] * gain + ;\n" : "        dst[base + c] = src[base + c] * gain + offset;\n")
        + "    }\n"
          "}\n";
}

constexpr InputSpec kStagedInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief The same gain and offset as a Point op of a StagedImageEffect, rendered on the GPU by the
 * runtime shader in gShaderSource.
 **/
struct GainOffsetPointOp {
    struct Params {
        float gain;
        float offset;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedGpuShaderTestOpPluginID,
        .label = "Test Staged GPU Shader Op",
        .grouping = "Other/Test",
        .inputs = kStagedInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params { kGain, kOffset };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    void row(const Params& params,
             const RowIO& io) const
    {
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;

        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                    dst[c] = src[c] * params.gain + params.offset;
                }
            }
        }
    }

    GpuOpRef gpu(const Params& params) const
    {
        GpuRuntimeShader shader;
        {
            std::lock_guard<std::mutex> lock(gShaderMutex);
            if (gShaderSource.empty()) {
                return GpuOpRef::none();
            }
            shader.source = gShaderSource;
        }
        ShaderParams block;
        block.gain = params.gain;
        block.offset = params.offset;
        GpuShaderInputs inputs;
        inputs.addUniform("Params", block);

        return GpuOpRef::runtime(shader, inputs);
    }
};

static_assert(PointOp<GainOffsetPointOp>);

typedef ImageNode<GainOffsetPointOp> StagedGpuShaderTestOp;

void
registerTestPlugin()
{
    EffectInstancePtr node(StagedGpuShaderTestOp::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&StagedGpuShaderTestOp::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is
    // made from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerGpuShaderTestOpsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerTestPlugin();
    });
}

GpuQuery
rgbaQuery()
{
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

std::string
uniqueTag(const char* what)
{
    static std::atomic<int> counter(0);

    return std::string(what) + " " + std::to_string((long long)QCoreApplication::applicationPid()) + " " + std::to_string(counter.fetch_add(1));
}

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class NativeRuntimeShaderGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerGpuShaderTestOpsOnce();
        BaseTest::SetUp();
    }

    virtual void TearDown() OVERRIDE
    {
        setShaderSource(std::string());
        BaseTest::TearDown();
    }

    // A parity source and the op reading it, with the source's RGBA pixels over its window.
    bool createGraph(NodePtr* source,
                     NodePtr* op,
                     RectI* window,
                     std::vector<float>* sourcePixels)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        EXPECT_TRUE(bool(*source));
        *op = createNode(QString::fromUtf8(kStagedGpuShaderTestOpPluginID));
        EXPECT_TRUE(bool(*op));
        if (!*source || !*op) {
            return false;
        }
        setParitySourceComponents(*source, "rgba");
        connectNodes(*source, *op, 0, true);
        *window = paritySourceWindow(*source, kTime, 0);

        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderNodePlanesDirect(*source, kTime, ViewIdx(0), 0, *window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1)) {
            return false;
        }
        *sourcePixels = planes[0].pixels;

        return true;
    }

    static std::vector<float> expectedFrom(const std::vector<float>& source)
    {
        std::vector<float> out(source.size());

        for (std::size_t i = 0; i < source.size(); ++i) {
            out[i] = source[i] * kGain + kOffset;
        }

        return out;
    }

    static int gpuTasksOf(const EndToEndResult& result,
                          const NodePtr& node)
    {
        std::map<std::string, int>::const_iterator it = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

        return (it == result.gpuTasksPerNode.end()) ? 0 : it->second;
    }

    static QString persistentMessageOf(const NodePtr& node,
                                       int* type)
    {
        QString message;

        *type = -1;
        node->getPersistentMessage(&message, type, false);

        return message;
    }

    static void expectSame(const std::vector<float>& want,
                           const std::vector<float>& got,
                           const char* what)
    {
        const ImageDiff diff = compareImages(want, got, ParityTolerance::exact());

        EXPECT_EQ(0u, diff.mismatches) << what << ": float " << diff.firstMismatchIndex << " of " << diff.compared;
    }
};

#ifdef NATRON_HAVE_GPU

// The Point op of a StagedImageEffect reaches the GPU through the same executor: a shader that
// compiles renders on the device, built once for every thread that asks, and matches the CPU frame.
TEST_F(NativeRuntimeShaderGpuTest, StagedPointOpRendersTheShaderOnTheGpuAndMatchesTheCpu)
{
    NodePtr source;
    NodePtr op;
    RectI window;
    std::vector<float> sourcePixels;
    ASSERT_TRUE(createGraph(&source, &op, &window, &sourcePixels));
    ASSERT_TRUE(dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get()) != NULL);
    const std::vector<float> expected = expectedFrom(sourcePixels);

    setShaderSource(shaderSource(uniqueTag("staged valid"), false));

    std::size_t cacheBuilds = 0;
    const GpuContextReady askFromThreads = [op, &cacheBuilds](std::string* error) {
        GpuContext* context = appPTR->getGpuContext();
        gpu::GpuRuntime* runtime = context ? context->runtime() : nullptr;
        StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
        if (!runtime || !effect) {
            *error = "no runtime or no effect";

            return false;
        }
        std::atomic<int> refused(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([effect, &refused]() {
                if (!effect->canRenderOnGpu(rgbaQuery())) {
                    ++refused;
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        EXPECT_EQ(0, refused.load());
        const gpu::GpuPipelineCache::Stats stats = runtime->pipelines().stats();
        cacheBuilds = stats.compiles + stats.diskHits;

        return true;
    };
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, op), op, window, kTime, askFromThreads);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1u, cacheBuilds);
    EXPECT_TRUE(result.ranInRegion(op));
    EXPECT_GT(gpuTasksOf(result, op), 0);
    expectSame(expected, result.cpu, "CPU frame");
    expectSame(result.cpu, result.gpu, "GPU frame");

    int type = -1;
    EXPECT_TRUE(persistentMessageOf(op, &type).isEmpty()) << persistentMessageOf(op, &type).toStdString();
}

// A staged node's broken shader stays on the CPU and is reported, and built, only once.
TEST_F(NativeRuntimeShaderGpuTest, StagedPointOpWithABrokenShaderRendersOnTheCpuAndIsReportedOnce)
{
    NodePtr source;
    NodePtr op;
    RectI window;
    std::vector<float> sourcePixels;
    ASSERT_TRUE(createGraph(&source, &op, &window, &sourcePixels));
    const std::vector<float> expected = expectedFrom(sourcePixels);

    setShaderSource(shaderSource(uniqueTag("staged broken"), true));
    const std::size_t compilesBefore = gpu::glslCompileCountForTesting();

    const EndToEndResult first = renderEndToEnd(std::vector<NodePtr>(1, op), op, window, kTime);
    if (first.skipped) {
        return;
    }
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_FALSE(first.ranInRegion(op));
    EXPECT_EQ(0, gpuTasksOf(first, op));
    expectSame(expected, first.gpu, "frame with GPU placement");
    EXPECT_EQ(1u, gpu::glslCompileCountForTesting() - compilesBefore);

    int type = -1;
    const QString message = persistentMessageOf(op, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("CPU"))) << message.toStdString();
    op->clearPersistentMessage(false);

    std::size_t cacheBuilds = 1;
    const GpuContextReady askFromThreads = [op, &cacheBuilds](std::string* error) {
        GpuContext* context = appPTR->getGpuContext();
        gpu::GpuRuntime* runtime = context ? context->runtime() : nullptr;
        StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
        if (!runtime || !effect) {
            *error = "no runtime or no effect";

            return false;
        }
        std::atomic<int> accepted(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([effect, &accepted]() {
                for (int i = 0; i < 16; ++i) {
                    if (effect->canRenderOnGpu(rgbaQuery())) {
                        ++accepted;
                    }
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        EXPECT_EQ(0, accepted.load());
        const gpu::GpuPipelineCache::Stats stats = runtime->pipelines().stats();
        cacheBuilds = stats.compiles + stats.diskHits;

        return true;
    };
    const EndToEndResult second = renderEndToEnd(std::vector<NodePtr>(1, op), op, window, kTime, askFromThreads);
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(0u, cacheBuilds);
    EXPECT_EQ(0, gpuTasksOf(second, op));
    expectSame(expected, second.gpu, "second frame with GPU placement");
    EXPECT_EQ(1u, gpu::glslCompileCountForTesting() - compilesBefore);
    EXPECT_TRUE(persistentMessageOf(op, &type).isEmpty()) << "reported again: " << persistentMessageOf(op, &type).toStdString();
}

#endif // NATRON_HAVE_GPU
