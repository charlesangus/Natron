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
#include <atomic>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
#include <memory>
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
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
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
#include "Engine/Gpu/GpuKernel.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"

#include "staged_test_compositealpha_xor_spirv.h"
#include "staged_test_pixelwrap_scale_spirv.h"
#endif

#include <ofxImageEffect.h>

#define kGpuScaleTestOpPluginID "test.natron.built-in.GpuScaleTestOp"
#define kGpuAlphaFoldTestOpPluginID "test.natron.built-in.GpuAlphaFoldTestOp"
#define kAlphaFoldParamMode "alphaMode"
#define kAlphaFoldParamFoldA2 "foldA2"
#define kAlphaFoldParamALayer "aLayer"
#define kAlphaFoldParamAAlpha "aAlpha"
#define kAlphaFoldParamAAlphaLayer "aAlphaLayer"
#define kAlphaFoldParamBAlpha "bAlpha"

NATRON_NAMESPACE_ENTER

namespace {

// The kernel GpuScaleTestOp names, as registered on the runtime the test renders with; -1 keeps
// the node on the CPU.
std::atomic<int> gScaleKernel(-1);

// When set, the op hands over a parameter block over the 64 KB limit.
std::atomic<bool> gOversizeParams(false);

const std::size_t kMaxParamBytes = 65536;

struct ScaleParams {
    float factor;
};

constexpr InputSpec kInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief A Point op scaling its processed channels by 2, with a mask input and the host
 * "(Un)premult by" selector, R, G and B processed by default. Its GPU kernel is the test kernel
 * pixelwrap_scale, so the GPU path is the host's alone.
 **/
struct GpuScaleTestOp {
    struct Params {
        int unused;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kGpuScaleTestOpPluginID,
        .label = "Test GPU Scale Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params { 0 };
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
                    dst[c] = 2.f * src[c];
                }
            }
        }
    }

    // pixelwrap_scale ignores its parameters; they are handed over so the upload path runs.
    GpuOpRef gpu(const Params& /*params*/) const
    {
        const int kernel = gScaleKernel.load();

        if (kernel < 0) {
            return GpuOpRef::none();
        }
        if (gOversizeParams.load()) {
            return GpuOpRef::packed(kernel, std::vector<std::uint32_t>(kMaxParamBytes / sizeof(std::uint32_t) + 1));
        }
        ScaleParams p;
        p.factor = 2.f;

        return GpuOpRef::compiled(kernel, p);
    }
};

static_assert(PointOp<GpuScaleTestOp>);

typedef ImageNode<GpuScaleTestOp> GpuScaleTestNode;

// The kernel GpuAlphaFoldTestOp names, as GpuScaleTestOp's; -1 keeps the node on the CPU.
std::atomic<int> gAlphaFoldKernel(-1);

// How GpuAlphaFoldTestOp's A input gets its alpha, in the order of the alphaMode knob.
enum class AlphaFoldMode {
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
 * @brief The Composite op of StagedComposite_Test's TestAlphaFoldOp with a GPU kernel: A, read in
 * the layer aLayer selects, folded over B and, with foldA2, A2 over that, each step the Porter-Duff
 * xor through both alphas. B's and A2's alphas are the channel bAlpha and aAlpha select; A's is
 * aAlpha's channel, the layer aAlphaLayer selects read per channel, or aLayer's own alpha by
 * alphaMode. Its kernel is the test entry compositealpha_xor.
 **/
struct GpuAlphaFoldTestOp {
    struct Params {
        AlphaFoldMode mode;
        bool foldA2;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kGpuAlphaFoldTestOpPluginID,
        .label = "Test GPU Alpha Fold Op",
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
        alphaMode->setName(kAlphaFoldParamMode);
        alphaMode->setDefaultValue((int)AlphaFoldMode::Channel);
        page->addKnob(alphaMode);
        mode = alphaMode;

        KnobBoolPtr a2 = AppManager::createKnob<KnobBool>(holder, std::string("Fold A2"));
        a2->setName(kAlphaFoldParamFoldA2);
        a2->setDefaultValue(false);
        page->addKnob(a2);
        foldA2 = a2;

        KnobLayerSelectPtr aLayer = AppManager::createKnob<KnobLayerSelect>(holder, std::string("A Layer"), 1, false);
        aLayer->setName(kAlphaFoldParamALayer);
        page->addKnob(aLayer);

        KnobLayerSelectPtr aAlphaLayer = AppManager::createKnob<KnobLayerSelect>(holder, std::string("A Alpha Layer"), 1, false);
        aAlphaLayer->setName(kAlphaFoldParamAAlphaLayer);
        page->addKnob(aAlphaLayer);

