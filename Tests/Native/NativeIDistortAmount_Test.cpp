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
#include <cstring>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeGolden.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// The Map input of IDistort and STMap: it scales IDistort's displacement, and STMap's move from
// each pixel's own centre to its UV position, per output pixel.

namespace {

const double kTime = 1.;

// An extent past the 64x48 format on every side, so that a constant Map or UV covers the pixels
// the Jacobians read beyond the frame.
const RectI kWideExtent(-8, -8, kParitySourceWidth + 8, kParitySourceHeight + 8);

const char* const kFilters[] = { "impulse", "cubic" };

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
    select->set(value);

    return true;
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

bool
renderLayer(const NodePtr& node,
            const RectI& window,
            const ImageLayerDesc& layer,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(layer);
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
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
                if (!tolerance.accepts(want[c], got[c])) {
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

// `a` and `b` hold the same floats, bit for bit.
void
expectSameFrame(const RenderedPlane& a,
                const RenderedPlane& b,
                const std::string& what)
{
    ASSERT_EQ(a.pixels.size(), b.pixels.size()) << what;
    EXPECT_EQ(0, std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(float))) << what;
}

// The Map value the default knobs make of a raw alpha sample: clamped to [0, 1].
float
mapValue(float alpha)
{
    if (!(alpha > 0.f)) {
        return 0.f;
    }

    return std::min(alpha, 1.f);
}

GoldenVariant
goldenVariant(const std::string& name,
              bool mask,
              const std::function<bool(const GoldenGraph&)>& apply)
{
    GoldenVariant v;

    v.name = name;
    v.mask = mask;
    v.apply = apply;

    return v;
}

} // namespace

class NativeIDistortAmountTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIDistortAmount64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    // A Constant of color over kWideExtent.
    NodePtr makeConstant(const std::vector<double>& color)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);

        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)kWideExtent.x1, (double)kWideExtent.y1 }));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { (double)kWideExtent.width(), (double)kWideExtent.height() }));
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, color));
        }

        return constant;
    }

    // A Map of m in every channel.
    NodePtr makeMap(double m)
    {
        return makeConstant({ m, m, m, m });
    }

    NodePtr makeIDistort(const NodePtr& source,
                         const NodePtr& uv,
                         const NodePtr& map,
                         const std::string& filter)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_IDISTORT), PLUGIN_MAJOR_NATRON_IDISTORT);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kIDistortInputSource, true);
        }
        if (uv) {
            connectNodes(uv, node, kIDistortInputUV, true);
        }
        if (map) {
            connectNodes(map, node, kIDistortInputMap, true);
        }
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));

        return node;
    }

    // An STMap reading U and V from R and G.
    NodePtr makeSTMap(const NodePtr& source,
                      const NodePtr& uv,
                      const NodePtr& map,
                      const std::string& filter)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_STMAP), PLUGIN_MAJOR_NATRON_STMAP);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (uv) {
            connectNodes(uv, node, kSTMapInputUV, true);
        }
        if (source) {
            connectNodes(source, node, kSTMapInputSource, true);
        }
        if (map) {
            connectNodes(map, node, kSTMapInputMap, true);
        }
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));
        EXPECT_TRUE(setChannelSelect(node, kSTMapParamChannelU, "rgba.R"));
        EXPECT_TRUE(setChannelSelect(node, kSTMapParamChannelV, "rgba.G"));

        return node;
    }

    // A UV field that changes from pixel to pixel: the parity source's spec layer, a few pixels
    // each way once scaled by IDistort's UV Scale.
    NodePtr makeIDistortField(const NodePtr& source,
                              const NodePtr& map,
                              const std::string& filter,
                              NodePtr* uv)
    {
        *uv = makeSource();
        if (!*uv) {
            return NodePtr();
        }
        setParitySourceExtraLayer(*uv, true);
        NodePtr node = makeIDistort(source, *uv, map, filter);
        if (node) {
            EXPECT_TRUE(setChannelSelect(node, kIDistortParamChannelU, std::string(kParitySourceExtraLayerID) + ".R"));
            EXPECT_TRUE(setChannelSelect(node, kIDistortParamChannelV, std::string(kParitySourceExtraLayerID) + ".G"));
            EXPECT_TRUE(setKnobValues(node, kIDistortParamUVScale, { 6., 4. }));
        }

        return node;
    }

    // `node` renders `source` unchanged: bit for bit with impulse, within the resampling bound
    // away from the edges with other filters, whose footprint reaches past the format there.
    void expectSource(const NodePtr& source,
                      const NodePtr& node,
                      bool impulse,
                      const std::string& what)
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        RenderedPlane before;
        RenderedPlane after;
        ASSERT_TRUE(renderRGBA(source, window, &before)) << what;
        ASSERT_TRUE(renderRGBA(node, window, &after)) << what;
        const ParityTolerance tolerance = impulse ? ParityTolerance::exact() : ParityTolerance::resampling();
        const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
            if (!impulse && ((x < window.x1 + 2) || (x >= window.x2 - 2) || (y < window.y1 + 2) || (y >= window.y2 - 2))) {
                return false;
            }
            std::memcpy(want, pixelAt(before, x, y), 4 * sizeof(float));

            return true; }, tolerance, what);
        EXPECT_EQ(0u, wrong) << what;
    }

    // `a` and `b` render the same frame, bit for bit.
    void expectSameRender(const NodePtr& a,
                          const NodePtr& b,
                          const RectI& window,
                          const std::string& what)
    {
        RenderedPlane pa;
        RenderedPlane pb;
        ASSERT_TRUE(renderRGBA(a, window, &pa)) << what;
        ASSERT_TRUE(renderRGBA(b, window, &pb)) << what;
        expectSameFrame(pa, pb, what);
    }
};

