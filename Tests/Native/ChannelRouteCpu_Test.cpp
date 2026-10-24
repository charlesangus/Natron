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

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

inline constexpr InputSpec kRouteCpuTestInputs[] = {
    InputSpec { .label = "A", .optional = true },
    InputSpec { .label = "B", .optional = true },
};

// A channel node whose every plane takes the one route it is given; the CPU executor only asks it
// whether the render was aborted.
class ChannelRouteCpuTestEffect
    : public ChannelRouteEffect {
public:
    explicit ChannelRouteCpuTestEffect(NodePtr node)
        : ChannelRouteEffect(node)
        , route()
    {
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = "test.natron.ChannelRouteCpu",
        .label = "ChannelRouteCpuTest",
        .inputs = kRouteCpuTestInputs,
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

        return true;
    }

    ChannelRoute route;
};

const ImageLayerDesc&
extraLayer()
{
    static const ImageLayerDesc layer("extra", "extra", "XYZ", std::vector<std::string>({ "X", "Y", "Z" }));

    return layer;
}

ChannelRoute
threeSourceRoute(ChannelRouteSource::NonFloatEnum nonFloat)
{
    ChannelRoute route;

    route.sources.push_back(ChannelRouteSource(0, ImageLayerDesc::getRGBAComponents(), nonFloat));
    route.sources.push_back(ChannelRouteSource(0, extraLayer(), nonFloat));
    route.sources.push_back(ChannelRouteSource(1, ImageLayerDesc::getRGBComponents(), nonFloat));

    return route;
}

// The routes of ChannelRouteGpu_Test, plus one reading a source twice.
ChannelRoute
mixedRoute(ChannelRouteSource::NonFloatEnum nonFloat)
{
    ChannelRoute route = threeSourceRoute(nonFloat);

    route.components.push_back(ChannelRouteComponent::channel(1, 2));
    route.components.push_back(ChannelRouteComponent::colorBit(2, 0));
    route.components.push_back(ChannelRouteComponent::one());
    route.components.push_back(ChannelRouteComponent::colorBit(0, 3));

    return route;
}

ChannelRoute
missingChannelRoute(ChannelRouteSource::NonFloatEnum nonFloat)
{
    ChannelRoute route = threeSourceRoute(nonFloat);

    route.components.push_back(ChannelRouteComponent::colorBit(2, 3));
    route.components.push_back(ChannelRouteComponent::channel(0, 1));
    route.components.push_back(ChannelRouteComponent::zero());

    return route;
}

ChannelRoute
constantsRoute(ChannelRouteSource::NonFloatEnum /*nonFloat*/)
{
    ChannelRoute route;

    route.components.push_back(ChannelRouteComponent::one());
    route.components.push_back(ChannelRouteComponent::zero());
    route.components.push_back(ChannelRouteComponent::one());
    route.components.push_back(ChannelRouteComponent::one());

    return route;
}

ChannelRoute
alphaRoute(ChannelRouteSource::NonFloatEnum nonFloat)
{
    ChannelRoute route = threeSourceRoute(nonFloat);

    route.components.push_back(ChannelRouteComponent::channel(1, 0));

    return route;
}

// Colour channels of input 0 swapped, with its alpha read twice through two sources.
ChannelRoute
swapRoute(ChannelRouteSource::NonFloatEnum nonFloat)
{
    ChannelRoute route = threeSourceRoute(nonFloat);

    route.sources.push_back(route.sources[0]);
    route.components.push_back(ChannelRouteComponent::colorBit(0, 2));
    route.components.push_back(ChannelRouteComponent::colorBit(3, 3));
    route.components.push_back(ChannelRouteComponent::colorBit(0, 0));
    route.components.push_back(ChannelRouteComponent::colorBit(0, 3));

    return route;
}

ImagePtr
makeImage(const ImageLayerDesc& layer,
          const RectI& bounds,
          ImageBitDepthEnum depth)
{
    const RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(layer, rod, bounds, 0 /*mipmapLevel*/, 1. /*par*/, depth, eImageFieldingOrderNone, false /*useBitmap*/);
}

template <typename PIX>
PIX
testValue(int i)
{
    return (PIX)((i * 37) % 256);
}

template <>
unsigned short
testValue<unsigned short>(int i)
{
    return (unsigned short)((i * 4099) % 65536);
}