        KnobChannelSelectPtr aAlpha = AppManager::createKnob<KnobChannelSelect>(holder, std::string("A Alpha"), 1, false);
        aAlpha->setName(kAlphaFoldParamAAlpha);
        page->addKnob(aAlpha);

        KnobChannelSelectPtr bAlpha = AppManager::createKnob<KnobChannelSelect>(holder, std::string("B Alpha"), 1, false);
        bAlpha->setName(kAlphaFoldParamBAlpha);
        page->addKnob(bAlpha);
    }

    Params bake(const KnobReader& reader) const
    {
        return Params { (AlphaFoldMode)reader.valueOr(mode, (int)AlphaFoldMode::Channel), reader.valueOr(foldA2, false) };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    CompositeInputs inputs(const Params& params,
                           const RouteContext& /*context*/) const
    {
        CompositeInputs folded;

        folded.base = CompositeInput { 0, {}, CompositeAlpha::channel(kAlphaFoldParamBAlpha) };
        CompositeAlpha aAlpha = CompositeAlpha::channel(kAlphaFoldParamAAlpha);
        if (params.mode == AlphaFoldMode::Layer) {
            aAlpha = CompositeAlpha::layer(kAlphaFoldParamAAlphaLayer);
        } else if (params.mode == AlphaFoldMode::ReadLayer) {
            aAlpha = CompositeAlpha::readLayer(kAlphaFoldParamAAlpha);
        }
        folded.add(CompositeInput { 1, kAlphaFoldParamALayer, aAlpha });
        if (params.foldA2) {
            folded.add(CompositeInput { 2, {}, CompositeAlpha::channel(kAlphaFoldParamAAlpha) });
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

    GpuOpRef gpu(const Params& /*params*/) const
    {
        const int kernel = gAlphaFoldKernel.load();

        return (kernel < 0) ? GpuOpRef::none() : GpuOpRef::packed(kernel, {});
    }
};

static_assert(GpuCompositeOp<GpuAlphaFoldTestOp>);

typedef ImageNode<GpuAlphaFoldTestOp> GpuAlphaFoldTestNode;

const double kTime = 1.;

template <typename NODE>
void
registerTestNode()
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
registerTestNodesOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerTestNode<GpuScaleTestNode>();
        registerTestNode<GpuAlphaFoldTestNode>();
    });
}

int
inputNamed(const NodePtr& node,
           const std::string& label)
{
    for (int i = 0; i < node->getNInputs(); ++i) {
        if (node->getInputLabel(i) == label) {
            return i;
        }
    }

    return -1;
}

ImageLayerDesc
outputColorLayer(const NodePtr& node)
{
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            return *it;
        }
    }

    return ImageLayerDesc();
}