TEST_F(NativeIDistortAmountTest, MapIsAnOptionalLastInput)
{
    NodePtr idistort = createNode(QString::fromUtf8(PLUGINID_NATRON_IDISTORT), PLUGIN_MAJOR_NATRON_IDISTORT);
    NodePtr stmap = createNode(QString::fromUtf8(PLUGINID_NATRON_STMAP), PLUGIN_MAJOR_NATRON_STMAP);
    ASSERT_TRUE(idistort && stmap);

    EXPECT_EQ(4, idistort->getNInputs());
    EXPECT_EQ(kIDistortInputMap, idistort->getNInputs() - 1);
    EXPECT_TRUE(idistort->getEffectInstance()->isInputOptional(kIDistortInputMap));
    EXPECT_EQ(4, stmap->getNInputs());
    EXPECT_EQ(kSTMapInputMap, stmap->getNInputs() - 1);
    EXPECT_TRUE(stmap->getEffectInstance()->isInputOptional(kSTMapInputMap));

    const char* const names[] = { kMapInputParamChannel, kMapInputParamInvert, kMapInputParamResponse, kMapInputParamGamma };
    for (const char* name : names) {
        EXPECT_TRUE(bool(idistort->getKnobByName(name))) << "IDistort " << name;
        EXPECT_TRUE(bool(stmap->getKnobByName(name))) << "STMap " << name;
    }
    // A warp has no size, so neither has a Size At Black.
    EXPECT_FALSE(bool(idistort->getKnobByName(kMapInputParamSizeAtBlack)));
    EXPECT_FALSE(bool(stmap->getKnobByName(kMapInputParamSizeAtBlack)));
}

// A white Map scales nothing: the frame is the unmapped node's, bit for bit, over a field whose
// Jacobian changes from pixel to pixel.
TEST_F(NativeIDistortAmountTest, IDistortWhiteMapIsTheUnmappedRender)
{
    for (const char* filter : kFilters) {
        NodePtr source = makeSource();
        NodePtr uvPlain;
        NodePtr uvMapped;
        NodePtr plain = makeIDistortField(source, NodePtr(), filter, &uvPlain);
        NodePtr mapped = makeIDistortField(source, makeMap(1.), filter, &uvMapped);
        ASSERT_TRUE(source && plain && mapped);
        expectSameRender(plain, mapped, paritySourceWindow(source, kTime, 0), std::string("white Map, ") + filter);
    }
}

// Connecting a Map and disconnecting it again gives back the render of a node that never had one.
TEST_F(NativeIDistortAmountTest, IDistortDisconnectedMapRendersAsBefore)
{
    NodePtr source = makeSource();
    NodePtr uv;
    NodePtr node = makeIDistortField(source, NodePtr(), "cubic", &uv);
    NodePtr map = makeMap(0.25);
    ASSERT_TRUE(source && node && map);
    const RectI window = paritySourceWindow(source, kTime, 0);

    RenderedPlane before;
    RenderedPlane during;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(node, window, &before));
    connectNodes(map, node, kIDistortInputMap, true);
    ASSERT_TRUE(renderRGBA(node, window, &during));
    EXPECT_NE(0, std::memcmp(before.pixels.data(), during.pixels.data(), before.pixels.size() * sizeof(float))) << "a Map of 0.25 changes nothing";
    disconnectNodes(map, node, true);
    ASSERT_TRUE(renderRGBA(node, window, &after));
    expectSameFrame(before, after, "Map disconnected");
}

TEST_F(NativeIDistortAmountTest, IDistortBlackMapIsTheSource)
{
    for (const char* filter : kFilters) {
        NodePtr source = makeSource();
        NodePtr uv;
        NodePtr node = makeIDistortField(source, makeMap(0.), filter, &uv);
        ASSERT_TRUE(source && node);
        expectSource(source, node, std::string(filter) == "impulse", std::string("black Map, ") + filter);
    }
}

