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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
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

#define kStagedFoldOpPluginID "test.natron.built-in.StagedFoldOp"
#define kStagedRoutedPointOpPluginID "test.natron.built-in.StagedRoutedPointOp"
#define kStagedParamFoldCount "foldCount"
#define kStagedParamBlendInput "blendInput"
#define kStagedParamBypass "bypass"
#define kStagedAlphaFoldOpPluginID "test.natron.built-in.StagedAlphaFoldOp"
#define kStagedParamAlphaMode "alphaMode"
#define kStagedParamFoldA2 "foldA2"
#define kStagedParamALayer "aLayer"
#define kStagedParamAAlpha "aAlpha"
#define kStagedParamAAlphaLayer "aAlphaLayer"
#define kStagedParamBAlpha "bAlpha"

NATRON_NAMESPACE_ENTER

namespace {

const double kTime = 1.;
const double kMix = 0.5;
const int kFoldMaskInput = 1;
const int kFoldMaxInputs = 5;

// The input number of the k-th folded input: A, then the Variadic run after the mask.
constexpr int
foldInput(int k)
{
    return (k == 0) ? 0 : k + 1;
}

constexpr InputSpec kFoldInputs[] = {
    InputSpec { .label = "A" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "In", .role = InputRole::Variadic, .optional = true, .count = kFoldMaxInputs - 1, .firstNumber = 1 },
};

/**
 * @brief A Composite op folding A and the first foldCount - 1 inputs of its Variadic run, each step
 * halving the result and adding the next input, so that the count and the order of the fold both
 * show in the result. The stages blend against blendInput, or A when it is -1.
 **/
struct TestFoldOp {
    struct Params {
        int foldCount;
        int blendInput;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedFoldOpPluginID,
        .label = "Test Staged Fold Op",
        .grouping = "Other/Test",
        .inputs = kFoldInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Composite,
    };

    KnobIntWPtr foldCount;
    KnobIntWPtr blendInput;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        KnobIntPtr count = AppManager::createKnob<KnobInt>(holder, std::string("Fold Count"));
        count->setName(kStagedParamFoldCount);
        count->setDefaultValue(2);
        page->addKnob(count);
        foldCount = count;

        KnobIntPtr blend = AppManager::createKnob<KnobInt>(holder, std::string("Blend Input"));
        blend->setName(kStagedParamBlendInput);
        blend->setDefaultValue(-1);
        page->addKnob(blend);
        blendInput = blend;
    }

    Params bake(const KnobReader& reader) const
    {
        return Params { reader.valueOr(foldCount, 2), reader.valueOr(blendInput, -1) };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    CompositeInputs inputs(const Params& params,
                           const RouteContext& /*context*/) const
    {
        CompositeInputs folded;

        folded.base = CompositeInput { foldInput(0) };
        for (int k = 1; k < params.foldCount; ++k) {
            folded.add(foldInput(k));
        }
        folded.blendInput = params.blendInput;

        return folded;
    }

    void step(const Params& /*params*/,
              const RowIO& io) const
    {
        const int nComps = io.nComps;
        const float* result = io.src[0];
        const float* input = io.src[1];
        float* dst = io.dst;

        for (int x = 0; x < io.width; ++x, result += nComps, input += nComps, dst += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                    dst[c] = result[c] * 0.5f + input[c];
                }
            }
        }
    }
};

static_assert(CompositeOp<TestFoldOp>);
static_assert(OpOfKind<TestFoldOp, OpKind::Composite>);
static_assert(!PointOp<TestFoldOp>);
static_assert(!RoutesOp<TestFoldOp>);

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

// How TestAlphaFoldOp's A input gets its alpha, in the order of the alphaMode knob.
enum class TestAlphaMode {
    Channel,
    Layer,
    ReadLayer
};

constexpr InputSpec kAlphaFoldInputs[] = {
    InputSpec { .label = "B" },
    InputSpec { .label = "A", .optional = true },
    InputSpec { .label = "A2", .optional = true },
};

