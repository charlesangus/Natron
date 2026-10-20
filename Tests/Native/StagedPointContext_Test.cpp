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
#include <mutex>
#include <string>
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
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kStagedScaledPointOpPluginID "test.natron.built-in.StagedScaledPointOp"

NATRON_NAMESPACE_ENTER

namespace {

const double kTime = 1.;

// The op is an identity from this mipmap level down, so that its identity depends on the scale too.
const unsigned int kIdentityMipmapLevel = 2;

// Mirrors the std140 block `Params` of the shader below.
struct ScaleUniforms {
    float scaleX;
    float scaleY;
};

std::mutex gShaderMutex;
// Empty keeps the op off the GPU.
std::string gShaderSource;

// The mipmap level of the context the op's gpu() saw last, -1 before any call.
std::atomic<int> gLastGpuMipmapLevel(-1);

void
setShaderSource(const std::string& source)
{
    std::lock_guard<std::mutex> lock(gShaderMutex);
    gShaderSource = source;
}

// The tag makes the source unique, so the process-wide compile memo and earlier runs' disk entries
// serve no other test.
std::string
scaleShaderSource(const std::string& tag)
{
    return "#version 460\n// " + tag + "\n"
                                       "layout(local_size_x = 8, local_size_y = 8) in;\n"
                                       "layout(std430, set = 0, binding = 0) readonly buffer Src { float src[]; };\n"
                                       "layout(std430, set = 0, binding = 1) writeonly buffer Dst { float dst[]; };\n"
                                       "layout(std140, set = 0, binding = 2) uniform Params { float scaleX; float scaleY; };\n"
                                       "layout(push_constant) uniform Region { int x1; int y1; uint width; uint height; uint nComps; uint channelMask; } region;\n"
                                       "void main() {\n"
                                       "    if (gl_GlobalInvocationID.x >= region.width || gl_GlobalInvocationID.y >= region.height) return;\n"
                                       "    uint base = (gl_GlobalInvocationID.y * region.width + gl_GlobalInvocationID.x) * region.nComps;\n"
                                       "    for (uint c = 0u; c < region.nComps; ++c) {\n"
                                       "        dst[base + c] = (c == 0u) ? scaleX : ((c == 1u) ? scaleY : src[base + c]);\n"
                                       "    }\n"
                                       "}\n";
}

constexpr InputSpec kInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief A Point op of the scaled form: it writes the render scale into R and G and keeps B and A.
 **/
struct TestScaledPointOp {
    struct Params {
        int unused;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedScaledPointOpPluginID,
        .label = "Test Staged Scaled Point Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params { 0 };
    }

    bool isIdentity(const Params& /*params*/,
                    const PassContext& context) const
    {
        return context.scale.toMipmapLevel() >= kIdentityMipmapLevel;
    }

    void row(const Params& /*params*/,
             const PassContext& context,
             const RowIO& io) const
    {
        const OfxPointD scale = context.scale.toOfxPointD();
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;

        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                    dst[c] = (c == 0) ? (float)scale.x : ((c == 1) ? (float)scale.y : src[c]);
                }
            }
        }
    }

    GpuOpRef gpu(const Params& /*params*/,
                 const PassContext& context) const
    {
        gLastGpuMipmapLevel = (int)context.scale.toMipmapLevel();

        GpuRuntimeShader shader;
        {
            std::lock_guard<std::mutex> lock(gShaderMutex);
            if (gShaderSource.empty()) {
                return GpuOpRef::none();
            }
            shader.source = gShaderSource;
        }
        const OfxPointD scale = context.scale.toOfxPointD();
        ScaleUniforms block;
        block.scaleX = (float)scale.x;
        block.scaleY = (float)scale.y;
        GpuShaderInputs inputs;
        inputs.addUniform("Params", block);

        return GpuOpRef::runtime(shader, inputs);
    }
};

static_assert(PointOp<TestScaledPointOp>);
static_assert(ScaledPointOp<TestScaledPointOp>);

struct ScaleFreeTestOp {
    struct Params {
        int unused;
    };

    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params, const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;
};

static_assert(PointOp<ScaleFreeTestOp>);
static_assert(!ScaledPointOp<ScaleFreeTestOp>);

// Members of both forms, each form complete.
struct BothFormsTestOp {
    struct Params {
        int unused;
    };

    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params, const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;
    bool isIdentity(const Params& params, const PassContext& context) const;
    void row(const Params& params, const PassContext& context, const RowIO& io) const;
    GpuOpRef gpu(const Params& params, const PassContext& context) const;
};

static_assert(!PointOp<BothFormsTestOp>);

// A scale-free form completed by one scaled member it does not need.
struct ScaleFreeWithAScaledMemberTestOp {
    struct Params {
        int unused;
    };

    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params, const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;
    void row(const Params& params, const PassContext& context, const RowIO& io) const;
};

static_assert(!PointOp<ScaleFreeWithAScaledMemberTestOp>);

// Neither form complete.
struct MixedFormsTestOp {
    struct Params {
        int unused;
    };

    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params, const PassContext& context, const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;
};

