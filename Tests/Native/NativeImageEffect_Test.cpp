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

#include <bitset>
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
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

#define kNativeImageTestOpPluginID "test.natron.built-in.NativeImageTestOp"

NATRON_NAMESPACE_ENTER

class DoublingTestKernel
    : public PixelKernel {
public:
    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        const float* src = io.src[0];
        const int count = io.width * io.nComps;

        for (int i = 0; i < count; ++i) {
            io.dst[i] = 2.f * src[i];
        }
    }
};

/**
 * @brief A point operator writing 2 * input, with a mask input, the host "(Un)premult by"
 * selector and R, G and B processed by default.
 **/
class NativeImageTestOp
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new NativeImageTestOp(n);
    }

    explicit NativeImageTestOp(NodePtr n)
        : NativeImageEffect(n, makeTraits())
    {
    }

    virtual bool isPointOp() const OVERRIDE FINAL
    {
        return true;
    }

private:
    static NativeImageTraits makeTraits()
    {
        NativeImageTraits traits;

        traits.hostUnPremult = true;
        traits.defaultChannels[3] = false;

        return traits;
    }

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kNativeImageTestOpPluginID;
        desc.label = "Test Native Image Op";
        desc.description = "";
        desc.grouping = "Other/Test";
        desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
        desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
        desc.outputKind = eDataKindImage;

        return desc;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

        addMaskMixKnobs(page);
    }

    virtual PixelKernelPtr makeKernel(const KernelContext& /*context*/) OVERRIDE FINAL
    {
        return std::make_shared<DoublingTestKernel>();
    }
};

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

void
registerNativeImageTestOpOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(NativeImageTestOp::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&NativeImageTestOp::BuildEffect));
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

float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    const std::size_t nComps = plane.channels.size();

    return plane.pixels[(static_cast<std::size_t>(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * nComps + c];
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

} // namespace

class NativeImageEffectTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerNativeImageTestOpOnce();
        BaseTest::SetUp();
    }

    NodePtr createSource()
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr createOp(const NodePtr& source)
    {
        NodePtr op = createNode(QString::fromUtf8(kNativeImageTestOpPluginID));

        EXPECT_TRUE(bool(op));
        if (op && source) {
            connectNodes(source, op, 0, true);
        }

        return op;
    }

    NodePtr connectMask(const NodePtr& op,
                        int originX,
                        int originY)
    {
        NodePtr mask = createSource();

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
        EXPECT_TRUE(setKnobValues(op, "enableMask_Mask", std::vector<double>(1, 1.)));

        return mask;
    }

    void setUnPremultBy(const NodePtr& op,
                        const std::string& channel)
    {
        KnobChannelSelect* selector = dynamic_cast<KnobChannelSelect*>(op->getKnobByName(kUnPremultByKnobName).get());

        ASSERT_TRUE(selector != NULL);
        selector->set(channel);
    }

    NodePtr createThreeLayerReader()
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));

        return getApp()->createNode(readerArgs);
    }
};

TEST_F(NativeImageEffectTest, BaseDeclaresTheHostFacingFlags)
{
    NodePtr op = createOp(createSource());
    ASSERT_TRUE(bool(op));
    EffectInstancePtr effect = op->getEffectInstance();

    EXPECT_TRUE(effect->supportsTiles());
    EXPECT_TRUE(effect->supportsMultiResolution());
    EXPECT_EQ(EffectInstance::eSupportsYes, effect->supportsRenderScaleMaybe());
    EXPECT_EQ(eRenderSafetyFullySafeFrame, effect->renderThreadSafety());
    EXPECT_TRUE(effect->rendersUnprocessedChannels());
    EXPECT_FALSE(effect->isMultiPlanar());

    std::list<ImageBitDepthEnum> depths;
    effect->addSupportedBitDepth(&depths);
    ASSERT_EQ(1u, depths.size());
    EXPECT_EQ(eImageBitDepthFloat, depths.front());

    const int maskInput = inputNamed(op, "Mask");
    ASSERT_GE(maskInput, 0);
    EXPECT_TRUE(effect->isInputMask(maskInput));
    EXPECT_FALSE(effect->isInputMask(0));
    EXPECT_TRUE(bool(op->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(op->getKnobByName("maskChannel_Mask")));
    EXPECT_TRUE(bool(op->getUnPremultBySelector()));
    EXPECT_TRUE(bool(op->getKnobByName(kOfxMixParamName)));
    EXPECT_TRUE(bool(op->getKnobByName(kOfxMaskInvertParamName)));

    std::list<std::string> grouping;
    effect->getPluginGrouping(&grouping);
    ASSERT_EQ(2u, grouping.size());
    EXPECT_EQ(std::string("Other"), grouping.front());
    EXPECT_EQ(std::string("Test"), grouping.back());
}

TEST_F(NativeImageEffectTest, ProcessInPlaceOnRgbLeavesAlphaBitIdentical)
{
    NodePtr source = createSource();
    NodePtr op = createOp(source);
    ASSERT_TRUE(bool(op));

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane plane;
    ASSERT_TRUE(renderColorPlane(op, window, &plane));
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(2.f * paritySourceColorValue(c, x, y, kTime), planeValue(plane, x, y, c)) << x << "," << y << " c=" << c;
            }
            ASSERT_EQ(paritySourceColorValue(3, x, y, kTime), planeValue(plane, x, y, 3)) << x << "," << y;
        }
    }
}