bool
renderColorPlane(const NodePtr& node,
                 const RectI& window,
                 RenderedPlane* plane)
{
    const ImageLayerDesc color = outputColorLayer(node);
    std::list<ImageLayerDesc> layers;

    EXPECT_GT(color.getNumComponents(), 0);
    layers.push_back(color);
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// `plane` in `layer`'s layout, converted as the region engine converts an input it hands over in
// another layout.
RenderedPlane
inLayout(const RenderedPlane& plane,
         const ImageLayerDesc& layer)
{
    const int srcNComps = (int)plane.channels.size();
    const int dstNComps = layer.getNumComponents();
    if (srcNComps == dstNComps) {
        return plane;
    }
    const RectD rod = plane.window.toCanonical_noClipping(0, 1.);
    Image src(plane.layer, rod, plane.window, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
    Image dst(layer, rod, plane.window, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
    {
        Image::WriteAccess access(&src);
        for (int y = plane.window.y1; y < plane.window.y2; ++y) {
            const float* row = &plane.pixels[(std::size_t)(y - plane.window.y1) * plane.window.width() * srcNComps];
            float* pix = (float*)access.pixelAt(plane.window.x1, y);
            std::copy(row, row + (std::size_t)plane.window.width() * srcNComps, pix);
        }
    }
    src.convertToFormat(plane.window, 3, false, &dst);

    RenderedPlane out;
    out.layer = layer;
    out.window = plane.window;
    for (int c = 0; c < dstNComps; ++c) {
        out.channels.push_back(layer.getChannels()[c]);
    }
    out.pixels.resize((std::size_t)plane.window.area() * dstNComps);
    Image::ReadAccess access(&dst);
    for (int y = plane.window.y1; y < plane.window.y2; ++y) {
        const float* pix = (const float*)access.pixelAt(plane.window.x1, y);
        std::copy(pix, pix + (std::size_t)plane.window.width() * dstNComps, &out.pixels[(std::size_t)(y - plane.window.y1) * plane.window.width() * dstNComps]);
    }

    return out;
}

// The query the placement builds for a task rendering `plane` of op (makeGpuQueryForTask()).
GpuQuery
queryFor(const NodePtr& op,
         const ImageLayerDesc& plane)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.plane = plane;
    q.nComps = plane.getNumComponents();
    q.bitDepth = eImageBitDepthFloat;
    const int maskInput = effect->getMaskInput();
    q.hasMask = (maskInput >= 0) && effect->getInput(maskInput) && effect->isMaskEnabled(maskInput);
    if (q.hasMask) {
        ImageLayerDesc maskLayer;
        int maskChannel = -1;
        q.maskInPlane = !effect->resolveInputPlaneForRender(maskInput, kTime, ViewIdx(0), &maskLayer, &maskChannel) || (maskChannel < 0) || (Node::getUnPremultSkipChannel(plane, maskLayer, maskChannel) >= 0);
    }
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(effect->getKnobByName(kOfxMixParamName));
    q.mix = mix ? mix->getValueAtTime(kTime, 0, ViewIdx(0)) : 1.;
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), plane);
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }
    KnobChannelSelectPtr unPremultBy = op->getUnPremultBySelector();
    if (unPremultBy && !unPremultBy->isNone() && effect->getInput(0)) {
        std::list<ImageLayerDesc> available;
        effect->getAvailableLayers(kTime, ViewIdx(0), 0, &available);
        ImageLayerDesc divisorLayer;
        const int divisorChannel = op->getUnPremultChannel(available, &divisorLayer);
        if (divisorLayer.getNumComponents() > 0) {
            q.unPremultChannel = divisorChannel;
            q.unPremultInPlane = (divisorChannel >= 0) && (Node::getUnPremultSkipChannel(plane, divisorLayer, divisorChannel) >= 0);
        }
    }

    return q;
}

// The bound where a value is divided by the "(Un)premult by" channel: Vulkan's division is 2.5
// ULP, so the result may differ by 3 ULP of the largest term, the multiplied-back 2 * src.
bool
withinDivideBound(float want,
                  float got,
                  float src)
{
    if (std::isnan(want) || std::isnan(got)) {
        return std::isnan(want) && std::isnan(got);
    }
    if (want == got) {
        return true;
    }
    const double largest = std::max(std::fabs((double)want), 2. * std::fabs((double)src));

    return std::fabs((double)want - (double)got) <= 3. * FLT_EPSILON * largest;
}

GpuParityImage
imageOf(const RenderedPlane& plane)
{
    GpuParityImage image;

    image.bounds = plane.window;
    image.nComps = (int)plane.channels.size();
    image.pixels = plane.pixels;

    return image;
}

#ifdef NATRON_HAVE_GPU
gpu::GpuKernelDesc
scaleKernelDesc()
{
    gpu::GpuKernelDesc d;

    d.spirv = staged_test_pixelwrap_scale_spirv;
    d.spirvWords = staged_test_pixelwrap_scale_spirv_words;
    d.entry = staged_test_pixelwrap_scale_entry;
    d.storageBufferCount = staged_test_pixelwrap_scale_storage_buffers;
    d.pushConstantBytes = staged_test_pixelwrap_scale_push_bytes;
    d.groupSize = { staged_test_pixelwrap_scale_group_size[0], staged_test_pixelwrap_scale_group_size[1], staged_test_pixelwrap_scale_group_size[2] };

    return d;
}