// A Map of 0.5 halves the displacement: the frame of UV Scale 0.5 without a Map, bit for bit,
// and with impulse the Source moved by half the UV.
TEST_F(NativeIDistortAmountTest, IDistortHalfMapHalvesTheDisplacement)
{
    NodePtr source = makeSource();
    NodePtr uv = makeConstant({ 4., 2., 0., 1. });
    NodePtr half = makeMap(0.5);
    ASSERT_TRUE(source && uv && half);
    const RectI window = paritySourceWindow(source, kTime, 0);

    for (const char* filter : kFilters) {
        NodePtr mapped = makeIDistort(source, uv, half, filter);
        NodePtr scaled = makeIDistort(source, uv, NodePtr(), filter);
        ASSERT_TRUE(mapped && scaled);
        ASSERT_TRUE(setKnobValues(scaled, kIDistortParamUVScale, { 0.5, 0.5 }));
        expectSameRender(mapped, scaled, window, std::string("Map 0.5 against UV Scale 0.5, ") + filter);
    }

    NodePtr impulse = makeIDistort(source, uv, half, "impulse");
    ASSERT_TRUE(bool(impulse));
    RenderedPlane before;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, window, &before));
    ASSERT_TRUE(renderRGBA(impulse, window, &after));
    const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
        if ((x + 2 >= window.x2) || (y + 1 >= window.y2)) {
            return false;
        }
        std::memcpy(want, pixelAt(before, x + 2, y + 1), 4 * sizeof(float));

        return true; }, ParityTolerance::exact(), "Map 0.5 moves by (2, 1)");
    EXPECT_EQ(0u, wrong);
}

// The Map is read at each output pixel: the parity source's alpha bands, exact 0, exact 1 and a
// ramp, scale a displacement field pixel by pixel.
TEST_F(NativeIDistortAmountTest, IDistortMapScalesEachPixel)
{
    NodePtr source = makeSource();
    NodePtr map = makeSource();
    NodePtr uv;
    NodePtr node = makeIDistortField(source, map, "impulse", &uv);
    ASSERT_TRUE(source && map && node);
    const RectI window = paritySourceWindow(source, kTime, 0);

    RenderedPlane before;
    RenderedPlane field;
    RenderedPlane mapPlane;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, window, &before));
    ASSERT_TRUE(renderLayer(uv, window, paritySourceExtraLayer(), &field));
    ASSERT_TRUE(renderRGBA(map, window, &mapPlane));
    ASSERT_TRUE(renderRGBA(node, window, &after));

    const Resampler::SourceImage image(before.pixels.data(), before.window, 4);
    std::size_t partial = 0;
    const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
        const float* d = pixelAt(field, x, y);
        const float m = mapValue(pixelAt(mapPlane, x, y)[3]);
        if ((m > 0.f) && (m < 1.f)) {
            ++partial;
        }
        const double cx = (double)x + 0.5;
        const double cy = (double)y + 0.5;
        const float sx = (m == 0.f) ? (float)cx : (float)(cx + (double)d[0] * 6. * (double)m);
        const float sy = (m == 0.f) ? (float)cy : (float)(cy + (double)d[1] * 4. * (double)m);
        (void)Resampler::interpolate(Resampler::eFilterImpulse, false, sx, sy, image, false, want);

        return true; }, ParityTolerance::exact(), "Map from the alpha bands");
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(partial, 0u) << "no pixel has a Map between 0 and 1";
}

// With a white Map STMap goes all the way to the UV position, as without one, UV Alpha gate
// included.
TEST_F(NativeIDistortAmountTest, STMapWhiteMapIsTheUnmappedRender)
{
    for (const char* filter : kFilters) {
        NodePtr source = makeSource();
        NodePtr uv = makeSource();
        ASSERT_TRUE(source && uv);
        NodePtr plain = makeSTMap(source, uv, NodePtr(), filter);
        NodePtr mapped = makeSTMap(source, uv, makeMap(1.), filter);
        ASSERT_TRUE(plain && mapped);
        expectSameRender(plain, mapped, paritySourceWindow(source, kTime, 0), std::string("white Map, ") + filter);
    }
}

// Where the Map is black every pixel keeps its own place, even where UV Alpha would black it out.
TEST_F(NativeIDistortAmountTest, STMapBlackMapIsTheSource)
{
    for (const char* filter : kFilters) {
        NodePtr source = makeSource();
        NodePtr uv = makeSource();
        ASSERT_TRUE(source && uv);
        NodePtr node = makeSTMap(source, uv, makeMap(0.), filter);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kSTMapParamUVAlpha, { 1. }));
        expectSource(source, node, std::string(filter) == "impulse", std::string("black Map, ") + filter);
    }
}