/**
 * @brief A Composite op folding A, read in the layer aLayer selects, over B, and then, with foldA2,
 * A2 over that result in its own plane, each step the Porter-Duff xor of the input and the result
 * through their alphas, per channel, so that both alphas and the result's carried to the next step
 * show in the result. B's and A2's alphas are the channel bAlpha and aAlpha select, broadcast; A's
 * is given by alphaMode: aAlpha's channel, the layer aAlphaLayer selects read per channel, or aLayer's
 * own alpha channel falling back to aAlpha's.
 **/
struct TestAlphaFoldOp {
    struct Params {
        TestAlphaMode mode;
        bool foldA2;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStagedAlphaFoldOpPluginID,
        .label = "Test Staged Alpha Fold Op",
        .grouping = "Other/Test",
        .inputs = kAlphaFoldInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Composite,
    };

    KnobIntWPtr mode;
    KnobBoolWPtr foldA2;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        KnobIntPtr alphaMode = AppManager::createKnob<KnobInt>(holder, std::string("Alpha Mode"));
        alphaMode->setName(kStagedParamAlphaMode);
        alphaMode->setDefaultValue((int)TestAlphaMode::Channel);
        page->addKnob(alphaMode);
        mode = alphaMode;

        KnobBoolPtr a2 = AppManager::createKnob<KnobBool>(holder, std::string("Fold A2"));
        a2->setName(kStagedParamFoldA2);
        a2->setDefaultValue(false);
        page->addKnob(a2);
        foldA2 = a2;

        KnobLayerSelectPtr aLayer = AppManager::createKnob<KnobLayerSelect>(holder, std::string("A Layer"), 1, false);
        aLayer->setName(kStagedParamALayer);
        page->addKnob(aLayer);

        KnobLayerSelectPtr aAlphaLayer = AppManager::createKnob<KnobLayerSelect>(holder, std::string("A Alpha Layer"), 1, false);
        aAlphaLayer->setName(kStagedParamAAlphaLayer);
        page->addKnob(aAlphaLayer);

        KnobChannelSelectPtr aAlpha = AppManager::createKnob<KnobChannelSelect>(holder, std::string("A Alpha"), 1, false);
        aAlpha->setName(kStagedParamAAlpha);
        page->addKnob(aAlpha);

        KnobChannelSelectPtr bAlpha = AppManager::createKnob<KnobChannelSelect>(holder, std::string("B Alpha"), 1, false);
        bAlpha->setName(kStagedParamBAlpha);
        page->addKnob(bAlpha);
    }

    Params bake(const KnobReader& reader) const
    {
        return Params { (TestAlphaMode)reader.valueOr(mode, (int)TestAlphaMode::Channel), reader.valueOr(foldA2, false) };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    CompositeInputs inputs(const Params& params,
                           const RouteContext& /*context*/) const
    {
        CompositeInputs folded;

        folded.base = CompositeInput { 0, {}, CompositeAlpha::channel(kStagedParamBAlpha) };
        CompositeAlpha aAlpha = CompositeAlpha::channel(kStagedParamAAlpha);
        if (params.mode == TestAlphaMode::Layer) {
            aAlpha = CompositeAlpha::layer(kStagedParamAAlphaLayer);
        } else if (params.mode == TestAlphaMode::ReadLayer) {
            aAlpha = CompositeAlpha::readLayer(kStagedParamAAlpha);
        }
        folded.add(CompositeInput { 1, kStagedParamALayer, aAlpha });
        if (params.foldA2) {
            folded.add(CompositeInput { 2, {}, CompositeAlpha::channel(kStagedParamAAlpha) });
        }

        return folded;
    }

    void step(const Params& /*params*/,
              const RowIO& io) const
    {
        const int nComps = io.nComps;
        const float* result = io.src[0];
        const float* input = io.src[1];
        float* dst = io.dst;
        float* dstAlpha = io.dstAlpha;

        for (int x = 0; x < io.width; ++x, result += nComps, input += nComps, dst += nComps, dstAlpha += nComps) {
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                    const float resultAlpha = io.alpha[0].at(x, c);
                    const float inputAlpha = io.alpha[1].at(x, c);
                    dst[c] = input[c] * (1.f - resultAlpha) + result[c] * (1.f - inputAlpha);
                    dstAlpha[c] = inputAlpha * (1.f - resultAlpha) + resultAlpha * (1.f - inputAlpha);
                }
            }
        }
    }
};

