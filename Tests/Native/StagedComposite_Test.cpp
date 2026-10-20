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

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kStagedTwoSourceOpPluginID "test.natron.built-in.StagedTwoSourceOp"
#define kStagedFourSourceOpPluginID "test.natron.built-in.StagedFourSourceOp"
#define kStagedRoutedPointOpPluginID "test.natron.built-in.StagedRoutedPointOp"
#define kStagedParamBlendFromB "blendFromB"
#define kStagedParamBypass "bypass"

NATRON_NAMESPACE_ENTER

namespace {

const double kTime = 1.;
const double kMix = 0.5;

constexpr InputSpec kTwoSourceInputs[] = {
    InputSpec { .label = "A" },
    InputSpec { .label = "B", .optional = true },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief A Composite op reading A and B, writing A - B, whose stages blend against A, or against B
 * with blendFromB.
 **/
struct TestTwoSourceOp {
    struct Params {
        bool blendFromB;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedTwoSourceOpPluginID,
        .label = "Test Staged Two Source Op",
        .grouping = "Other/Test",
        .inputs = kTwoSourceInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Composite,
    };

    KnobBoolWPtr blendFromB;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string("Blend From B"));
        knob->setName(kStagedParamBlendFromB);
        knob->setDefaultValue(false);
        page->addKnob(knob);
        blendFromB = knob;
    }

    Params bake(const KnobReader& reader) const
    {
        return Params { reader.valueOr(blendFromB, false) };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    CompositeSources sources(const Params& params,
                             const RouteContext& context) const
    {
        CompositeSources s;

        s.add(0, context.time());
        s.add(1, context.time());
        s.blendInput = params.blendFromB ? 1 : -1;

        return s;
    }

    void row(const Params& /*params*/,
             const RowIO& io) const
    {
        const int nComps = io.nComps;
        const float* a = io.src[0];
        const float* b = io.src[1];
        float* dst = io.dst;

        for (int x = 0; x < io.width; ++x, a += nComps, b += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                    dst[c] = a[c] - b[c];
                }
            }
        }
    }
};

static_assert(CompositeOp<TestTwoSourceOp>);
static_assert(OpOfKind<TestTwoSourceOp, OpKind::Composite>);
static_assert(!PointOp<TestTwoSourceOp>);
static_assert(!RoutesOp<TestTwoSourceOp>);

constexpr InputSpec kFourSourceInputs[] = {
    InputSpec { .label = "A" },
    InputSpec { .label = "Matte", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "B", .optional = true },
    InputSpec { .label = "C", .optional = true },
};

/**
 * @brief A Composite op reading A, the one channel of the Mask-role input Matte, B and C, writing
 * A + Matte x B - C.
 **/
struct TestFourSourceOp {
    struct Params {
        int unused;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedFourSourceOpPluginID,
        .label = "Test Staged Four Source Op",
        .grouping = "Other/Test",
        .inputs = kFourSourceInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Composite,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params { 0 };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    CompositeSources sources(const Params& /*params*/,
                             const RouteContext& context) const
    {
        CompositeSources s;

        for (int i = 0; i < 4; ++i) {
            s.add(i, context.time());
        }

        return s;
    }

    void row(const Params& /*params*/,
             const RowIO& io) const
    {
        const int nComps = io.nComps;
        const float* a = io.src[0];
        const float* matte = io.src[1];
        const float* b = io.src[2];
        const float* cc = io.src[3];
        float* dst = io.dst;

        for (int x = 0; x < io.width; ++x, a += nComps, ++matte, b += nComps, cc += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                    const float scaled = matte[0] * b[c];
                    dst[c] = (a[c] + scaled) - cc[c];
                }
            }
        }
    }
};

static_assert(CompositeOp<TestFourSourceOp>);

// Five inputs are one more than a Composite op reads.
static_assert([] {
    CompositeSources s;
    for (int i = 0; i < kPixelKernelMaxInputs; ++i) {
        if (!s.add(i, 0.)) {
            return false;
        }
    }

    return !s.add(kPixelKernelMaxInputs, 0.) && (s.count == kPixelKernelMaxInputs);
}());

constexpr InputSpec kRoutedInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief A Point op doubling its source, whose route() makes it an identity of its source with
 * bypass, while its own isIdentity() never does.
 **/