template <>
float
testValue<float>(int i)
{
    const float values[] = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, std::numeric_limits<float>::quiet_NaN(), 0.9f, -1.f,
                             std::numeric_limits<float>::infinity(), 0.3333f, 0.7f, -0.f, 2.75f, 0.2f, -0.6f, 1e-39f, 1.1f };

    return values[i % (int)(sizeof(values) / sizeof(values[0]))];
}

template <typename PIX>
void
fillTyped(Image& image,
          int seed)
{
    // getBounds() takes the image's read lock, which a thread holding its write lock cannot take.
    const RectI bounds = image.getBounds();
    const int nComps = (int)image.getComponentsCount();
    Image::WriteAccess access(&image);

    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            PIX* p = (PIX*)access.pixelAt(x, y);
            for (int c = 0; c < nComps; ++c) {
                p[c] = testValue<PIX>(((y - bounds.y1) * bounds.width() + (x - bounds.x1)) * nComps + c + seed * 5);
            }
        }
    }
}

ImagePtr
testImage(const ImageLayerDesc& layer,
          const RectI& bounds,
          ImageBitDepthEnum depth,
          int seed)
{
    ImagePtr image = makeImage(layer, bounds, depth);

    switch (depth) {
    case eImageBitDepthByte:
        fillTyped<unsigned char>(*image, seed);
        break;
    case eImageBitDepthShort:
        fillTyped<unsigned short>(*image, seed);
        break;
    default:
        fillTyped<float>(*image, seed);
        break;
    }

    return image;
}

template <typename SRCPIX, typename DSTPIX>
DSTPIX
readConverted(const Image& image,
              int x,
              int y,
              int c)
{
    Image::ReadAccess access(&image);
    const SRCPIX* p = (const SRCPIX*)access.pixelAt(x, y);

    return p ? Image::convertPixelDepth<SRCPIX, DSTPIX>(p[c]) : DSTPIX(0);
}

// The value component c of the route gives at (x, y), written out: a colour bit is the channel of
// that index in a colour plane of three or four channels and alpha's one channel for bit 3, and a
// non-float source in the reads-zero mode reads 0.
template <typename DSTPIX>
DSTPIX
expectedValue(const ChannelRoute& route,
              const std::vector<ImagePtr>& images,
              int x,
              int y,
              int c)
{
    if (c >= (int)route.components.size()) {
        return DSTPIX(0);
    }
    const ChannelRouteComponent& comp = route.components[c];
    if (comp.kind == ChannelRouteComponent::eKindConstant) {
        return Image::convertPixelDepth<float, DSTPIX>(comp.constant);
    }
    const ImagePtr& image = images[comp.source];
    if (!image) {
        return DSTPIX(0);
    }
    const bool isFloat = (image->getBitDepth() == eImageBitDepthFloat);
    if (!isFloat && (route.sources[comp.source].nonFloat == ChannelRouteSource::eNonFloatReadsZero)) {
        return DSTPIX(0);
    }
    int channel = comp.index;
    if ((comp.kind == ChannelRouteComponent::eKindColorBit) && (image->getComponentsCount() == 1)) {
        channel = (comp.index == 3) ? 0 : -1;
    }
    if ((channel < 0) || (channel >= (int)image->getComponentsCount())) {
        return DSTPIX(0);
    }
    switch (image->getBitDepth()) {
    case eImageBitDepthByte:
        return readConverted<unsigned char, DSTPIX>(*image, x, y, channel);
    case eImageBitDepthShort:
        return readConverted<unsigned short, DSTPIX>(*image, x, y, channel);
    default:
        return readConverted<float, DSTPIX>(*image, x, y, channel);
    }
}

// Compares bit for bit, so NaN, -0 and denormals must come through unchanged.
template <typename DSTPIX>
int
countMismatches(const ChannelRoute& route,
                const std::vector<ImagePtr>& images,
                const Image& dst,
                const RectI& roi,
                std::string* first)
{
    Image::ReadAccess access(&dst);
    const int nComps = (int)dst.getComponentsCount();
    int mismatches = 0;

    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            const DSTPIX* p = (const DSTPIX*)access.pixelAt(x, y);
            for (int c = 0; c < nComps; ++c) {
                const DSTPIX want = expectedValue<DSTPIX>(route, images, x, y, c);
                if (std::memcmp(&want, &p[c], sizeof(DSTPIX)) != 0) {
                    if (mismatches == 0) {
                        *first = "(" + std::to_string(x) + ", " + std::to_string(y) + ") component " + std::to_string(c)
                            + ": got " + std::to_string((double)p[c]) + ", want " + std::to_string((double)want);
                    }
                    ++mismatches;
                }
            }
        }
    }

    return mismatches;
}

