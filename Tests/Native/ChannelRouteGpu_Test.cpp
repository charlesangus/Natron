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
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"

#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

inline constexpr InputSpec kRouteTestInputs[] = {
    InputSpec { .label = "A", .optional = true },
    InputSpec { .label = "B", .optional = true },
};

// A channel node whose every plane takes the one route it is given.
class ChannelRouteGpuTestEffect
    : public ChannelRouteEffect {
public:
    explicit ChannelRouteGpuTestEffect(NodePtr node)
        : ChannelRouteEffect(node)
        , route()
        , describable(true)
    {
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = "test.natron.ChannelRouteGpu",
        .label = "ChannelRouteGpuTest",
        .inputs = kRouteTestInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual bool channelRoute(double /*time*/,
                              ViewIdx /*view*/,
                              const ImageLayerDesc& /*plane*/,
                              ChannelRoute* r) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *r = route;

        return describable;
    }

    ChannelRoute route;
    bool describable;
};

const ImageLayerDesc&
extraLayer()
{
    static const ImageLayerDesc layer("extra", "extra", "XYZ", std::vector<std::string>({ "X", "Y", "Z" }));

    return layer;
}

// Input 0 in its colour plane and in the extra layer, input 1 in RGB.
ChannelRoute
threeSourceRoute()
{
    ChannelRoute route;

    route.sources.push_back(ChannelRouteSource(0, ImageLayerDesc::getRGBAComponents(), ChannelRouteSource::eNonFloatReadsZero));
    route.sources.push_back(ChannelRouteSource(0, extraLayer(), ChannelRouteSource::eNonFloatReadsZero));
    route.sources.push_back(ChannelRouteSource(1, ImageLayerDesc::getRGBComponents(), ChannelRouteSource::eNonFloatReadsZero));

    return route;
}

// R from extra.Z, G from input 1's R by colour bit, B is 1, A from input 0's alpha by colour bit.
ChannelRoute
mixedRoute()
{
    ChannelRoute route = threeSourceRoute();

    route.components.push_back(ChannelRouteComponent::channel(1, 2));
    route.components.push_back(ChannelRouteComponent::colorBit(2, 0));
    route.components.push_back(ChannelRouteComponent::one());
    route.components.push_back(ChannelRouteComponent::colorBit(0, 3));

    return route;
}

// Into the extra layer: X asks input 1's RGB for alpha, which it lacks, Y is input 0's G, Z is 0.
ChannelRoute
missingChannelRoute()
{
    ChannelRoute route = threeSourceRoute();

    route.components.push_back(ChannelRouteComponent::colorBit(2, 3));
    route.components.push_back(ChannelRouteComponent::channel(0, 1));
    route.components.push_back(ChannelRouteComponent::zero());

    return route;
}

ChannelRoute
constantsRoute()
{
    ChannelRoute route;

    route.components.push_back(ChannelRouteComponent::one());
    route.components.push_back(ChannelRouteComponent::zero());
    route.components.push_back(ChannelRouteComponent::one());
    route.components.push_back(ChannelRouteComponent::one());

    return route;
}

// Into alpha: extra.X.
ChannelRoute
alphaRoute()
{
    ChannelRoute route = threeSourceRoute();

    route.components.push_back(ChannelRouteComponent::channel(1, 0));

    return route;
}

#ifdef NATRON_HAVE_GPU
const double kTime = 1.;

std::vector<float>
testPixels(const RectI& bounds,
           int nComps,
           int seed)
{
    const float values[] = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, std::numeric_limits<float>::quiet_NaN(), 0.9f, -1.f,
                             std::numeric_limits<float>::infinity(), 0.3333f, 0.7f, -0.f, 2.75f, 0.2f, -0.6f, 1e-39f, 1.1f };
    const std::size_t nValues = sizeof(values) / sizeof(values[0]);
    std::vector<float> pixels((std::size_t)bounds.width() * bounds.height() * nComps);

    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = values[(i * 7 + (std::size_t)seed * 5) % nValues];
    }

    return pixels;
}

GpuParityImage
testImage(const RectI& bounds,
          int nComps,
          int seed)
{
    GpuParityImage image;

    image.bounds = bounds;
    image.nComps = nComps;
    image.pixels = testPixels(bounds, nComps, seed);

    return image;
}

float
pixelAt(const GpuParityImage& image,
        int x,
        int y,
        int c)
{
    if (image.pixels.empty() || (c < 0) || (c >= image.nComps) || (x < image.bounds.x1) || (x >= image.bounds.x2) || (y < image.bounds.y1) || (y >= image.bounds.y2)) {
        return 0.f;
    }

    return image.pixels[(((std::size_t)(y - image.bounds.y1) * image.bounds.width()) + (std::size_t)(x - image.bounds.x1)) * image.nComps + (std::size_t)c];
}