static_assert(CompositeOp<TestAlphaFoldOp>);

typedef ImageNode<TestFoldOp> StagedFoldOp;
typedef ImageNode<TestRoutedPointOp> StagedRoutedPointOp;
typedef ImageNode<TestAlphaFoldOp> StagedAlphaFoldOp;

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
        registerNode<StagedFoldOp>();
        registerNode<StagedRoutedPointOp>();
        registerNode<StagedAlphaFoldOp>();
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

    // The first count of the fold's inputs, each at its own origin so that no two read alike, the
    // second far enough off to read zero over part of the window, and a mask offset elsewhere with
    // a mix of 0.5: the halve-and-add fold, blended by mask alpha x mix with blendInput (the base
    // for -1). At a mix of 0 the node is an identity of its blend input.
    void expectFold(int count,
                    int blendInput)
    {
        const std::string what = "fold of " + std::to_string(count) + " blending with input " + std::to_string(blendInput);
        const int origins[kFoldMaxInputs][2] = { { 0, 0 }, { 10, 6 }, { 1, 2 }, { 3, 1 }, { 2, 3 } };
        NodePtr op = createNode(QString::fromUtf8(kStagedFoldOpPluginID));
        ASSERT_TRUE(bool(op)) << what;
        std::vector<NodePtr> inputs;
        for (int k = 0; k < kFoldMaxInputs; ++k) {
            NodePtr input = createSource(origins[k][0], origins[k][1]);
            ASSERT_TRUE(bool(input)) << what;
            connectNodes(input, op, foldInput(k), true);
            inputs.push_back(input);
        }
        NodePtr mask = createSource(5, 20);
        ASSERT_TRUE(bool(mask)) << what;
        connectNodes(mask, op, kFoldMaskInput, true);
        ASSERT_TRUE(setKnobValues(op, "enableMask_Mask", { 1. })) << what;
        ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, { kMix })) << what;
        ASSERT_TRUE(setKnobValues(op, kStagedParamFoldCount, { (double)count })) << what;
        ASSERT_TRUE(setKnobValues(op, kStagedParamBlendInput, { (double)blendInput })) << what;

        const RectI window = paritySourceWindow(inputs[0], kTime, 0);
        ASSERT_FALSE(window.isNull()) << what;
        std::vector<std::vector<float>> pixels(kFoldMaxInputs);
        for (int k = 0; k < kFoldMaxInputs; ++k) {
            ASSERT_TRUE(renderRGBA(inputs[k], window, &pixels[k])) << what;
        }
        std::vector<float> maskPixels;
        std::vector<float> got;
        ASSERT_TRUE(renderRGBA(mask, window, &maskPixels)) << what;
        ASSERT_TRUE(renderRGBA(op, window, &got)) << what;

        int blendK = 0;
        for (int k = 0; k < kFoldMaxInputs; ++k) {
            if (foldInput(k) == blendInput) {
                blendK = k;
            }
        }
        const std::vector<float>& under = pixels[blendK];

        bool offsetMissesSomewhere = false;
        std::vector<float> want(pixels[0].size());
        const float mix = (float)kMix;
        for (std::size_t p = 0; p + 3 < want.size(); p += 4) {
            const float alpha = maskPixels[p + 3] * mix;
            offsetMissesSomewhere = offsetMissesSomewhere || ((pixels[1][p] == 0.f) && (pixels[1][p + 3] == 0.f) && (pixels[0][p] != 0.f));
            for (int c = 0; c < 4; ++c) {
                float v = pixels[0][p + c];
                for (int k = 1; k < count; ++k) {
                    v = v * 0.5f + pixels[k][p + c];
                }
                if (alpha == 0.f) {
                    v = under[p + c];
                } else if (alpha != 1.f) {
                    v = v * alpha + (1.f - alpha) * under[p + c];
                }
                want[p + c] = v;
            }
        }
        EXPECT_TRUE(offsetMissesSomewhere) << what;
        expectSame(want, got, what);

        ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, { 0. })) << what;
        bool identity = false;
        const int inputNb = identityInput(op, window, &identity);
        EXPECT_TRUE(identity) << what;
        EXPECT_EQ((blendInput >= 0) ? blendInput : foldInput(0), inputNb) << what;
    }

    // The pixels of node's layer over window at full scale, as many floats per pixel as the layer
    // has channels, zero outside its region of definition.
    static bool renderLayer(const NodePtr& node,
                            const RectI& window,
                            const ImageLayerDesc& layer,
                            std::vector<float>* pixels)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, layer), &planes, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1) || (planes[0].channels.size() != (std::size_t)layer.getNumComponents())) {
            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }

    static void setLayerKnob(const NodePtr& node,
                             const std::string& name,
                             const std::string& layerID)
    {
        KnobLayerSelectPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobLayerSelect>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setLayer(layerID);
    }

    static void setChannelKnob(const NodePtr& node,
                               const std::string& name,
                               const std::string& channel)
    {
        KnobChannelSelectPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChannelSelect>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->set(channel);
    }

    struct AlphaFold {
        NodePtr op;
        NodePtr b;
        NodePtr a;
        NodePtr a2;
        RectI window;
    };

    // TestAlphaFoldOp with B at the origin, A, which also carries the spec layer, offset so that it
    // reads zero over part of the window, and A2 offset elsewhere. B's alpha is rgba.A.
    void createAlphaFold(AlphaFold* fold)
    {
        fold->op = createNode(QString::fromUtf8(kStagedAlphaFoldOpPluginID));
        ASSERT_TRUE(bool(fold->op));
        fold->b = createSource(0, 0);
        fold->a = createSource(10, 6);
        fold->a2 = createSource(1, 2);
        ASSERT_TRUE(fold->b && fold->a && fold->a2);
        setParitySourceExtraLayer(fold->a, true);
        connectNodes(fold->b, fold->op, 0, true);
        connectNodes(fold->a, fold->op, 1, true);
        connectNodes(fold->a2, fold->op, 2, true);
        setChannelKnob(fold->op, kStagedParamBAlpha, std::string(kNatronColorViewRGBA) + ".A");
        fold->window = paritySourceWindow(fold->b, kTime, 0);
        ASSERT_FALSE(fold->window.isNull());
    }

    // Renders fold's op with A read in aLayerID (rgba or spec), A's alpha as mode gives it from
    // aAlphaChannel (an rgba channel name) or aAlphaLayerID, and with or without A2, and checks it
    // against the xor fold computed by hand.
    void expectAlphaFold(const AlphaFold& fold,
                         TestAlphaMode mode,
                         const std::string& aLayerID,
                         const std::string& aAlphaChannel,
                         const std::string& aAlphaLayerID,
                         bool foldA2)
    {
        const std::string what = "alpha mode " + std::to_string((int)mode) + ", A in " + aLayerID + ", alpha " + aAlphaChannel + "/" + aAlphaLayerID + (foldA2 ? ", with A2" : "");
        ASSERT_TRUE(setKnobValues(fold.op, kStagedParamAlphaMode, { (double)mode })) << what;
        ASSERT_TRUE(setKnobValues(fold.op, kStagedParamFoldA2, { foldA2 ? 1. : 0. })) << what;
        setLayerKnob(fold.op, kStagedParamALayer, aLayerID);
        setLayerKnob(fold.op, kStagedParamAAlphaLayer, aAlphaLayerID);
        setChannelKnob(fold.op, kStagedParamAAlpha, std::string(kNatronColorViewRGBA) + "." + aAlphaChannel);
        const int aAlphaIndex = (aAlphaChannel == "A") ? 3 : (aAlphaChannel == "B") ? 2
            : (aAlphaChannel == "G")                                                ? 1
                                                                                    : 0;

        std::vector<float> b, a, aSpec, a2, got;
        ASSERT_TRUE(renderRGBA(fold.b, fold.window, &b)) << what;
        ASSERT_TRUE(renderRGBA(fold.a, fold.window, &a)) << what;
        ASSERT_TRUE(renderLayer(fold.a, fold.window, paritySourceExtraLayer(), &aSpec)) << what;
        ASSERT_TRUE(renderRGBA(fold.a2, fold.window, &a2)) << what;
        ASSERT_TRUE(renderRGBA(fold.op, fold.window, &got)) << what;

        const bool aInSpec = (aLayerID == kParitySourceExtraLayerID);
        std::vector<float> want(b.size());
        bool alphaVaries = false;
        for (std::size_t p = 0; (p + 1) * 4 <= want.size(); ++p) {
            float result[4];
            float resultAlpha[4];
            float input[4];
            float inputAlpha[4];
            for (int c = 0; c < 4; ++c) {
                result[c] = b[p * 4 + c];
                resultAlpha[c] = b[p * 4 + 3];
                // A three-channel layer is read as RGB, its alpha reading zero.
                input[c] = aInSpec ? ((c < 3) ? aSpec[p * 3 + c] : 0.f) : a[p * 4 + c];
                switch (mode) {
                case TestAlphaMode::Channel:
                    inputAlpha[c] = a[p * 4 + aAlphaIndex];
                    break;
                case TestAlphaMode::Layer:
                    inputAlpha[c] = (c < 3) ? aSpec[p * 3 + c] : 0.f;
                    break;
                case TestAlphaMode::ReadLayer:
                    inputAlpha[c] = aInSpec ? a[p * 4 + aAlphaIndex] : a[p * 4 + 3];
                    break;
                }
            }
            alphaVaries = alphaVaries || (inputAlpha[0] != inputAlpha[2]) || (resultAlpha[0] != inputAlpha[0]);
            for (int c = 0; c < 4; ++c) {
                const float v = input[c] * (1.f - resultAlpha[c]) + result[c] * (1.f - inputAlpha[c]);
                resultAlpha[c] = inputAlpha[c] * (1.f - resultAlpha[c]) + resultAlpha[c] * (1.f - inputAlpha[c]);
                result[c] = v;
            }
            if (foldA2) {
                for (int c = 0; c < 4; ++c) {
                    const float inputA2 = a2[p * 4 + c];
                    const float alphaA2 = a2[p * 4 + aAlphaIndex];
                    result[c] = inputA2 * (1.f - resultAlpha[c]) + result[c] * (1.f - alphaA2);
                }
            }
            for (int c = 0; c < 4; ++c) {
                want[p * 4 + c] = result[c];
            }
        }
        EXPECT_TRUE(alphaVaries) << what;
        expectSame(want, got, what);
    }
};

