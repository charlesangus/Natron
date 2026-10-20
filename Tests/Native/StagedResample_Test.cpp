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

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

#define kStagedResampleOpPluginID "test.natron.built-in.StagedResampleOp"
#define kStagedMaskedResampleOpPluginID "test.natron.built-in.StagedMaskedResampleOp"
#define kStagedResampleParamTranslate "translate"
#define kStagedResampleParamScale "scale"

NATRON_NAMESPACE_ENTER

namespace {

const double kTime = 1.;
const double kTranslateX = 3.25;
const double kTranslateY = -2.5;
const double kScaleX = 1.5;
const double kScaleY = 0.75;
const double kMix = 0.5;

constexpr InputSpec kInputs[] = {
    InputSpec { .label = "Source" },
};

constexpr InputSpec kMaskedInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief A matrix-form Resample op that scales about the origin, then translates, sampling with
 * the cubic filter and black outside.
 **/
struct TestResampleOp {
    struct Params {
        double translateX;
        double translateY;
        double scaleX;
        double scaleY;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedResampleOpPluginID,
        .label = "Test Staged Resample Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    static constexpr NodeSpec kMaskedNodeSpec = {
        .id = kStagedMaskedResampleOpPluginID,
        .label = "Test Staged Masked Resample Op",
        .grouping = "Other/Test",
        .inputs = kMaskedInputs,
    };

    static constexpr ImagePipelineSpec kMaskedPipeline = {
        .stages = { Stage::MaskMix },
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    KnobDoubleWPtr translate;
    KnobDoubleWPtr scale;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(holder, std::string("Translate"), 2);
        translateKnob->setName(kStagedResampleParamTranslate);
        translateKnob->setDefaultValue(0., 0);
        translateKnob->setDefaultValue(0., 1);
        page->addKnob(translateKnob);
        translate = translateKnob;

        KnobDoublePtr scaleKnob = AppManager::createKnob<KnobDouble>(holder, std::string("Scale"), 2);
        scaleKnob->setName(kStagedResampleParamScale);
        scaleKnob->setDefaultValue(1., 0);
        scaleKnob->setDefaultValue(1., 1);
        page->addKnob(scaleKnob);
        scale = scaleKnob;
    }

    Params bake(const KnobReader& reader) const
    {
        Params p;

        p.translateX = reader.valueOr(translate, 0., 0);
        p.translateY = reader.valueOr(translate, 0., 1);
        p.scaleX = reader.valueOr(scale, 1., 0);
        p.scaleY = reader.valueOr(scale, 1., 1);

        return p;
    }

    bool isIdentity(const Params& params,
                    const PassContext& /*context*/) const
    {
        return (params.translateX == 0.) && (params.translateY == 0.) && (params.scaleX == 1.) && (params.scaleY == 1.);
    }

    ResampleSettings settings(const Params& params,
                              const PassContext& context) const
    {
        ResampleSettings s;

        s.filter = Resampler::eFilterCubic;
        s.clamp = false;
        s.blackOutside = true;
        s.invert = false;
        s.transformIsIdentity = isIdentity(params, context);

        return s;
    }

    bool transform(const Params& params,
                   const PassContext& /*context*/,
                   double /*amount*/,
                   bool invert,
                   TransformMath::Mat3* matrix) const
    {
        *matrix = invert ? TransformMath::transformCanonical(params.translateX, params.translateY, params.scaleX, params.scaleY, 0., 0., false, 0., 0., 0.)
                         : TransformMath::inverseTransformCanonical(params.translateX, params.translateY, params.scaleX, params.scaleY, 0., 0., false, 0., 0., 0.);

        return true;
    }
};

static_assert(ResampleOp<TestResampleOp>);
static_assert(MatrixResampleOp<TestResampleOp>);
static_assert(OpOfKind<TestResampleOp, OpKind::Resample>);
static_assert(!OpOfKind<TestResampleOp, OpKind::Point>);

struct OffsetTestOp {
    struct Params {
        int unused;
    };

    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params, const PassContext& context) const;
    PixelOffset offset(const Params& params, const PassContext& context) const;
};