// The route written out: sourceImages[i] is route.sources[i] as the GPU reads it. A colour bit is
// the channel of that index in a colour plane of four or three channels, of index 0 in alpha.
std::vector<float>
reference(const ChannelRoute& route,
          const std::vector<const GpuParityImage*>& sourceImages,
          const RectI& window,
          int nComps)
{
    std::vector<float> want((std::size_t)window.width() * window.height() * nComps, 0.f);

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            for (int c = 0; c < nComps; ++c) {
                float v = 0.f;
                if (c < (int)route.components.size()) {
                    const ChannelRouteComponent& comp = route.components[c];
                    if (comp.kind == ChannelRouteComponent::eKindConstant) {
                        v = comp.constant;
                    } else if (const GpuParityImage* image = sourceImages[comp.source]) {
                        int channel = comp.index;
                        if (comp.kind == ChannelRouteComponent::eKindColorBit) {
                            channel = (image->nComps == 1) ? ((comp.index == 3) ? 0 : -1) : comp.index;
                        }
                        v = pixelAt(*image, x, y, channel);
                    }
                }
                want[(((std::size_t)(y - window.y1) * window.width()) + (std::size_t)(x - window.x1)) * nComps + (std::size_t)c] = v;
            }
        }
    }

    return want;
}

GpuQuery
queryFor(const ImageLayerDesc& plane)
{
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.plane = plane;
    q.nComps = plane.getNumComponents();
    q.bitDepth = eImageBitDepthFloat;

    return q;
}
#endif // NATRON_HAVE_GPU

} // namespace

// The first layer a route reads of an input is the input's own, the others are extra layers, each
// named once; an input the route does not read is handed over in the plane.
TEST(ChannelRouteGpuTest, InputLayersFollowTheRoute)
{
    std::shared_ptr<ChannelRouteGpuTestEffect> effect(new ChannelRouteGpuTestEffect(NodePtr()));
    effect->route = mixedRoute();
    effect->route.sources.push_back(ChannelRouteSource(0, extraLayer(), ChannelRouteSource::eNonFloatReadsZero));
    const ImageLayerDesc& plane = ImageLayerDesc::getRGBAComponents();

    ImageLayerDesc layer;
    ASSERT_TRUE(effect->getGpuInputLayer(0, plane, 1., ViewIdx(0), &layer));
    EXPECT_EQ(ImageLayerDesc::getRGBAComponents(), layer);
    std::vector<ImageLayerDesc> extra;
    ASSERT_TRUE(effect->getGpuExtraInputLayers(0, plane, 1., ViewIdx(0), &extra));
    ASSERT_EQ(1u, extra.size());
    EXPECT_EQ(extraLayer(), extra[0]);

    ASSERT_TRUE(effect->getGpuInputLayer(1, plane, 1., ViewIdx(0), &layer));
    EXPECT_EQ(ImageLayerDesc::getRGBComponents(), layer);
    extra.clear();
    ASSERT_TRUE(effect->getGpuExtraInputLayers(1, plane, 1., ViewIdx(0), &extra));
    EXPECT_TRUE(extra.empty());

    effect->route = constantsRoute();
    ASSERT_TRUE(effect->getGpuInputLayer(1, plane, 1., ViewIdx(0), &layer));
    EXPECT_EQ(plane, layer);

    effect->describable = false;
    EXPECT_FALSE(effect->getGpuInputLayer(0, plane, 1., ViewIdx(0), &layer));
    EXPECT_FALSE(effect->getGpuExtraInputLayers(0, plane, 1., ViewIdx(0), &extra));
}

// The GPU takes a describable, valid route at float, with no flag keeping it on the CPU.
TEST(ChannelRouteGpuTest, RefusesWhatTheGpuCannotReproduce)
{
    std::shared_ptr<ChannelRouteGpuTestEffect> effect(new ChannelRouteGpuTestEffect(NodePtr()));
    effect->route = mixedRoute();
    GpuQuery query;
    query.plane = ImageLayerDesc::getRGBAComponents();
    query.nComps = 4;
    EXPECT_TRUE(effect->canRenderOnGpu(query));

    query.bitDepth = eImageBitDepthShort;
    EXPECT_FALSE(effect->canRenderOnGpu(query)) << "short output";
    query.bitDepth = eImageBitDepthFloat;

    effect->route.cpuOnly = true;
    EXPECT_FALSE(effect->canRenderOnGpu(query)) << "cpuOnly";
    effect->route.cpuOnly = false;

    effect->describable = false;
    EXPECT_FALSE(effect->canRenderOnGpu(query)) << "no route";
    effect->describable = true;

    effect->route.components[0] = ChannelRouteComponent::channel(5, 0);
    EXPECT_FALSE(effect->canRenderOnGpu(query)) << "a component naming no source";
    effect->route = mixedRoute();
    effect->route.sources[2].inputNb = 2;
    EXPECT_FALSE(effect->canRenderOnGpu(query)) << "a source naming no input";
}