struct TestRoutedPointOp {
    struct Params {
        bool bypass;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedRoutedPointOpPluginID,
        .label = "Test Staged Routed Point Op",
        .grouping = "Other/Test",
        .inputs = kRoutedInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    KnobBoolWPtr bypass;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string("Bypass"));
        knob->setName(kStagedParamBypass);
        knob->setDefaultValue(false);
        page->addKnob(knob);
        bypass = knob;
    }

    Params bake(const KnobReader& reader) const
    {
        return Params { reader.valueOr(bypass, false) };
    }

    Route route(const Params& params,
                const RouteContext& context) const
    {
        return params.bypass ? Route::input(0, context.time()) : Route::none();
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    void row(const Params& /*params*/,
             const RowIO& io) const
    {
        const int nComps = io.nComps;
        const float* src = io.src[0];
        float* dst = io.dst;

        for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                    dst[c] = src[c] * 2.f;
                }
            }
        }
    }

    GpuOpRef gpu(const Params& /*params*/) const
    {
        return GpuOpRef::none();
    }
};

static_assert(PointOp<TestRoutedPointOp>);
static_assert(RoutesOp<TestRoutedPointOp>);
static_assert(!CompositeOp<TestRoutedPointOp>);

typedef ImageNode<TestTwoSourceOp> StagedTwoSourceOp;
typedef ImageNode<TestFourSourceOp> StagedFourSourceOp;
typedef ImageNode<TestRoutedPointOp> StagedRoutedPointOp;

template <typename NODE>
void
registerNode()
{
    EffectInstancePtr node(NODE::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&NODE::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerCompositeOpsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNode<StagedTwoSourceOp>();
        registerNode<StagedFourSourceOp>();
        registerNode<StagedRoutedPointOp>();
    });
}

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class StagedCompositeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerCompositeOpsOnce();
        BaseTest::SetUp();
    }

    // An RGBA parity source with its region of definition at (x, y).
    NodePtr createSource(int x,
                         int y)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, "rgba");
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    // The RGBA pixels of node over window at full scale, zero outside its region of definition.
    static bool renderRGBA(const NodePtr& node,
                           const RectI& window,
                           std::vector<float>* pixels)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }

    static void expectSame(const std::vector<float>& want,
                           const std::vector<float>& got,
                           const std::string& what)
    {
        ASSERT_EQ(want.size(), got.size()) << what;
        const ImageDiff diff = compareImages(want, got, ParityTolerance::exact());

        EXPECT_EQ(0u, diff.mismatches) << what << ": float " << diff.firstMismatchIndex << " of " << diff.compared;
    }

    static int identityInput(const NodePtr& node,
                             const RectI& window,
                             bool* identity)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -2;

        *identity = effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

        return inputNb;
    }

    // A over the window, B offset from it, and a mask offset elsewhere with a mix of 0.5: the op's
    // A - B, blended by mask alpha x mix with A, or with B under blendFromB.
    void expectTwoSourceBlend(bool blendFromB)
    {
        const std::string what = blendFromB ? "blend from B" : "blend from A";
        NodePtr a = createSource(0, 0);
        NodePtr b = createSource(10, 6);
        NodePtr mask = createSource(5, 20);
        NodePtr op = createNode(QString::fromUtf8(kStagedTwoSourceOpPluginID));
        ASSERT_TRUE(a && b && mask && op) << what;
        connectNodes(a, op, 0, true);
        connectNodes(b, op, 1, true);
        connectNodes(mask, op, 2, true);
        ASSERT_TRUE(setKnobValues(op, "enableMask_Mask", { 1. })) << what;
        ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, { kMix })) << what;
        ASSERT_TRUE(setKnobValues(op, kStagedParamBlendFromB, { blendFromB ? 1. : 0. })) << what;

        const RectI window = paritySourceWindow(a, kTime, 0);
        ASSERT_FALSE(window.isNull()) << what;
        std::vector<float> aPixels;
        std::vector<float> bPixels;
        std::vector<float> maskPixels;
        std::vector<float> got;
        ASSERT_TRUE(renderRGBA(a, window, &aPixels)) << what;
        ASSERT_TRUE(renderRGBA(b, window, &bPixels)) << what;
        ASSERT_TRUE(renderRGBA(mask, window, &maskPixels)) << what;
        ASSERT_TRUE(renderRGBA(op, window, &got)) << what;

        // B covers only part of the window, so some of A - B reads B as zero.
        bool bMissesSomewhere = false;
        std::vector<float> want(aPixels.size());
        const float mix = (float)kMix;
        for (std::size_t p = 0; p + 3 < want.size(); p += 4) {
            const float alpha = maskPixels[p + 3] * mix;
            bMissesSomewhere = bMissesSomewhere || ((bPixels[p] == 0.f) && (bPixels[p + 3] == 0.f) && (aPixels[p] != 0.f));
            for (int c = 0; c < 4; ++c) {
                const float under = blendFromB ? bPixels[p + c] : aPixels[p + c];
                float v = aPixels[p + c] - bPixels[p + c];
                if (alpha == 0.f) {
                    v = under;
                } else if (alpha != 1.f) {
                    v = v * alpha + (1.f - alpha) * under;
                }
                want[p + c] = v;
            }
        }
        EXPECT_TRUE(bMissesSomewhere) << what;
        expectSame(want, got, what);

        // At a mix of 0 the node is an identity of its blend input.
        ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, { 0. })) << what;
        bool identity = false;
        const int inputNb = identityInput(op, window, &identity);
        EXPECT_TRUE(identity) << what;
        EXPECT_EQ(blendFromB ? 1 : 0, inputNb) << what;
    }
};