static_assert(ResampleOp<OffsetTestOp>);
static_assert(!MatrixResampleOp<OffsetTestOp>);

// Members of both forms, each form complete.
struct BothResampleFormsTestOp {
    struct Params {
        int unused;
    };

    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params, const PassContext& context) const;
    ResampleSettings settings(const Params& params, const PassContext& context) const;
    bool transform(const Params& params, const PassContext& context, double amount, bool invert, TransformMath::Mat3* matrix) const;
    PixelOffset offset(const Params& params, const PassContext& context) const;
};

static_assert(!ResampleOp<BothResampleFormsTestOp>);

typedef ImageNode<TestResampleOp> StagedResampleOp;
typedef ImageNode<TestResampleOp, TestResampleOp::kMaskedNodeSpec, TestResampleOp::kMaskedPipeline> StagedMaskedResampleOp;

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
registerResampleOpsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNode<StagedResampleOp>();
        registerNode<StagedMaskedResampleOp>();
    });
}

Resampler::CanonicalTransformFn
testTransformFn()
{
    return [](double /*time*/, double /*amount*/, bool invert, TransformMath::Mat3* matrix) {
        *matrix = invert ? TransformMath::transformCanonical(kTranslateX, kTranslateY, kScaleX, kScaleY, 0., 0., false, 0., 0., 0.)
                         : TransformMath::inverseTransformCanonical(kTranslateX, kTranslateY, kScaleX, kScaleY, 0., 0., false, 0., 0., 0.);

        return true;
    };
}

