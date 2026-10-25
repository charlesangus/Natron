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

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

#define kTestPluginIDSTMapUV "net.natron.test.STMapUV"
#define kSTMapUVParamBad "bad"

// The 2-channel and 1-channel non-colour layers the UV test source produces.
ImageLayerDesc
uvMapLayer()
{
    return ImageLayerDesc("uvmap", "uvmap", "UV", std::vector<std::string> { "U", "V" });
}

ImageLayerDesc
uOnlyLayer()
{
    return ImageLayerDesc("uonly", "uonly", "U", std::vector<std::string> { "U" });
}

/**
 * @brief A 64x48 UV map. Layer uvmap holds the identity map, U = (x + 0.5) / 64 and
 * V = (y + 0.5) / 48 at every pixel centre whatever the render scale; uonly holds that U; RGBA
 * holds the map flipped left to right, (1 - U, V, 0, 1). With "bad" on, U is NaN for canonical
 * x < 16 and +inf for 16 <= x < 32 in every layer.
 **/
class STMapUVTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new STMapUVTestEffect(n);
    }

    explicit STMapUVTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
        setSupportsRenderScaleMaybe(eSupportsYes);
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *rod = RectD(0., 0., kParitySourceWidth, kParitySourceHeight);

        return eStatusOK;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDSTMapUV,
        .label = "Test STMap UV",
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobBoolPtr bad = createKnob<KnobBool>(std::string("Bad"));
        bad->setName(kSTMapUVParamBad);
        bad->setAnimationEnabled(false);
        bad->setDefaultValue(false);
        page->addKnob(bad);
        _bad = bad;
    }

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);

        return eStatusOK;
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL
    {
        std::list<ImageLayerDesc>& produced = (*comps)[-1];

        produced.clear();
        produced.push_back(uvMapLayer());
        produced.push_back(uOnlyLayer());
        *passThroughTime = time;
        *passThroughView = view;
        *passThroughInputNb = -1;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const int step = 1 << args.mappedScale.toMipmapLevel();
        KnobBoolPtr badKnob = _bad.lock();
        const bool bad = badKnob && badKnob->getValue();

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image) {
                continue;
            }
            if (image->getBitDepth() != eImageBitDepthFloat) {
                return eStatusFailed;
            }
            const bool isColor = it->first.isColorLayer();
            const int nComps = (int)image->getComponentsCount();
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                const float v = (float)((y * step + 0.5 * step) / kParitySourceHeight);
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float u = (float)((x * step + 0.5 * step) / kParitySourceWidth);
                    if (isColor) {
                        u = 1.f - u;
                    }
                    if (bad && (x * step < 16)) {
                        u = std::numeric_limits<float>::quiet_NaN();
                    } else if (bad && (x * step < 32)) {
                        u = std::numeric_limits<float>::infinity();
                    }
                    const float values[4] = { u, v, 0.f, 1.f };
                    float* pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; c < nComps; ++c) {
                        pixel[c] = values[c];
                    }
                }
            }
        }

        return eStatusOK;
    }

    KnobBoolWPtr _bad;
};

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
registerTestNodes()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNode<STMapUVTestEffect>();
    });
}

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// An empty value sets the selector to None.
bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    if (value.empty()) {
        select->setNone();
    } else {
        select->set(value);
    }

    return true;
}

bool
renderLayer(const NodePtr& node,
            unsigned mipmapLevel,
            const RectI& window,
            const ImageLayerDesc& layer,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(layer);
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != (std::size_t)layer.getNumComponents())) {
        return false;
    }
    *plane = planes[0];

    return true;
}

bool
renderRGBA(const NodePtr& node,
           unsigned mipmapLevel,
           const RectI& window,
           RenderedPlane* plane)
{
    return renderLayer(node, mipmapLevel, window, ImageLayerDesc::getRGBAComponents(), plane);
}

const float*
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    const std::size_t n = plane.channels.size();

    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * n];
}