gpu::GpuKernelDesc
alphaFoldKernelDesc()
{
    gpu::GpuKernelDesc d;

    d.spirv = staged_test_compositealpha_xor_spirv;
    d.spirvWords = staged_test_compositealpha_xor_spirv_words;
    d.entry = staged_test_compositealpha_xor_entry;
    d.storageBufferCount = staged_test_compositealpha_xor_storage_buffers;
    d.pushConstantBytes = staged_test_compositealpha_xor_push_bytes;
    d.groupSize = { staged_test_compositealpha_xor_group_size[0], staged_test_compositealpha_xor_group_size[1], staged_test_compositealpha_xor_group_size[2] };

    return d;
}
#endif

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class StagedImageEffectGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerTestNodesOnce();
        BaseTest::SetUp();
    }

    virtual void TearDown() OVERRIDE
    {
        gScaleKernel = -1;
        gAlphaFoldKernel = -1;
        gOversizeParams = false;
        BaseTest::TearDown();
    }

    NodePtr createSource(const std::string& components)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, components);
        }

        return source;
    }

    NodePtr createOp(const NodePtr& source)
    {
        NodePtr op = createNode(QString::fromUtf8(kGpuScaleTestOpPluginID));

        EXPECT_TRUE(bool(op));
        if (op && source) {
            connectNodes(source, op, 0, true);
        }

        return op;
    }

    // A parity source at (originX, originY) on the op's mask input, its alpha selected; the mask
    // stays disabled until enableMask() turns it on.
    NodePtr connectMask(const NodePtr& op,
                        int originX,
                        int originY,
                        const std::string& components = "rgba")
    {
        NodePtr mask = createSource(components);

        if (!mask) {
            return mask;
        }
        setParitySourceOrigin(mask, originX, originY);
        const int maskInput = inputNamed(op, "Mask");
        EXPECT_GE(maskInput, 0);
        connectNodes(mask, op, maskInput, true);
        KnobChannelSelect* channel = dynamic_cast<KnobChannelSelect*>(op->getKnobByName("maskChannel_Mask").get());
        EXPECT_TRUE(channel != NULL);
        if (channel) {
            channel->set("rgba.A");
        }

        return mask;
    }

    static void enableMask(const NodePtr& op,
                           bool enabled)
    {
        EXPECT_TRUE(setKnobValues(op, "enableMask_Mask", std::vector<double>(1, enabled ? 1. : 0.)));
    }

    static void setUnPremultBy(const NodePtr& op,
                               const std::string& channel)
    {
        KnobChannelSelect* selector = dynamic_cast<KnobChannelSelect*>(op->getKnobByName(kUnPremultByKnobName).get());

        ASSERT_TRUE(selector != NULL);
        if (channel.empty()) {
            selector->setNone();
        } else {
            selector->set(channel);
        }
    }

    static KnobChannelSetPtr channelSetOf(const NodePtr& op)
    {
        KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(op->getKnobByName(kNodeParamChannelSet));

        EXPECT_TRUE(bool(channels));

        return channels;
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    struct AlphaFold {
        NodePtr op;
        NodePtr b;
        NodePtr a;
        NodePtr a2;
        RectI window;
    };

    // GpuAlphaFoldTestOp with B at the origin, A, which also carries the spec layer, offset so that
    // it reads zero over part of the window, and A2 offset elsewhere. B's alpha is rgba.A.
    void createAlphaFold(AlphaFold* fold)
    {
        fold->op = createNode(QString::fromUtf8(kGpuAlphaFoldTestOpPluginID));
        ASSERT_NE(nullptr, fold->op);
        fold->b = createSource("rgba");
        fold->a = createSource("rgba");
        fold->a2 = createSource("rgba");
        ASSERT_NE(nullptr, fold->b);
        ASSERT_NE(nullptr, fold->a);
        ASSERT_NE(nullptr, fold->a2);
        setParitySourceOrigin(fold->a, 10, 6);
        setParitySourceOrigin(fold->a2, 1, 2);
        setParitySourceExtraLayer(fold->a, true);
        connectNodes(fold->b, fold->op, 0, true);
        connectNodes(fold->a, fold->op, 1, true);
        connectNodes(fold->a2, fold->op, 2, true);
        setChannelKnob(fold->op, kAlphaFoldParamBAlpha, std::string(kNatronColorViewRGBA) + ".A");
        fold->window = paritySourceWindow(fold->b, kTime, 0);
        ASSERT_FALSE(fold->window.isNull());
    }

    // Sets fold's op to read A in aLayerID, its alpha as mode gives it from aAlphaChannel (an rgba
    // channel name) or aAlphaLayerID, with or without A2.
    static void setAlphaFold(const AlphaFold& fold,
                             AlphaFoldMode mode,
                             const std::string& aLayerID,
                             const std::string& aAlphaChannel,
                             const std::string& aAlphaLayerID,
                             bool foldA2)
    {
        ASSERT_TRUE(setKnobValues(fold.op, kAlphaFoldParamMode, { (double)mode }));
        ASSERT_TRUE(setKnobValues(fold.op, kAlphaFoldParamFoldA2, { foldA2 ? 1. : 0. }));
        setLayerKnob(fold.op, kAlphaFoldParamALayer, aLayerID);
        setLayerKnob(fold.op, kAlphaFoldParamAAlphaLayer, aAlphaLayerID);
        setChannelKnob(fold.op, kAlphaFoldParamAAlpha, std::string(kNatronColorViewRGBA) + "." + aAlphaChannel);
    }

    static void setLayerKnob(const NodePtr& node,
                             const std::string& name,
                             const std::string& layerID)
    {
        KnobLayerSelectPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobLayerSelect>(name);
        ASSERT_NE(nullptr, knob) << name;
        knob->setLayer(layerID);
    }

    static void setChannelKnob(const NodePtr& node,
                               const std::string& name,
                               const std::string& channel)
    {
        KnobChannelSelectPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChannelSelect>(name);
        ASSERT_NE(nullptr, knob) << name;
        knob->set(channel);
    }
};