// The base alone: the result is the base row, blended with itself.
TEST_F(StagedCompositeTest, FoldOfTheBaseAlone)
{
    expectFold(1, -1);
}

// Two inputs, the second offset, in one step, blended with the base.
TEST_F(StagedCompositeTest, FoldOfTwoInputsUnderMaskAndMix)
{
    expectFold(2, -1);
}

// Two inputs blended with an input the fold does not read.
TEST_F(StagedCompositeTest, FoldOfTwoInputsBlendingWithAnUnfoldedInput)
{
    expectFold(2, foldInput(4));
}

// Three inputs blended with the third.
TEST_F(StagedCompositeTest, FoldOfThreeInputsBlendingWithAFoldedInput)
{
    expectFold(3, foldInput(2));
}

// Five inputs, more than a fixed list of four would hold, under mask and mix.
TEST_F(StagedCompositeTest, FoldOfFiveInputsUnderMaskAndMix)
{
    expectFold(5, -1);
}

// Five inputs blended with the offset one.
TEST_F(StagedCompositeTest, FoldOfFiveInputsBlendingWithTheOffsetInput)
{
    expectFold(5, foldInput(1));
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

// A folded over B in a layer the op selects, spec, with a universal alpha from rgba.A, alone and
// followed by A2, which then reads the result's alpha the first step wrote.
TEST_F(StagedCompositeTest, AlphaFoldReadsASelectedLayerWithAUniversalAlpha)
{
    AlphaFold fold;
    createAlphaFold(&fold);
    ASSERT_TRUE(bool(fold.op));
    expectAlphaFold(fold, TestAlphaMode::Channel, kParitySourceExtraLayerID, "A", kParitySourceExtraLayerID, false);
    expectAlphaFold(fold, TestAlphaMode::Channel, kParitySourceExtraLayerID, "A", kParitySourceExtraLayerID, true);
    expectAlphaFold(fold, TestAlphaMode::Channel, kNatronColorViewRGBA, "R", kParitySourceExtraLayerID, true);
}

// A's alpha is a layer read per channel: spec's channel i is the alpha of A's channel i.
TEST_F(StagedCompositeTest, AlphaFoldReadsAPerChannelAlphaLayer)
{
    AlphaFold fold;
    createAlphaFold(&fold);
    ASSERT_TRUE(bool(fold.op));
    expectAlphaFold(fold, TestAlphaMode::Layer, kParitySourceExtraLayerID, "A", kParitySourceExtraLayerID, false);
    expectAlphaFold(fold, TestAlphaMode::Layer, kParitySourceExtraLayerID, "A", kParitySourceExtraLayerID, true);
}

// A's alpha is its read layer's own alpha channel, and the universal channel for a layer without
// one.
TEST_F(StagedCompositeTest, AlphaFoldReadsTheReadLayersOwnAlpha)
{
    AlphaFold fold;
    createAlphaFold(&fold);
    ASSERT_TRUE(bool(fold.op));
    expectAlphaFold(fold, TestAlphaMode::ReadLayer, kNatronColorViewRGBA, "R", kParitySourceExtraLayerID, true);
    expectAlphaFold(fold, TestAlphaMode::ReadLayer, kParitySourceExtraLayerID, "R", kParitySourceExtraLayerID, true);
}

// A per-channel alpha layer with another channel count than the layer it is the alpha of fails the
// render with an error naming both; the error is withdrawn once they match.
TEST_F(StagedCompositeTest, AlphaLayerChannelCountMismatchIsARenderError)
{
    AlphaFold fold;
    createAlphaFold(&fold);
    ASSERT_TRUE(bool(fold.op));
    ASSERT_TRUE(setKnobValues(fold.op, kStagedParamAlphaMode, { (double)TestAlphaMode::Layer }));
    setLayerKnob(fold.op, kStagedParamALayer, kNatronColorViewRGBA);
    setLayerKnob(fold.op, kStagedParamAAlphaLayer, kParitySourceExtraLayerID);

    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(fold.op, kTime, ViewIdx(0), 0, fold.window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error));
    QString message;
    int type = 0;
    fold.op->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8(kParitySourceExtraLayerID))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(kNatronColorViewRGBA), Qt::CaseInsensitive)) << message.toStdString();

    setLayerKnob(fold.op, kStagedParamALayer, kParitySourceExtraLayerID);
    std::vector<float> got;
    EXPECT_TRUE(renderRGBA(fold.op, fold.window, &got));
    message.clear();
    fold.op->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
}