// A Map value m moves each pixel from its centre c to c + (p - c) * m, p being where the UV sends
// it: half-way at 0.5, and per pixel with the parity source's alpha bands as the Map.
TEST_F(NativeIDistortAmountTest, STMapMapMovesPartOfTheWay)
{
    NodePtr source = makeSource();
    // Every pixel looks up (16, 36) in the 64x48 Source format.
    NodePtr uv = makeConstant({ 0.25, 0.75, 0., 1. });
    NodePtr half = makeMap(0.5);
    NodePtr bands = makeSource();
    ASSERT_TRUE(source && uv && half && bands);
    const RectI window = paritySourceWindow(source, kTime, 0);

    RenderedPlane before;
    RenderedPlane bandPlane;
    ASSERT_TRUE(renderRGBA(source, window, &before));
    ASSERT_TRUE(renderRGBA(bands, window, &bandPlane));
    const Resampler::SourceImage image(before.pixels.data(), before.window, 4);
    const double px = 0. + 0.25 * kParitySourceWidth;
    const double py = 0. + 0.75 * kParitySourceHeight;

    const auto expected = [&](float m, int x, int y, float* want) {
        const double cx = (double)x + 0.5;
        const double cy = (double)y + 0.5;
        float sx = (float)px;
        float sy = (float)py;
        if (m == 0.f) {
            sx = (float)cx;
            sy = (float)cy;
        } else if (m != 1.f) {
            sx = (float)(cx + (px - cx) * (double)m);
            sy = (float)(cy + (py - cy) * (double)m);
        }
        (void)Resampler::interpolate(Resampler::eFilterImpulse, false, sx, sy, image, false, want);
    };

    NodePtr halfway = makeSTMap(source, uv, half, "impulse");
    ASSERT_TRUE(bool(halfway));
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(halfway, window, &after));
    std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
        expected(0.5f, x, y, want);

        return true; }, ParityTolerance::exact(), "Map 0.5");
    EXPECT_EQ(0u, wrong);

    NodePtr perPixel = makeSTMap(source, uv, bands, "impulse");
    ASSERT_TRUE(bool(perPixel));
    ASSERT_TRUE(renderRGBA(perPixel, window, &after));
    wrong = countMismatches(after, window, [&](int x, int y, float* want) {
        expected(mapValue(pixelAt(bandPlane, x, y)[3]), x, y, want);

        return true; }, ParityTolerance::exact(), "Map from the alpha bands");
    EXPECT_EQ(0u, wrong);
}

// Regression fixtures: the parity source warped through a UV that is another parity source and a
// Map that is a third, whose alpha bands give exact 0, exact 1 and a ramp.
TEST_F(NativeIDistortAmountTest, GoldenIDistortMap)
{
    GoldenNodeSpec spec;
    spec.name = "IDistortMap";
    spec.pluginID = PLUGINID_NATRON_IDISTORT;
    spec.major = PLUGIN_MAJOR_NATRON_IDISTORT;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back("UV");
    spec.extraInputs.push_back(kMapInputLabel);

    const std::function<bool(const GoldenGraph&)> knobs = [](const GoldenGraph& g) {
        return setGoldenChannelSelect(g.node, kIDistortParamChannelU, "rgba.R") && setGoldenChannelSelect(g.node, kIDistortParamChannelV, "rgba.G") && setKnobValues(g.node, kIDistortParamUVScale, { 6., 4. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, knobs));
    v.push_back(goldenVariant("remap", false, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValues(g.node, kMapInputParamGamma, { 2. }) && setKnobValue(g.node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)) && setKnobValues(g.node, kMapInputParamInvert, { 1. });
    }));
    v.push_back(goldenVariant("impulse", false, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValue(g.node, kResamplerParamFilterType, std::string("impulse"));
    }));
    runGoldenCases(getApp(), spec, v);
}

// The parity source on STMap's first input is its UV; the Source and the Map are two more.
TEST_F(NativeIDistortAmountTest, GoldenSTMapMap)
{
    GoldenNodeSpec spec;
    spec.name = "STMapMap";
    spec.pluginID = PLUGINID_NATRON_STMAP;
    spec.major = PLUGIN_MAJOR_NATRON_STMAP;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back("Source");
    spec.extraInputs.push_back(kMapInputLabel);

    const std::function<bool(const GoldenGraph&)> knobs = [](const GoldenGraph& g) {
        return setGoldenChannelSelect(g.node, kSTMapParamChannelU, "rgba.R") && setGoldenChannelSelect(g.node, kSTMapParamChannelV, "rgba.G");
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, knobs));
    v.push_back(goldenVariant("remap", false, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValues(g.node, kMapInputParamGamma, { 2. }) && setKnobValue(g.node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)) && setKnobValues(g.node, kMapInputParamInvert, { 1. });
    }));
    v.push_back(goldenVariant("uvalpha", false, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValues(g.node, kSTMapParamUVAlpha, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