TEST_F(StagedImageEffectGpuTest, HostAcceptsAPointOpOnlyWithAKernel)
{
    NodePtr op = createOp(createSource("rgba"));
    ASSERT_TRUE(bool(op));
    StagedImageEffect* effect = effectOf(op);
    ASSERT_TRUE(effect != NULL);

    GpuQuery q;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;
    EXPECT_FALSE(effect->canRenderOnGpu(q)) << "no kernel";

    gScaleKernel = 0;
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    for (int nComps = 1; nComps <= 4; ++nComps) {
        GpuQuery c = q;
        c.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(c)) << nComps << " components";
    }
    for (int nComps : { 0, 5 }) {
        GpuQuery c = q;
        c.nComps = nComps;
        EXPECT_FALSE(effect->canRenderOnGpu(c)) << nComps << " components";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf, eImageBitDepthNone }) {
        GpuQuery c = q;
        c.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(c)) << "bit depth " << (int)depth;
    }
    {
        GpuQuery c = q;
        c.hasMask = true;
        c.mix = 0.5;
        c.processChannels[3] = false;
        EXPECT_EQ(supported, effect->canRenderOnGpu(c)) << "a mask channel outside the plane, read in its own layer";
        c.maskInPlane = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(c)) << "mask, mix and a channel subset";
    }
    {
        GpuQuery c = q;
        c.unPremultChannel = 3;
        c.unPremultInPlane = false;
        EXPECT_FALSE(effect->canRenderOnGpu(c)) << "a divisor outside the plane";
        c.unPremultInPlane = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(c)) << "a divisor in the plane";
    }

    // A point op naming no kernel stays on the CPU however plain its query.
    gScaleKernel = -1;
    ASSERT_TRUE(effect->opKind().has_value());
    ASSERT_EQ(OpKind::Point, *effect->opKind());
    EXPECT_FALSE(effect->canRenderOnGpu(q));
}

#ifdef NATRON_HAVE_GPU