const char*
depthName(ImageBitDepthEnum depth)
{
    switch (depth) {
    case eImageBitDepthByte:
        return "byte";
    case eImageBitDepthShort:
        return "short";
    default:
        return "float";
    }
}

} // namespace

// Every route of the GPU test, and one reading an image through two sources, from byte, short and
// float sources into byte, short and float outputs, with non-float sources read as 0 and converted.
// The output is wider than the window, which covers part of input 1, so reads outside a source are
// 0; input 1 is also disconnected.
TEST(ChannelRouteCpuTest, MatchesTheRouteWrittenOut)
{
    std::shared_ptr<ChannelRouteCpuTestEffect> effect(new ChannelRouteCpuTestEffect(NodePtr()));

    struct RouteCase {
        const char* name;
        ChannelRoute (*route)(ChannelRouteSource::NonFloatEnum);
        const ImageLayerDesc* plane;
    };
    const RouteCase routes[] = {
        { "mixed", &mixedRoute, &ImageLayerDesc::getRGBAComponents() },
        { "missing channel into the extra layer", &missingChannelRoute, &extraLayer() },
        { "constants only", &constantsRoute, &ImageLayerDesc::getRGBAComponents() },
        { "alpha from the extra layer", &alphaRoute, &ImageLayerDesc::getAlphaComponents() },
        { "swap, alpha read twice", &swapRoute, &ImageLayerDesc::getRGBAComponents() },
    };
    const ImageBitDepthEnum depths[] = { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthFloat };
    const ChannelRouteSource::NonFloatEnum modes[] = { ChannelRouteSource::eNonFloatReadsZero, ChannelRouteSource::eNonFloatConverts };
    const RectI dstBounds(-5, 0, 40, 21);
    const RectI roi(-3, 2, 37, 19);
    int nRenders = 0;

    for (const RouteCase& r : routes) {
        for (ImageBitDepthEnum srcDepth : depths) {
            for (ImageBitDepthEnum dstDepth : depths) {
                for (ChannelRouteSource::NonFloatEnum mode : modes) {
                    for (int connected = 1; connected >= 0; --connected) {
                        const ChannelRoute route = r.route(mode);
                        ASSERT_TRUE(route.isValid(effect->getNInputs())) << r.name;

                        const ImagePtr color = testImage(ImageLayerDesc::getRGBAComponents(), RectI(-5, 0, 40, 21), srcDepth, 1);
                        const ImagePtr extra = testImage(extraLayer(), RectI(-4, 1, 38, 20), srcDepth, 2);
                        const ImagePtr rgb = connected ? testImage(ImageLayerDesc::getRGBComponents(), RectI(6, 5, 30, 14), srcDepth, 3) : ImagePtr();
                        std::vector<ImagePtr> images = { color, extra, rgb };
                        images.resize(route.sources.size(), color);

                        const ImagePtr dst = makeImage(*r.plane, dstBounds, dstDepth);
                        dst->fillZero(dstBounds);
                        ASSERT_EQ(eStatusOK, ChannelCopy::applyChannelRoute(route, images, dst.get(), roi, *effect)) << r.name;

                        std::string first;
                        int mismatches = 0;
                        switch (dstDepth) {
                        case eImageBitDepthByte:
                            mismatches = countMismatches<unsigned char>(route, images, *dst, roi, &first);
                            break;
                        case eImageBitDepthShort:
                            mismatches = countMismatches<unsigned short>(route, images, *dst, roi, &first);
                            break;
                        default:
                            mismatches = countMismatches<float>(route, images, *dst, roi, &first);
                            break;
                        }
                        EXPECT_EQ(0, mismatches) << r.name << ", " << depthName(srcDepth) << " into " << depthName(dstDepth)
                                                 << (mode == ChannelRouteSource::eNonFloatConverts ? ", converted" : ", read as 0")
                                                 << (connected ? "" : ", input 1 disconnected") << ": first at " << first;
                        ++nRenders;
                    }
                }
            }
        }
    }
    std::printf("channel route cpu vs reference: %d renders\n", nRenders);
}