// Two inputs, the second offset so that it reads zero where it has no pixel, under a mask and a
// mix of 0.5, blended with the first input.
TEST_F(StagedCompositeTest, TwoSourcesWithAnOffsetInputUnderMaskAndMix)
{
    expectTwoSourceBlend(false);
}

// The op names its second input as the one the stages blend against and the identity answers.
TEST_F(StagedCompositeTest, BlendInputOtherThanTheFirst)
{
    expectTwoSourceBlend(true);
}

// Four inputs, one of them a Mask-role input read as the channel its selector names, and zero
// once that mask is disabled.
TEST_F(StagedCompositeTest, FourSourcesWithAMaskRoleInput)
{
    NodePtr a = createSource(0, 0);
    NodePtr matte = createSource(5, 20);
    NodePtr b = createSource(10, 6);
    NodePtr cc = createSource(24, 30);
    NodePtr op = createNode(QString::fromUtf8(kStagedFourSourceOpPluginID));
    ASSERT_TRUE(a && matte && b && cc && op);
    connectNodes(a, op, 0, true);
    connectNodes(matte, op, 1, true);
    connectNodes(b, op, 2, true);
    connectNodes(cc, op, 3, true);

    const RectI window = paritySourceWindow(a, kTime, 0);
    ASSERT_FALSE(window.isNull());
    std::vector<float> aPixels;
    std::vector<float> mattePixels;
    std::vector<float> bPixels;
    std::vector<float> cPixels;
    ASSERT_TRUE(renderRGBA(a, window, &aPixels));
    ASSERT_TRUE(renderRGBA(matte, window, &mattePixels));
    ASSERT_TRUE(renderRGBA(b, window, &bPixels));
    ASSERT_TRUE(renderRGBA(cc, window, &cPixels));

    for (int enabled = 1; enabled >= 0; --enabled) {
        const std::string what = enabled ? "matte enabled" : "matte disabled";
        ASSERT_TRUE(setKnobValues(op, "enableMask_Matte", { enabled ? 1. : 0. })) << what;
        std::vector<float> got;
        ASSERT_TRUE(renderRGBA(op, window, &got)) << what;

        std::vector<float> want(aPixels.size());
        for (std::size_t p = 0; p + 3 < want.size(); p += 4) {
            const float m = enabled ? mattePixels[p + 3] : 0.f;
            for (int c = 0; c < 4; ++c) {
                const float scaled = m * bPixels[p + c];
                want[p + c] = (aPixels[p + c] + scaled) - cPixels[p + c];
            }
        }
        expectSame(want, got, what);
    }
}

// A Point op's route() makes the node an identity of the input it names, which the op's own
// isIdentity() never does.
TEST_F(StagedCompositeTest, PointOpRouteAnswersIdentity)
{
    NodePtr source = createSource(0, 0);
    NodePtr op = createNode(QString::fromUtf8(kStagedRoutedPointOpPluginID));
    ASSERT_TRUE(source && op);
    connectNodes(source, op, 0, true);

    const RectI window = paritySourceWindow(source, kTime, 0);
    ASSERT_FALSE(window.isNull());
    std::vector<float> sourcePixels;
    ASSERT_TRUE(renderRGBA(source, window, &sourcePixels));

    bool identity = true;
    static_cast<void>(identityInput(op, window, &identity));
    EXPECT_FALSE(identity);
    std::vector<float> doubled(sourcePixels);
    for (std::size_t i = 0; i < doubled.size(); ++i) {
        doubled[i] = sourcePixels[i] * 2.f;
    }
    std::vector<float> got;
    ASSERT_TRUE(renderRGBA(op, window, &got));
    expectSame(doubled, got, "not bypassed");

    ASSERT_TRUE(setKnobValues(op, kStagedParamBypass, { 1. }));
    const int inputNb = identityInput(op, window, &identity);
    EXPECT_TRUE(identity);
    EXPECT_EQ(0, inputNb);
    ASSERT_TRUE(renderRGBA(op, window, &got));
    expectSame(sourcePixels, got, "bypassed");
}