// The host's renderGpu() against the node's own render() over the parity sources: channel
// layouts, channel subsets, masks (partly covering, inverted), mix and "(Un)premult by" channels
// whose values include exact 0, negatives and small ramps, with a source smaller than the window
// and views at offsets and row strides.
TEST_F(StagedImageEffectGpuTest, RenderGpuMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const int kernel = (int)runtime->runtime().registerKernel(scaleKernelDesc());

    // An alpha source renders in RGBA under the default rows and in alpha under "All", so its
    // source and mask reach renderGpu() converted to a wider and to a narrower layout.
    struct Inputs {
        const char* source;
        const char* mask;
    };
    const Inputs inputsList[] = { { "rgba", "rgba" }, { "rgb", "rgba" }, { "alpha", "rgba" }, { "alpha", "alpha" } };
    const char* const divisors[] = { "", "rgba.A", "rgba.R" };
    const double mixes[] = { 0., 0.5, 1. };
    int nRendered = 0;
    int nDivided = 0;
    int nCase = 0;

    for (const Inputs& in : inputsList) {
        const std::string components = std::string(in.source) + " under " + in.mask;
        NodePtr source = createSource(in.source);
        NodePtr op = createOp(source);
        ASSERT_TRUE(bool(op));
        ASSERT_TRUE(bool(connectMask(op, 8, 4, in.mask)));
        StagedImageEffect* effect = effectOf(op);
        ASSERT_TRUE(effect != NULL);
        const int maskInput = effect->getMaskInput();
        ASSERT_GE(maskInput, 0);
        NodePtr maskNode = op->getInput(maskInput);
        ASSERT_TRUE(bool(maskNode));

        const RectI sourceWindow = paritySourceWindow(source, kTime, 0);
        const RectI window(sourceWindow.x1 - 3, sourceWindow.y1 - 2, sourceWindow.x2 + 4, sourceWindow.y2 + 3);
        RenderedPlane sourceRendered;
        RenderedPlane sourceOverWindowRendered;
        RenderedPlane maskRendered;
        ASSERT_TRUE(renderColorPlane(source, sourceWindow, &sourceRendered));
        ASSERT_TRUE(renderColorPlane(source, window, &sourceOverWindowRendered));
        ASSERT_TRUE(renderColorPlane(maskNode, paritySourceWindow(maskNode, kTime, 0), &maskRendered));

        KnobChannelSetPtr channelSet = channelSetOf(op);
        ASSERT_TRUE(bool(channelSet));
        const std::vector<ChannelSetRow> defaultRows = channelSet->getRows();

        for (int all = 0; all < 2; ++all) {
            if (all) {
                channelSet->setAll();
            } else {
                channelSet->setRows(defaultRows);
            }
            // The plane the engine renders follows the channel rows: "All" keeps the stream's own
            // layout, the default rows the clip layout. Every input reaches renderGpu() in it.
            const ImageLayerDesc plane = outputColorLayer(op);
            const int nComps = plane.getNumComponents();
            if (std::string(in.source) == "alpha") {
                ASSERT_EQ(all ? 1 : 4, nComps) << components;
            } else {
                ASSERT_EQ((int)sourceRendered.channels.size(), nComps) << components;
            }
            const RenderedPlane sourcePlane = inLayout(sourceRendered, plane);
            const RenderedPlane sourceOverWindow = inLayout(sourceOverWindowRendered, plane);
            const RenderedPlane maskInPlane = inLayout(maskRendered, plane);
            for (int maskMode = 0; maskMode < 3; ++maskMode) {
                enableMask(op, maskMode != 0);
                ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, maskMode == 2 ? 1. : 0.)));
                for (double mix : mixes) {
                    ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, mix)));
                    for (const char* divisor : divisors) {
                        setUnPremultBy(op, divisor);
                        if (HasFatalFailure()) {
                            return;
                        }
                        const std::string name = components + (all ? " all" : " rgb") + " mask " + std::to_string(maskMode) + " mix " + std::to_string(mix) + " divisor '" + divisor + "'";
                        ++nCase;

                        gScaleKernel = kernel;
                        const GpuQuery query = queryFor(op, plane);
                        EXPECT_EQ(maskMode != 0, query.hasMask) << name;
                        const bool outsidePlane = ((nComps == 3) && (std::string(divisor) == "rgba.A")) || ((nComps == 1) && (std::string(divisor) == "rgba.R"));
                        // The mask selects alpha, which an RGB plane drops.
                        const bool maskOutsidePlane = (maskMode != 0) && (nComps == 3);
                        EXPECT_TRUE(!query.hasMask || (query.maskInPlane == !maskOutsidePlane)) << name;
                        EXPECT_EQ(!outsidePlane, effect->canRenderOnGpu(query)) << name;
                        if (!effect->canRenderOnGpu(query)) {
                            continue;
                        }

                        RenderedPlane cpu;
                        gScaleKernel = -1;
                        ASSERT_TRUE(renderColorPlane(op, window, &cpu)) << name;
                        gScaleKernel = kernel;
                        ASSERT_EQ((std::size_t)nComps, cpu.channels.size()) << name;

                        std::vector<GpuParityImage> inputs(1, imageOf(sourcePlane));
                        // The region engine hands the mask over in the layer its channel was resolved in; a mask
                        // the plane holds may also arrive in the plane's layout.
                        ImageLayerDesc maskLayer;
                        ASSERT_TRUE(effect->getGpuInputLayer(maskInput, plane, kTime, ViewIdx(0), &maskLayer)) << name;
                        GpuParityImage mask = imageOf((maskOutsidePlane || (nCase % 3 == 0)) ? inLayout(maskRendered, maskLayer) : maskInPlane);
                        GpuViewLayout layout;
                        if (nCase % 2) {
                            inputs[0].layout.padFloats = 16;
                            inputs[0].layout.rowPadFloats = 3;
                            mask.layout.padFloats = 5;
                            mask.layout.rowPadFloats = 7;
                            layout.padFloats = 8;
                            layout.rowPadFloats = 5;
                            layout.marginLeft = 3;
                            layout.marginBottom = 2;
                            layout.marginRight = 4;
                            layout.marginTop = 1;
                        }
                        const GpuRunResult run = runRenderGpu(*runtime, *effect, query, inputs, query.hasMask ? &mask : nullptr, window, layout);
                        ASSERT_TRUE(run.ok) << name;
                        EXPECT_EQ(0u, run.writtenOutside) << name;
                        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << name;
                        ++nRendered;

                        const bool divided = query.unPremultChannel >= 0;
                        if (divided) {
                            ++nDivided;
                            std::size_t mismatches = 0;
                            for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
                                if (!withinDivideBound(cpu.pixels[i], run.pixels[i], sourceOverWindow.pixels[i])) {
                                    if (mismatches == 0) {
                                        ADD_FAILURE() << name << ": float " << i << " gpu " << run.pixels[i] << " cpu " << cpu.pixels[i];
                                    }
                                    ++mismatches;
                                }
                            }
                            EXPECT_EQ(0u, mismatches) << name;
                        } else {
                            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::exact());
                            EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
                        }
                    }
                }
            }
        }
    }
    std::printf("base point-op gpu vs cpu: %d of %d cases rendered, %d divided\n", nRendered, nCase, nDivided);
    EXPECT_GT(nDivided, 0);
}