// Each source is read in the layout it was fetched in: a colour bit finds the same channel of an
// RGB, an RGBA or an alpha image, and an index past the image's channels reads 0.
TEST(ChannelRouteCpuTest, ColourBitsFollowTheFetchedLayout)
{
    std::shared_ptr<ChannelRouteCpuTestEffect> effect(new ChannelRouteCpuTestEffect(NodePtr()));
    const RectI bounds(0, 0, 4, 3);

    struct LayoutCase {
        const ImageLayerDesc* layout;
        int alphaIndex;
    };
    const LayoutCase layouts[] = {
        { &ImageLayerDesc::getRGBAComponents(), 3 },
        { &ImageLayerDesc::getRGBComponents(), -1 },
        { &ImageLayerDesc::getAlphaComponents(), 0 },
    };

    for (const LayoutCase& l : layouts) {
        ChannelRoute route;
        route.sources.push_back(ChannelRouteSource(0, ImageLayerDesc::getRGBAComponents(), ChannelRouteSource::eNonFloatConverts));
        route.components.push_back(ChannelRouteComponent::colorBit(0, 3));
        route.components.push_back(ChannelRouteComponent::channel(0, 3));

        const ImagePtr source = testImage(*l.layout, bounds, eImageBitDepthFloat, 4);
        const ImagePtr dst = makeImage(extraLayer(), bounds, eImageBitDepthFloat);
        dst->fillZero(bounds);
        ASSERT_EQ(eStatusOK, ChannelCopy::applyChannelRoute(route, std::vector<ImagePtr>(1, source), dst.get(), bounds, *effect));

        Image::ReadAccess in(source.get());
        Image::ReadAccess out(dst.get());
        const float* p = (const float*)in.pixelAt(1, 1);
        const float* q = (const float*)out.pixelAt(1, 1);
        const float alpha = (l.alphaIndex >= 0) ? p[l.alphaIndex] : 0.f;
        const float index3 = ((int)source->getComponentsCount() > 3) ? p[3] : 0.f;
        EXPECT_EQ(0, std::memcmp(&alpha, &q[0], sizeof(float))) << l.layout->getLayerID() << " " << l.layout->getNumComponents() << " channels: colour bit 3";
        EXPECT_EQ(0, std::memcmp(&index3, &q[1], sizeof(float))) << l.layout->getLayerID() << " " << l.layout->getNumComponents() << " channels: index 3";
        EXPECT_EQ(0.f, q[2]);
    }
}

// A route the executor cannot run fails the plane rather than writing part of it.
TEST(ChannelRouteCpuTest, RejectsWhatItCannotRun)
{
    std::shared_ptr<ChannelRouteCpuTestEffect> effect(new ChannelRouteCpuTestEffect(NodePtr()));
    const RectI bounds(0, 0, 4, 3);
    const ImagePtr source = testImage(ImageLayerDesc::getRGBAComponents(), bounds, eImageBitDepthFloat, 0);
    const ImagePtr dst = makeImage(ImageLayerDesc::getRGBAComponents(), bounds, eImageBitDepthFloat);

    ChannelRoute route;
    route.sources.push_back(ChannelRouteSource(0, ImageLayerDesc::getRGBAComponents(), ChannelRouteSource::eNonFloatConverts));
    route.components.push_back(ChannelRouteComponent::channel(1, 0));
    EXPECT_FALSE(route.isValid(2));
    EXPECT_EQ(eStatusFailed, ChannelCopy::applyChannelRoute(route, std::vector<ImagePtr>(1, source), dst.get(), bounds, *effect)) << "a component naming no source";

    route.components[0] = ChannelRouteComponent::channel(0, 0);
    EXPECT_TRUE(route.isValid(2));
    EXPECT_EQ(eStatusFailed, ChannelCopy::applyChannelRoute(route, std::vector<ImagePtr>(), dst.get(), bounds, *effect)) << "no image for the source";

    route.sources[0].inputNb = 2;
    EXPECT_FALSE(route.isValid(2)) << "a source naming no input";
}