static_assert(!PointOp<MixedFormsTestOp>);

typedef ImageNode<TestScaledPointOp> StagedScaledPointOp;

void
registerScaledPointOpOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(StagedScaledPointOp::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&StagedScaledPointOp::BuildEffect));
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
    });
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

class StagedPointContextTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerScaledPointOpOnce();
        BaseTest::SetUp();
    }

    virtual void TearDown() OVERRIDE
    {
        setShaderSource(std::string());
        BaseTest::TearDown();
    }

    bool createGraph(NodePtr* source,
                     NodePtr* op)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        EXPECT_TRUE(bool(*source));
        *op = createNode(QString::fromUtf8(kStagedScaledPointOpPluginID));
        EXPECT_TRUE(bool(*op));
        if (!*source || !*op) {
            return false;
        }
        setParitySourceComponents(*source, "rgba");
        connectNodes(*source, *op, 0, true);

        return true;
    }

    // The RGBA pixels of node over the source's window at mipmapLevel.
    static bool renderRGBA(const NodePtr& node,
                           unsigned int mipmapLevel,
                           const RectI& window,
                           std::vector<float>* pixels)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1)) {
            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }

    static std::vector<float> expectedFrom(const std::vector<float>& source,
                                           float scale)
    {
        std::vector<float> out(source);

        for (std::size_t i = 0; i + 3 < out.size(); i += 4) {
            out[i] = scale;
            out[i + 1] = scale;
        }

        return out;
    }

    static void expectSame(const std::vector<float>& want,
                           const std::vector<float>& got,
                           const char* what)
    {
        ASSERT_EQ(want.size(), got.size()) << what;
        const ImageDiff diff = compareImages(want, got, ParityTolerance::exact());

        EXPECT_EQ(0u, diff.mismatches) << what << ": float " << diff.firstMismatchIndex << " of " << diff.compared;
    }
};

// The op's row sees the scale the render runs at, and its identity follows that scale.
TEST_F(StagedPointContextTest, RowAndIdentitySeeTheRenderScaleOnTheCpu)
{
    NodePtr source;
    NodePtr op;
    ASSERT_TRUE(createGraph(&source, &op));

    const struct {
        unsigned int mipmapLevel;
        float scale;
    } cases[] = { { 0, 1.f }, { 1, 0.5f } };
    for (const auto& c : cases) {
        const RectI window = paritySourceWindow(source, kTime, c.mipmapLevel);
        ASSERT_FALSE(window.isNull()) << "mipmap level " << c.mipmapLevel;
        std::vector<float> sourcePixels;
        std::vector<float> opPixels;
        ASSERT_TRUE(renderRGBA(source, c.mipmapLevel, window, &sourcePixels));
        ASSERT_TRUE(renderRGBA(op, c.mipmapLevel, window, &opPixels));
        const std::string what = "mipmap level " + std::to_string(c.mipmapLevel);
        expectSame(expectedFrom(sourcePixels, c.scale), opPixels, what.c_str());
    }

    const RectI window = paritySourceWindow(source, kTime, kIdentityMipmapLevel);
    std::vector<float> sourcePixels;
    std::vector<float> opPixels;
    ASSERT_TRUE(renderRGBA(source, kIdentityMipmapLevel, window, &sourcePixels));
    ASSERT_TRUE(renderRGBA(op, kIdentityMipmapLevel, window, &opPixels));
    expectSame(sourcePixels, opPixels, "identity mipmap level");
}

// The GPU query's scale reaches the op's gpu(), with or without a device.
TEST_F(StagedPointContextTest, GpuSeesTheQueryScale)
{
    NodePtr source;
    NodePtr op;
    ASSERT_TRUE(createGraph(&source, &op));
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    for (unsigned int level = 0; level < 2; ++level) {
        GpuQuery query;
        query.time = kTime;
        query.view = ViewIdx(0);
        query.mappedScale = RenderScale::fromMipmapLevel(level);
        gLastGpuMipmapLevel = -1;
        EXPECT_FALSE(effect->canRenderOnGpu(query));
        EXPECT_EQ((int)level, gLastGpuMipmapLevel.load());
    }
}

#ifdef NATRON_HAVE_GPU

// A scaled op's shader renders on the device with the scale its gpu() was given, and matches the CPU.
TEST_F(StagedPointContextTest, ScaledShaderRendersOnTheGpuAndMatchesTheCpu)
{
    NodePtr source;
    NodePtr op;
    ASSERT_TRUE(createGraph(&source, &op));
    const RectI window = paritySourceWindow(source, kTime, 0);
    std::vector<float> sourcePixels;
    ASSERT_TRUE(renderRGBA(source, 0, window, &sourcePixels));

    setShaderSource(scaleShaderSource(uniqueTag("staged scaled")));

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, op), op, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ranInRegion(op));
    expectSame(expectedFrom(sourcePixels, 1.f), result.cpu, "CPU frame");
    expectSame(result.cpu, result.gpu, "GPU frame");
}

#endif // NATRON_HAVE_GPU