TEST_F(StagedImageEffectGpuTest, RenderGpuRefusesParametersOverTheLimit)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr op = createOp(createSource("rgba"));
    ASSERT_TRUE(bool(op));
    StagedImageEffect* effect = effectOf(op);
    ASSERT_TRUE(effect != NULL);
    gScaleKernel = (int)runtime->runtime().registerKernel(scaleKernelDesc());
    gOversizeParams = true;

    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = runtime->runtime().newRecordContext(context);
    ASSERT_TRUE(bool(created)) << created.message;
    const RectI rect(0, 0, 8, 8);
    GpuRenderArgs args(*context);
    args.query.nComps = 4;
    args.outputRect = rect;
    args.output.buffer = runtime->buffer(0, (std::size_t)rect.area() * 4);
    ASSERT_TRUE(args.output.buffer != nullptr);
    args.output.bounds = rect;
    args.output.rowStrideFloats = rect.width() * 4;
    args.output.nComps = 4;
    EXPECT_FALSE(effect->canRenderOnGpu(args.query));
    EXPECT_EQ(eStatusFailed, effect->renderGpu(args));
}

// The scheduler places the node in a region through makeGpuQueryForTask() and the region engine
// renders it with the host's renderGpu(), masked and divided by alpha too.
TEST_F(StagedImageEffectGpuTest, RunsInARegionAndMatchesTheCpuFrame)
{
    NodePtr source = createSource("rgba");
    NodePtr op = createOp(source);
    ASSERT_TRUE(bool(op));
    ASSERT_TRUE(bool(connectMask(op, 8, 4)));

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane sourcePlane;
    ASSERT_TRUE(renderColorPlane(source, window, &sourcePlane));
    ASSERT_EQ(4u, sourcePlane.channels.size());

    const GpuContextReady registerKernel = [](std::string* error) {
        GpuContext* context = appPTR->getGpuContext();
        gpu::GpuRuntime* gpuRuntime = context ? context->runtime() : nullptr;
        if (!gpuRuntime) {
            *error = "the forced GPU context has no runtime";

            return false;
        }
        gScaleKernel = (int)gpuRuntime->registerKernel(scaleKernelDesc());

        return true;
    };

    for (int masked = 0; masked < 2; ++masked) {
        if (masked) {
            enableMask(op, true);
            ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, 0.5)));
            setUnPremultBy(op, "rgba.A");
            if (HasFatalFailure()) {
                return;
            }
        }
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, op), op, window, kTime, registerKernel);
        gScaleKernel = -1;
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(op)) << "masked " << masked;
        std::map<std::string, int>::const_iterator ran = result.gpuTasksPerNode.find(op->getScriptName_mt_safe());
        EXPECT_TRUE((ran != result.gpuTasksPerNode.end()) && (ran->second > 0)) << "masked " << masked;
        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        ASSERT_EQ(sourcePlane.pixels.size(), result.cpu.size());

        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < result.cpu.size(); ++i) {
            const bool ok = masked ? withinDivideBound(result.cpu[i], result.gpu[i], sourcePlane.pixels[i])
                                   : ParityTolerance::exact().accepts(result.cpu[i], result.gpu[i]);
            if (!ok) {
                if (mismatches == 0) {
                    ADD_FAILURE() << "masked " << masked << ": float " << i << " gpu " << result.gpu[i] << " cpu " << result.cpu[i];
                }
                ++mismatches;
            }
        }
        EXPECT_EQ(0u, mismatches) << "masked " << masked;
    }
}