Resampler::RegionParams
testRegionParams(bool doMasking,
                 double mix)
{
    Resampler::RegionParams params;

    params.invert = false;
    params.filter = Resampler::eFilterCubic;
    params.blackOutside = true;
    params.isIdentity = false;
    params.doMasking = doMasking;
    params.mix = doMasking ? mix : 1.;

    return params;
}

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class StagedResampleTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerResampleOpsOnce();
        BaseTest::SetUp();
    }

    // A parity source connected to a new op, translated and scaled; with masked, the masked op
    // with a second parity source, offset from the first, on its mask input and a mix of 0.5.
    bool createGraph(bool masked,
                     NodePtr* source,
                     NodePtr* mask,
                     NodePtr* op)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        *op = createNode(QString::fromUtf8(masked ? kStagedMaskedResampleOpPluginID : kStagedResampleOpPluginID));
        EXPECT_TRUE(bool(*source));
        EXPECT_TRUE(bool(*op));
        if (!*source || !*op) {
            return false;
        }
        setParitySourceComponents(*source, "rgba");
        connectNodes(*source, *op, 0, true);
        if (!setKnobValues(*op, kStagedResampleParamTranslate, { kTranslateX, kTranslateY }) || !setKnobValues(*op, kStagedResampleParamScale, { kScaleX, kScaleY })) {
            return false;
        }
        if (!masked) {
            return true;
        }
        *mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        EXPECT_TRUE(bool(*mask));
        if (!*mask) {
            return false;
        }
        setParitySourceComponents(*mask, "rgba");
        setParitySourceOrigin(*mask, 10, 6);
        connectNodes(*mask, *op, 1, true);

        return setKnobValues(*op, "enableMask_Mask", { 1. }) && setKnobValues(*op, kOfxMixParamName, { kMix });
    }

    static bool renderRGBA(const NodePtr& node,
                           unsigned int mipmapLevel,
                           const RectI& window,
                           std::vector<float>* pixels)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }

    static bool regionOfDefinition(const NodePtr& node,
                                   unsigned int mipmapLevel,
                                   RectD* rod)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        bool isProjectFormat = false;

        return effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), rod, &isProjectFormat) != eStatusFailed;
    }

    RectD projectRect() const
    {
        Format format;

        getApp()->getProject()->getProjectDefaultFormat(&format);
        const RectI pixel(format.x1, format.y1, format.x2, format.y2);

        return pixel.toCanonical_noClipping(0, format.getPixelAspectRatio());
    }

    // The op's pixels as Resampler computes them from the source's: the source resampled through
    // the test matrix over window, then, with masked, blended with the source under each pixel by
    // the mask's alpha times the mix.
    void expectRenderMatchesResampler(bool masked)
    {
        NodePtr source;
        NodePtr mask;
        NodePtr op;
        ASSERT_TRUE(createGraph(masked, &source, &mask, &op));

        for (unsigned int level = 0; level < 2; ++level) {
            const std::string what = std::string(masked ? "masked" : "plain") + ", mipmap level " + std::to_string(level);
            RectD rod;
            ASSERT_TRUE(regionOfDefinition(op, level, &rod)) << what;
            const RectI window = rod.toPixelEnclosing(level, 1.);
            ASSERT_FALSE(window.isNull()) << what;

            const RectI sourceWindow = paritySourceWindow(source, kTime, level);
            std::vector<float> sourcePixels;
            ASSERT_TRUE(renderRGBA(source, level, sourceWindow, &sourcePixels)) << what;
            std::vector<float> under;
            std::vector<float> maskPixels;
            if (masked) {
                ASSERT_TRUE(renderRGBA(source, level, window, &under)) << what;
                ASSERT_TRUE(renderRGBA(mask, level, window, &maskPixels)) << what;
            }
            std::vector<float> got;
            ASSERT_TRUE(renderRGBA(op, level, window, &got)) << what;

            const double s = RenderScale::fromMipmapLevel(level).toOfxPointD().x;
            const Resampler::SourceImage image(&sourcePixels[0], sourceWindow, 4);
            Resampler::SamplingTransforms transforms;
            Resampler::buildSamplingTransforms(testTransformFn(), kTime, false, Resampler::BlurSettings(), s, s, false, 1., 1., &transforms);
            const Resampler::ResampleParams params = Resampler::makeResampleParams(transforms, Resampler::eFilterCubic, false, true);

            const int width = window.width();
            std::vector<float> want((std::size_t)width * window.height() * 4);
            for (int y = window.y1; y < window.y2; ++y) {
                float* row = &want[(std::size_t)(y - window.y1) * width * 4];
                Resampler::resampleRow(params, image, y, window.x1, window.x2, row);
                if (!masked) {
                    continue;
                }
                const float mix = (float)kMix;
                for (int i = 0; i < width; ++i) {
                    const std::size_t pixel = ((std::size_t)(y - window.y1) * width + i) * 4;
                    const float alpha = maskPixels[pixel + 3] * mix;
                    for (int c = 0; c < 4; ++c) {
                        const float src = under[pixel + c];
                        float v = row[(std::size_t)i * 4 + c];
                        if (alpha == 0.f) {
                            v = src;
                        } else if (alpha != 1.f) {
                            v = v * alpha + (1.f - alpha) * src;
                        }
                        row[(std::size_t)i * 4 + c] = v;
                    }
                }
            }

            ASSERT_EQ(want.size(), got.size()) << what;
            const ImageDiff diff = compareImages(want, got, ParityTolerance::exact());
            EXPECT_EQ(0u, diff.mismatches) << what << ": float " << diff.firstMismatchIndex << " of " << diff.compared;
        }
    }
};

// The op renders bit-exact what Resampler computes from its source, at full and half scale.
TEST_F(StagedResampleTest, RendersLikeTheResampler)
{
    expectRenderMatchesResampler(false);
}

// Under a mask and a mix of 0.5 the resampled pixels are blended with the source under them.
TEST_F(StagedResampleTest, MaskAndMixBlendWithTheSourceUnderThePixel)
{
    expectRenderMatchesResampler(true);
}