TEST_F(NativeImageEffectTest, AlphaOnlyInputProcessesAlpha)
{
    NodePtr source = createSource();
    setParitySourceComponents(source, "alpha");
    NodePtr op = createOp(source);
    ASSERT_TRUE(bool(op));
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(op->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setAll();

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane plane;
    ASSERT_TRUE(renderColorPlane(op, window, &plane));
    ASSERT_EQ(1u, plane.channels.size());
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            ASSERT_EQ(2.f * paritySourceColorValue(3, x, y, kTime), planeValue(plane, x, y, 0)) << x << "," << y;
        }
    }
}

// The in-kernel divide and multiply must give exactly what the host's own pair gives around the
// same operation.
TEST_F(NativeImageEffectTest, UnPremultByAlphaMatchesTheHostDivideAndMultiply)
{
    NodePtr source = createSource();
    NodePtr op = createOp(source);
    ASSERT_TRUE(bool(op));
    setUnPremultBy(op, "rgba.A");

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane plane;
    ASSERT_TRUE(renderColorPlane(op, window, &plane));
    ASSERT_EQ(4u, plane.channels.size());

    const RectD rod = window.toCanonical_noClipping(0, 1.);
    ImagePtr original = std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), rod, window, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
    ImagePtr expected = std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), rod, window, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
    {
        Image::WriteAccess originalAccess(original.get());
        Image::WriteAccess expectedAccess(expected.get());
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                float* o = (float*)originalAccess.pixelAt(x, y);
                float* e = (float*)expectedAccess.pixelAt(x, y);
                for (int c = 0; c < 4; ++c) {
                    o[c] = e[c] = paritySourceColorValue(c, x, y, kTime);
                }
            }
        }
    }
    std::bitset<4> all;
    all.set();
    std::bitset<4> rgb;
    rgb.set(0);
    rgb.set(1);
    rgb.set(2);
    expected->unPremultiplyByChannel(window, original.get(), 3, all, 3);
    {
        Image::WriteAccess expectedAccess(expected.get());
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                float* e = (float*)expectedAccess.pixelAt(x, y);
                for (int c = 0; c < 3; ++c) {
                    e[c] = 2.f * e[c];
                }
            }
        }
    }
    expected->premultiplyByChannel(window, original.get(), 3, rgb, 3);

    Image::ReadAccess expectedAccess(expected.get());
    int differing = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* e = (const float*)expectedAccess.pixelAt(x, y);
            for (int c = 0; c < 4; ++c) {
                if (e[c] != planeValue(plane, x, y, c)) {
                    if (differing == 0) {
                        ADD_FAILURE() << "first difference at " << x << "," << y << " c=" << c << ": host " << e[c] << ", native " << planeValue(plane, x, y, c);
                    }
                    ++differing;
                }
            }
        }
    }
    EXPECT_EQ(0, differing);
}