#ifdef NATRON_HAVE_GPU

// Every route shape against the route written out: channels by index and colour bit from two layers
// of one input and from a second input, constants, a channel the source lacks, a source covering
// part of the window, a disconnected input, and padded views. Pixels travel as bits, so NaN,
// infinity, -0 and denormals come through, and the result is exact.
TEST(ChannelRouteGpuTest, MatchesTheRouteWrittenOut)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    struct Case {
        const char* name;
        ChannelRoute (*route)();
        const ImageLayerDesc* plane;
        bool input1Connected;
        bool input1Partial;
        bool padded;
    };
    const Case cases[] = {
        { "mixed", &mixedRoute, &ImageLayerDesc::getRGBAComponents(), true, false, false },
        { "mixed, input 1 partial", &mixedRoute, &ImageLayerDesc::getRGBAComponents(), true, true, false },
        { "mixed, input 1 partial, padded", &mixedRoute, &ImageLayerDesc::getRGBAComponents(), true, true, true },
        { "mixed, input 1 disconnected", &mixedRoute, &ImageLayerDesc::getRGBAComponents(), false, false, false },
        { "missing channel into the extra layer", &missingChannelRoute, &extraLayer(), true, true, true },
        { "constants only", &constantsRoute, &ImageLayerDesc::getRGBAComponents(), true, false, true },
        { "alpha from the extra layer", &alphaRoute, &ImageLayerDesc::getAlphaComponents(), true, false, false },
    };
    const RectI window(-3, 2, 37, 19);
    int nRenders = 0;

    for (const Case& c : cases) {
        std::shared_ptr<ChannelRouteGpuTestEffect> effect(new ChannelRouteGpuTestEffect(NodePtr()));
        effect->route = c.route();
        const GpuQuery query = queryFor(*c.plane);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        GpuParityImage color = testImage(RectI(-5, 0, 40, 21), 4, 1);
        GpuParityLayerImage extra;
        extra.inputNb = 0;
        extra.layer = extraLayer();
        extra.image = testImage(RectI(-4, 1, 38, 20), 3, 2);
        GpuParityImage rgb;
        if (c.input1Connected) {
            rgb = testImage(c.input1Partial ? RectI(6, 5, 30, 14) : window, 3, 3);
        }
        GpuViewLayout layout;
        if (c.padded) {
            color.layout.padFloats = 12;
            color.layout.rowPadFloats = 5;
            extra.image.layout.padFloats = 3;
            extra.image.layout.rowPadFloats = 2;
            rgb.layout.padFloats = 9;
            rgb.layout.rowPadFloats = 7;
            layout.padFloats = 4;
            layout.rowPadFloats = 3;
            layout.marginLeft = 2;
            layout.marginBottom = 1;
            layout.marginRight = 3;
            layout.marginTop = 2;
        }

        const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>({ color, rgb }), nullptr, window, layout,
                                              std::vector<GpuParityLayerImage>(1, extra));
        ASSERT_TRUE(run.ok) << c.name;
        EXPECT_EQ(0u, run.writtenOutside) << c.name;

        const std::vector<const GpuParityImage*> sources = { &color, &extra.image, c.input1Connected ? &rgb : nullptr };
        const std::vector<float> want = reference(effect->route, sources, window, query.nComps);
        ASSERT_EQ(want.size(), run.pixels.size()) << c.name;
        const ImageDiff diff = compareImages(want, run.pixels, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << c.name << ": first at float " << diff.firstMismatchIndex << " gpu "
                                       << run.pixels[diff.firstMismatchIndex] << " want " << want[diff.firstMismatchIndex];
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (!std::isnan(want[i]) && (std::signbit(want[i]) != std::signbit(run.pixels[i]))) {
                ADD_FAILURE() << c.name << ": the sign of float " << i << " differs";
                break;
            }
        }
        ++nRenders;
    }
    std::printf("channel route gpu vs reference: %d renders\n", nRenders);
}

#endif // NATRON_HAVE_GPU