// The region of definition is the source's mapped through the forward matrix and grown by a
// pixel for black outside; under a mask it also covers the source's.
TEST_F(StagedResampleTest, RegionOfDefinitionFollowsTheMatrix)
{
    for (int masked = 0; masked < 2; ++masked) {
        NodePtr source;
        NodePtr mask;
        NodePtr op;
        ASSERT_TRUE(createGraph(masked != 0, &source, &mask, &op));

        for (unsigned int level = 0; level < 2; ++level) {
            const std::string what = std::string(masked ? "masked" : "plain") + ", mipmap level " + std::to_string(level);
            RectD srcRoD;
            ASSERT_TRUE(regionOfDefinition(source, level, &srcRoD)) << what;
            RectD rod;
            ASSERT_TRUE(regionOfDefinition(op, level, &rod)) << what;

            const double s = RenderScale::fromMipmapLevel(level).toOfxPointD().x;
            RectD want;
            Resampler::getRegionOfDefinition(testTransformFn(), srcRoD, kTime, 1., s, s, testRegionParams(masked != 0, kMix), &want);
            EXPECT_EQ(want.x1, rod.x1) << what;
            EXPECT_EQ(want.y1, rod.y1) << what;
            EXPECT_EQ(want.x2, rod.x2) << what;
            EXPECT_EQ(want.y2, rod.y2) << what;
            if (masked) {
                EXPECT_LE(rod.x1, srcRoD.x1) << what;
                EXPECT_GE(rod.x2, srcRoD.x2) << what;
            }
        }
    }
}

// The source's region of interest is the window mapped back through the matrix and grown by the
// filter's reach; under a mask it also covers the window, and the mask is read under the window.
TEST_F(StagedResampleTest, RegionsOfInterestFollowTheMatrix)
{
    const RectD window(10., 5., 40., 30.);

    for (int masked = 0; masked < 2; ++masked) {
        NodePtr source;
        NodePtr mask;
        NodePtr op;
        ASSERT_TRUE(createGraph(masked != 0, &source, &mask, &op));
        EffectInstancePtr effect = op->getEffectInstance();

        for (unsigned int level = 0; level < 2; ++level) {
            const std::string what = std::string(masked ? "masked" : "plain") + ", mipmap level " + std::to_string(level);
            const RenderScale scale = RenderScale::fromMipmapLevel(level);
            RectD srcRoD;
            ASSERT_TRUE(regionOfDefinition(source, level, &srcRoD)) << what;
            RectD rod;
            ASSERT_TRUE(regionOfDefinition(op, level, &rod)) << what;

            RoIMap rois;
            effect->getRegionsOfInterest_public(kTime, scale, rod, window, ViewIdx(0), &rois);

            const double s = scale.toOfxPointD().x;
            const Resampler::RegionParams params = testRegionParams(masked != 0, kMix);
            RectD want;
            Resampler::getRegionOfInterest(testTransformFn(), window, srcRoD, projectRect(), kTime, 1., s, s, params, &want);
            if (masked) {
                RectD reference = window;
                Resampler::expandRoI(window, 1., s, s, params.filter, false, 1., &reference);
                want.merge(reference);
            }

            RoIMap::const_iterator found = rois.find(source->getEffectInstance());
            ASSERT_TRUE(found != rois.end()) << what;
            EXPECT_EQ(want.x1, found->second.x1) << what;
            EXPECT_EQ(want.y1, found->second.y1) << what;
            EXPECT_EQ(want.x2, found->second.x2) << what;
            EXPECT_EQ(want.y2, found->second.y2) << what;

            if (masked) {
                RoIMap::const_iterator maskFound = rois.find(mask->getEffectInstance());
                ASSERT_TRUE(maskFound != rois.end()) << what;
                EXPECT_EQ(window.x1, maskFound->second.x1) << what;
                EXPECT_EQ(window.y1, maskFound->second.y1) << what;
                EXPECT_EQ(window.x2, maskFound->second.x2) << what;
                EXPECT_EQ(window.y2, maskFound->second.y2) << what;
            }
        }
    }
}