// Counts the pixels of `actual` over `window` that differ from `expected(x, y)` beyond `tolerance`,
// reporting the first. `expected` returns false for a pixel not to check.
std::size_t
countMismatches(const RenderedPlane& actual,
                const RectI& window,
                const std::function<bool(int, int, float*)>& expected,
                const ParityTolerance& tolerance,
                const std::string& what)
{
    std::size_t wrong = 0;
    float want[4];

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            if (!expected(x, y, want)) {
                continue;
            }
            const float* got = pixelAt(actual, x, y);
            for (int c = 0; c < 4; ++c) {
                if (!std::isfinite(got[c]) || !tolerance.accepts(want[c], got[c])) {
                    if (wrong == 0) {
                        ADD_FAILURE() << what << ": first mismatch at (" << x << ", " << y << ") channel " << c << ": " << got[c] << " expected " << want[c];
                    }
                    ++wrong;
                    break;
                }
            }
        }
    }

    return wrong;
}

// Pixel index p of an axis of n pixels after wrapping (0 clamp, 1 repeat, 2 mirror).
int
wrapIndex(int p,
          int n,
          int mode)
{
    switch (mode) {
    case 1:
        return ((p % n) + n) % n;
    case 2: {
        const int q = ((p % (2 * n)) + 2 * n) % (2 * n);

        return (q < n) ? q : (2 * n - 1 - q);
    }
    default:
        return std::min(std::max(p, 0), n - 1);
    }
}

} // namespace

class NativeSTMapTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        registerTestNodes();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeSTMap64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeUV()
    {
        NodePtr uv = createNodeAtMajor(getApp(), kTestPluginIDSTMapUV, -1);
        EXPECT_TRUE(bool(uv));

        return uv;
    }

    // An STMap reading U and V from the identity layer of the test UV source, unless the caller
    // sets them again.
    NodePtr makeSTMap(const NodePtr& source,
                      const NodePtr& uv,
                      const std::string& filter)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_STMAP), PLUGIN_MAJOR_NATRON_STMAP);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        if (uv) {
            connectNodes(uv, node, kSTMapInputUV, true);
        }
        if (source) {
            connectNodes(source, node, kSTMapInputSource, true);
        }
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));
        EXPECT_TRUE(setChannelSelect(node, kSTMapParamChannelU, "uvmap.U"));
        EXPECT_TRUE(setChannelSelect(node, kSTMapParamChannelV, "uvmap.V"));

        return node;
    }

    // `node` at mipmap 0 matches the Source pixel `map(x, y)` writes to (*sx, *sy), or 0 where map
    // returns false.
    void expectMapped(const NodePtr& source,
                      const NodePtr& node,
                      const std::function<bool(int, int, int*, int*)>& map,
                      const ParityTolerance& tolerance,
                      const std::string& what)
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        ASSERT_FALSE(window.isNull()) << what;
        RenderedPlane before;
        RenderedPlane after;
        ASSERT_TRUE(renderRGBA(source, 0, window, &before)) << what;
        ASSERT_TRUE(renderRGBA(node, 0, window, &after)) << what;
        const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
            int sx = 0;
            int sy = 0;
            if (!map(x, y, &sx, &sy)) {
                std::memset(want, 0, 4 * sizeof(float));
            } else {
                std::memcpy(want, pixelAt(before, sx, sy), 4 * sizeof(float));
            }

            return true; }, tolerance, what);
        EXPECT_EQ(0u, wrong) << what;
    }
};

TEST_F(NativeSTMapTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP, "STMap");
}

TEST_F(NativeSTMapTest, KnobsAndInputs)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_STMAP), PLUGIN_MAJOR_NATRON_STMAP);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(3, node->getNInputs());
    const char* const names[] = {
        kSTMapParamChannelU,
        kSTMapParamChannelV,
        kSTMapParamUVOffset,
        kSTMapParamUVScale,
        kSTMapParamWrapU,
        kSTMapParamWrapV,
        kSTMapParamFormatFrom,
        kResamplerParamFilterType,
        kResamplerParamFilterClamp,
        kResamplerParamFilterBlackOutside,
        kOfxMixParamName,
        kOfxMaskInvertParamName,
    };
    for (const char* name : names) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }
}