TEST_F(NativeImageEffectTest, MaskMixAndMaskInvertFollowTheOpenFxArithmetic)
{
    NodePtr source = createSource();
    NodePtr op = createOp(source);
    ASSERT_TRUE(bool(op));
    const int maskX = 8;
    const int maskY = 4;
    ASSERT_TRUE(bool(connectMask(op, maskX, maskY)));
    ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, 0.5)));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const float mix = 0.5f;
    for (int invert = 0; invert < 2; ++invert) {
        ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, invert)));
        RenderedPlane plane;
        ASSERT_TRUE(renderColorPlane(op, window, &plane));
        ASSERT_EQ(4u, plane.channels.size());
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const bool inMask = (x >= maskX) && (x < maskX + kParitySourceWidth) && (y >= maskY) && (y < maskY + kParitySourceHeight);
                const float m = inMask ? paritySourceColorValue(3, x - maskX, y - maskY, kTime) : 0.f;
                const float alpha = (invert ? (1.f - m) : m) * mix;
                for (int c = 0; c < 3; ++c) {
                    const float s = paritySourceColorValue(c, x, y, kTime);
                    const float v = 2.f * s;
                    const float expected = (alpha == 0.f) ? s : ((alpha == 1.f) ? v : (v * alpha + (1.f - alpha) * s));
                    ASSERT_EQ(expected, planeValue(plane, x, y, c)) << "invert " << invert << " at " << x << "," << y << " c=" << c;
                }
                ASSERT_EQ(paritySourceColorValue(3, x, y, kTime), planeValue(plane, x, y, 3)) << "invert " << invert << " at " << x << "," << y;
            }
        }
    }
}

TEST_F(NativeImageEffectTest, MixZeroAndAMissedMaskAreIdentities)
{
    NodePtr source = createSource();
    NodePtr op = createOp(source);
    ASSERT_TRUE(bool(op));
    EffectInstancePtr effect = op->getEffectInstance();
    const RectI window = paritySourceWindow(source, kTime, 0);
    double inputTime = 0.;
    ViewIdx inputView;
    int inputNb = -1;

    EXPECT_FALSE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));

    ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, 0.)));
    inputNb = -1;
    EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);
    EXPECT_EQ(kTime, inputTime);

    ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, 1.)));
    ASSERT_TRUE(bool(connectMask(op, 1000, 1000)));
    inputNb = -1;
    EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, 1.)));
    EXPECT_FALSE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));

    ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, 0.)));
    const int maskInput = inputNamed(op, "Mask");
    ASSERT_GE(maskInput, 0);
    setParitySourceOrigin(op->getInput(maskInput), 8, 4);
    EXPECT_FALSE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
}

// Legacy and Task graph renders agree bit for bit, and the scheduler plans every image the
// render fetches: the mask layer and the divisor layer, here a layer other than colour.
TEST_F(NativeImageEffectTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    NodePtr source = createSource();
    setParitySourceExtraLayer(source, true);
    NodePtr op = createOp(source);
    ASSERT_TRUE(bool(op));
    ASSERT_TRUE(bool(connectMask(op, 8, 4)));
    ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, 0.75)));

    const char* const divisors[2] = { kParitySourceExtraLayerID ".G", "rgba.A" };
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (int d = 0; d < 2; ++d) {
        setUnPremultBy(op, divisors[d]);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
            std::vector<int> unplannedPulls;
            const RenderMismatch m = renderBothWaysDirect(op, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << divisors[d] << ", mipmap " << mipmapLevel << ": " << describe(m);
            ASSERT_EQ(poolSizes.size(), unplannedPulls.size()) << divisors[d];
            for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
                EXPECT_EQ(0, unplannedPulls[i]) << divisors[d] << ", mipmap " << mipmapLevel << ", pool " << poolSizes[i];
            }
        }
    }
}

TEST_F(NativeImageEffectTest, ChannelsAllRendersEveryPlaneOfAThreeLayerInput)
{
    NodePtr reader = createThreeLayerReader();
    ASSERT_TRUE(bool(reader));
    NodePtr op = createOp(reader);
    ASSERT_TRUE(bool(op));
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(op->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setAll();

    std::list<ImageLayerDesc> layers;
    reader->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);
    ASSERT_EQ(3u, layers.size());

    RectD rod;
    bool isProjectFormat = false;
    ASSERT_NE(eStatusFailed, reader->getEffectInstance()->getRegionOfDefinition_public(reader->getEffectInstance()->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    const RectI window = rod.toPixelEnclosing(0, 1.);

    std::vector<RenderedPlane> in;
    std::vector<RenderedPlane> out;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(reader, kTime, ViewIdx(0), 0, window, layers, &in, &error)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(op, kTime, ViewIdx(0), 0, window, layers, &out, &error)) << error;
    ASSERT_EQ(3u, in.size());
    ASSERT_EQ(3u, out.size());
    for (std::size_t p = 0; p < in.size(); ++p) {
        ASSERT_EQ(in[p].pixels.size(), out[p].pixels.size()) << in[p].layer.getLayerID();
        int differing = 0;
        for (std::size_t i = 0; i < in[p].pixels.size(); ++i) {
            if (out[p].pixels[i] != 2.f * in[p].pixels[i]) {
                ++differing;
            }
        }
        EXPECT_EQ(0, differing) << "plane " << in[p].layer.getLayerID();
    }
}