// The op hands the host its forward pixel matrix for concatenation, and only the node without
// mask and mix offers it; both apply the transforms concatenated into their source.
TEST_F(StagedResampleTest, TransformIsTheForwardPixelMatrix)
{
    for (int masked = 0; masked < 2; ++masked) {
        NodePtr source;
        NodePtr mask;
        NodePtr op;
        ASSERT_TRUE(createGraph(masked != 0, &source, &mask, &op));
        EffectInstancePtr effect = op->getEffectInstance();
        const std::string kind = masked ? "masked" : "plain";

        EXPECT_EQ(masked == 0, effect->getCanTransform()) << kind;
        std::list<int> inputs;
        EXPECT_TRUE(effect->getInputsHoldingTransform(&inputs)) << kind;
        ASSERT_EQ(1u, inputs.size()) << kind;
        EXPECT_EQ(0, inputs.front()) << kind;

        for (unsigned int level = 0; level < 2; ++level) {
            const std::string what = kind + ", mipmap level " + std::to_string(level);
            const RenderScale scale = RenderScale::fromMipmapLevel(level);
            EffectInstancePtr inputToTransform;
            Transform::Matrix3x3 matrix;
            ASSERT_EQ(eStatusOK, effect->getTransform_public(kTime, scale, false, ViewIdx(0), &inputToTransform, &matrix)) << what;
            EXPECT_EQ(source->getEffectInstance(), inputToTransform) << what;

            const double s = scale.toOfxPointD().x;
            const TransformMath::Mat3 invCanonical = TransformMath::inverseTransformCanonical(kTranslateX, kTranslateY, kScaleX, kScaleY, 0., 0., false, 0., 0., 0.);
            TransformMath::Mat3 canonical;
            ASSERT_TRUE(invCanonical.inverse(&canonical)) << what;
            const TransformMath::Mat3 want = TransformMath::forwardToPixel(canonical, 1., 1., s, s);
            const TransformMath::Mat3 got = TransformMath::fromEngineMatrix(matrix);
            for (int k = 0; k < 9; ++k) {
                EXPECT_EQ(want.m[k], got.m[k]) << what << ": element " << k;
            }

            // Source pixel (x, y) lands on output pixel (scaleX x + translateX s, scaleY y + translateY s).
            EXPECT_NEAR(kScaleX, got(0, 0), 1e-12) << what;
            EXPECT_NEAR(kTranslateX * s, got(0, 2), 1e-12) << what;
            EXPECT_NEAR(kScaleY, got(1, 1), 1e-12) << what;
            EXPECT_NEAR(kTranslateY * s, got(1, 2), 1e-12) << what;
            EXPECT_NEAR(0., got(0, 1), 1e-12) << what;
            EXPECT_NEAR(0., got(1, 0), 1e-12) << what;
        }
    }
}

// An op at its identity leaves the source unchanged.
TEST_F(StagedResampleTest, IdentityPassesTheSourceThrough)
{
    NodePtr source;
    NodePtr mask;
    NodePtr op;
    ASSERT_TRUE(createGraph(false, &source, &mask, &op));
    ASSERT_TRUE(setKnobValues(op, kStagedResampleParamTranslate, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(op, kStagedResampleParamScale, { 1., 1. }));

    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;
    const RectI window = paritySourceWindow(source, kTime, 0);
    EffectInstancePtr effect = op->getEffectInstance();
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    std::vector<float> sourcePixels;
    std::vector<float> opPixels;
    ASSERT_TRUE(renderRGBA(source, 0, window, &sourcePixels));
    ASSERT_TRUE(renderRGBA(op, 0, window, &opPixels));
    ASSERT_EQ(sourcePixels.size(), opPixels.size());
    const ImageDiff diff = compareImages(sourcePixels, opPixels, ParityTolerance::exact());
    EXPECT_EQ(0u, diff.mismatches) << "float " << diff.firstMismatchIndex << " of " << diff.compared;
}