TEST_F(NativeSTMapTest, IdentityMapReproducesTheSource)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    expectMapped(source, node, [](int x, int y, int* sx, int* sy) {
        *sx = x;
        *sy = y;

        return true; }, ParityTolerance::exact(), "identity, impulse");

    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("cubic")));
    // The filter's footprint reaches past the format at the edge pixels.
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane before;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(node, 0, window, &after));
    const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
        if ((x < window.x1 + 2) || (x >= window.x2 - 2) || (y < window.y1 + 2) || (y >= window.y2 - 2)) {
            return false;
        }
        std::memcpy(want, pixelAt(before, x, y), 4 * sizeof(float));

        return true; }, ParityTolerance::resampling(), "identity, cubic");
    EXPECT_EQ(0u, wrong);
}

TEST_F(NativeSTMapTest, ColourLayerByDefault)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelU, "rgba.R"));
    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelV, "rgba.G"));
    expectMapped(source, node, [](int x, int y, int* sx, int* sy) {
        *sx = kParitySourceWidth - 1 - x;
        *sy = y;

        return true; }, ParityTolerance::exact(), "flipped map in RGBA");
}

TEST_F(NativeSTMapTest, WrapModes)
{
    const char* const modes[] = { kSTMapWrapClamp, kSTMapWrapRepeat, kSTMapWrapMirror };

    for (int mode = 0; mode < 3; ++mode) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr uv = makeUV();
        ASSERT_TRUE(source && uv);
        NodePtr node = makeSTMap(source, uv, "impulse");
        ASSERT_TRUE(bool(node));
        // U and V * 3 land on the centre of pixel 3x + 1 (3y + 1), up to three formats out.
        ASSERT_TRUE(setKnobValues(node, kSTMapParamUVScale, { 3., 3. }));
        ASSERT_TRUE(setKnobValue(node, kSTMapParamWrapU, std::string(modes[mode])));
        ASSERT_TRUE(setKnobValue(node, kSTMapParamWrapV, std::string(modes[mode])));
        expectMapped(source, node, [mode](int x, int y, int* sx, int* sy) {
            *sx = wrapIndex(3 * x + 1, kParitySourceWidth, mode);
            *sy = wrapIndex(3 * y + 1, kParitySourceHeight, mode);

            return true; }, ParityTolerance::exact(), std::string("wrap ") + modes[mode]);
    }
}

TEST_F(NativeSTMapTest, OffsetAndScale)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    // U - 0.25 moves left by 16 pixels; V * 0.5 halves the rows.
    ASSERT_TRUE(setKnobValues(node, kSTMapParamUVOffset, { 0.25, 0. }));
    ASSERT_TRUE(setKnobValues(node, kSTMapParamUVScale, { 1., 0.5 }));
    expectMapped(source, node, [](int x, int y, int* sx, int* sy) {
        *sx = std::max(x - 16, 0);
        *sy = y / 2;

        return true; }, ParityTolerance::exact(), "offset 0.25, scale 0.5");
}

TEST_F(NativeSTMapTest, UVFromANonColourLayer)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));

    // V from RGBA, U from uvmap: two layers of the same input.
    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelV, "rgba.G"));
    expectMapped(source, node, [](int x, int y, int* sx, int* sy) {
        *sx = x;
        *sy = y;

        return true; }, ParityTolerance::exact(), "U from uvmap, V from RGBA");

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        EXPECT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}

TEST_F(NativeSTMapTest, MissingChannelReadsZero)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    // uonly has one channel: V names one it lacks, and reads 0, the bottom edge of the format.
    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelU, "uonly.U"));
    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelV, "uonly.V"));
    expectMapped(source, node, [](int x, int /*y*/, int* sx, int* sy) {
        *sx = x;
        *sy = 0;

        return true; }, ParityTolerance::exact(), "one-channel layer");

    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelV, std::string()));
    expectMapped(source, node, [](int x, int /*y*/, int* sx, int* sy) {
        *sx = x;
        *sy = 0;

        return true; }, ParityTolerance::exact(), "V set to None");
}