// A Composite fold reading a selected layer is accepted on the GPU once it names a kernel, whether
// the layer is the rendered plane or another one handed over beside it.
TEST_F(StagedImageEffectGpuTest, HostAcceptsAFoldReadingASelectedLayer)
{
    AlphaFold fold;
    createAlphaFold(&fold);
    if (HasFatalFailure()) {
        return;
    }
    StagedImageEffect* effect = effectOf(fold.op);
    ASSERT_TRUE(effect != NULL);
    const ImageLayerDesc plane = outputColorLayer(fold.op);
    ASSERT_EQ(4, plane.getNumComponents());

    for (const std::string& aLayer : { std::string(kNatronColorViewRGBA), std::string(kParitySourceExtraLayerID) }) {
        setAlphaFold(fold, AlphaFoldMode::Layer, aLayer, "A", aLayer, true);
        if (HasFatalFailure()) {
            return;
        }
        gAlphaFoldKernel = -1;
        EXPECT_FALSE(effect->canRenderOnGpu(queryFor(fold.op, plane))) << aLayer << ", no kernel";
        gAlphaFoldKernel = 0;
        EXPECT_TRUE(effect->canRenderOnGpu(queryFor(fold.op, plane))) << aLayer;
    }

    // An alpha layer with another channel count than its layer fails the render, so stays on the CPU.
    setAlphaFold(fold, AlphaFoldMode::Layer, kNatronColorViewRGBA, "A", kParitySourceExtraLayerID, true);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_FALSE(effect->canRenderOnGpu(queryFor(fold.op, plane)));
    gAlphaFoldKernel = -1;
}

// The fold carries the result's alpha on the GPU as on the CPU: a broadcast alpha channel, a
// per-channel alpha layer, and the read layer's own alpha, with A read in the rendered plane or in
// the spec layer handed over beside it, and with A2 folded over that result.
TEST_F(StagedImageEffectGpuTest, AlphaFoldRunsInARegionAndMatchesTheCpuFrame)
{
    AlphaFold fold;
    createAlphaFold(&fold);
    if (HasFatalFailure()) {
        return;
    }

    const GpuContextReady registerKernel = [](std::string* error) {
        GpuContext* context = appPTR->getGpuContext();
        gpu::GpuRuntime* gpuRuntime = context ? context->runtime() : nullptr;
        if (!gpuRuntime) {
            *error = "the forced GPU context has no runtime";

            return false;
        }
        gAlphaFoldKernel = (int)gpuRuntime->registerKernel(alphaFoldKernelDesc());

        return true;
    };

    struct Case {
        AlphaFoldMode mode;
        const char* aLayer;
        const char* aAlphaChannel;
        const char* aAlphaLayer;
        bool foldA2;
    };
    // A per-channel alpha layer has as many channels as the layer it is the alpha of.
    const Case cases[] = {
        { AlphaFoldMode::Channel, kParitySourceExtraLayerID, "A", kParitySourceExtraLayerID, false },
        { AlphaFoldMode::Channel, kParitySourceExtraLayerID, "A", kParitySourceExtraLayerID, true },
        { AlphaFoldMode::Channel, kNatronColorViewRGBA, "R", kParitySourceExtraLayerID, true },
        { AlphaFoldMode::Layer, kParitySourceExtraLayerID, "A", kParitySourceExtraLayerID, false },
        { AlphaFoldMode::Layer, kNatronColorViewRGBA, "A", kNatronColorViewRGBA, true },
        { AlphaFoldMode::ReadLayer, kNatronColorViewRGBA, "R", kParitySourceExtraLayerID, true },
        { AlphaFoldMode::ReadLayer, kParitySourceExtraLayerID, "R", kParitySourceExtraLayerID, true },
    };
    for (const Case& c : cases) {
        const std::string what = "alpha mode " + std::to_string((int)c.mode) + ", A in " + c.aLayer + ", alpha " + c.aAlphaChannel + (c.foldA2 ? ", with A2" : "");
        setAlphaFold(fold, c.mode, c.aLayer, c.aAlphaChannel, c.aAlphaLayer, c.foldA2);
        if (HasFatalFailure()) {
            return;
        }
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, fold.op), fold.op, fold.window, kTime, registerKernel);
        gAlphaFoldKernel = -1;
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(fold.op)) << what;
        std::map<std::string, int>::const_iterator ran = result.gpuTasksPerNode.find(fold.op->getScriptName_mt_safe());
        EXPECT_TRUE((ran != result.gpuTasksPerNode.end()) && (ran->second > 0)) << what;
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        ASSERT_FALSE(result.cpu.empty()) << what;

        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < result.cpu.size(); ++i) {
            if (!ParityTolerance::exact().accepts(result.cpu[i], result.gpu[i])) {
                if (mismatches == 0) {
                    ADD_FAILURE() << what << ": float " << i << " gpu " << result.gpu[i] << " cpu " << result.cpu[i];
                }
                ++mismatches;
            }
        }
        EXPECT_EQ(0u, mismatches) << what;
    }
}

#endif // NATRON_HAVE_GPU