TEST_F(NativeSTMapTest, NonFiniteUVIsOutside)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    ASSERT_TRUE(setKnobValues(uv, kSTMapUVParamBad, { 1. }));
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    // NaN is black; +inf under Clamp is the right edge.
    expectMapped(source, node, [](int x, int y, int* sx, int* sy) {
        *sx = (x < 32) ? (kParitySourceWidth - 1) : x;
        *sy = y;

        return x >= 16; }, ParityTolerance::exact(), "NaN and inf, clamp");

    ASSERT_TRUE(setKnobValue(node, kSTMapParamWrapU, std::string(kSTMapWrapRepeat)));
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("cubic")));
    // Under Repeat both NaN and inf are outside: black, never NaN, whatever the filter.
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(node, 0, window, &after));
    const std::size_t wrong = countMismatches(after, window, [](int x, int /*y*/, float* want) {
        std::memset(want, 0, 4 * sizeof(float));

        return x < 32; }, ParityTolerance::exact(), "NaN and inf, repeat, cubic");
    EXPECT_EQ(0u, wrong);
}

TEST_F(NativeSTMapTest, DisconnectedUVIsBlack)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeSTMap(source, NodePtr(), "impulse");
    ASSERT_TRUE(bool(node));

    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_TRUE(rod.isNull());

    // With no UV the output is empty: renderRoI succeeds and hands back no plane, which the helper
    // reports as its "no plane" failure.
    const RectI window = paritySourceWindow(source, kTime, 0);
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error));
    EXPECT_EQ(std::string("the render produced no plane"), error);
    EXPECT_TRUE(planes.empty());
}

TEST_F(NativeSTMapTest, FormatFrom)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeSource();
    ASSERT_TRUE(source && uv);
    setParitySourceOrigin(uv, 8, 4);
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    RectD rod;
    bool isProjectFormat = false;
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_EQ(RectD(0., 0., kParitySourceWidth, kParitySourceHeight), rod) << "UV format";

    ASSERT_TRUE(setKnobValue(node, kSTMapParamFormatFrom, std::string(kSTMapFormatFromUVRoD)));
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_EQ(RectD(8., 4., 8. + kParitySourceWidth, 4. + kParitySourceHeight), rod) << "UV RoD";
}

TEST_F(NativeSTMapTest, MaskAndMixFromTheBase)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    NodePtr mask = makeSource();
    ASSERT_TRUE(source && uv && mask);
    setParitySourceOrigin(mask, 8, 4);
    NodePtr node = makeSTMap(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    // The flipped map, so that the warp differs from the Source almost everywhere.
    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelU, "rgba.R"));
    ASSERT_TRUE(setChannelSelect(node, kSTMapParamChannelV, "rgba.G"));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane before;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(node, 0, window, &after));
    std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
        const float* warped = pixelAt(before, kParitySourceWidth - 1 - x, y);
        const float* here = pixelAt(before, x, y);
        for (int c = 0; c < 4; ++c) {
            want[c] = here[c] + (warped[c] - here[c]) * 0.5f;
        }

        return true; }, ParityTolerance::arithmetic(), "mix 0.5");
    EXPECT_EQ(0u, wrong);

    connectNodes(mask, node, kSTMapInputMask, true);
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 1. }));
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(node, "maskChannel_Mask", "rgba.A"));
    RenderedPlane maskPlane;
    RenderedPlane masked;
    ASSERT_TRUE(renderRGBA(mask, 0, window, &maskPlane));
    ASSERT_TRUE(renderRGBA(node, 0, window, &masked));
    wrong = countMismatches(masked, window, [&](int x, int y, float* want) {
        const float* warped = pixelAt(before, kParitySourceWidth - 1 - x, y);
        const float* here = pixelAt(before, x, y);
        const float m = std::min(1.f, std::max(0.f, pixelAt(maskPlane, x, y)[3]));
        for (int c = 0; c < 4; ++c) {
            want[c] = here[c] + (warped[c] - here[c]) * m;
        }

        return true; }, ParityTolerance::arithmetic(), "mask");
    EXPECT_EQ(0u, wrong);
}
