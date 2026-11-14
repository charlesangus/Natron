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
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/EdgeSample.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

const int kNComps = 4;
const int kChannel = 2;
const RectI kBounds(10, 20, 15, 24);

// Distinct and non-zero everywhere, so that a zero can only come from a read outside.
float
valueAt(int x,
        int y,
        int c)
{
    return (float)(100 * (c + 1) + 10 * (y - kBounds.y1) + (x - kBounds.x1) + 1);
}

class EdgeSampleImage {
public:
    EdgeSampleImage()
        : _image(ImageLayerDesc::getRGBAComponents(), kBounds.toCanonical_noClipping(0, 1.), kBounds, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false)
    {
        Image::WriteAccess access(&_image);
        for (int y = kBounds.y1; y < kBounds.y2; ++y) {
            float* pix = (float*)access.pixelAt(kBounds.x1, y);
            for (int x = kBounds.x1; x < kBounds.x2; ++x, pix += kNComps) {
                for (int c = 0; c < kNComps; ++c) {
                    pix[c] = valueAt(x, y, c);
                }
            }
        }
    }

    const Image* image() const
    {
        return &_image;
    }

private:
    Image _image;
};

float
readAt(const EdgeSampleImage& src,
       const RectI& bounds,
       int x,
       int y,
       EdgeMode mode)
{
    Image::ReadAccess access(src.image());

    return PassExec::readChannelAt(src.image(), &access, bounds, kNComps, kChannel, x, y, mode);
}

std::vector<float>
readRow(const EdgeSampleImage& src,
        const RectI& bounds,
        int x0,
        int y,
        int width,
        EdgeMode mode)
{
    Image::ReadAccess access(src.image());
    std::vector<float> row((std::size_t)width, -1.f);
    PassExec::readChannelRow(src.image(), &access, bounds, kNComps, kChannel, x0, y, width, mode, row.data());

    return row;
}

} // anonymous namespace

TEST(EdgeSampleTest, CoordClampsForStretchAndRefusesForBlack)
{
    int v = 0;
    EXPECT_TRUE(EdgeSample::coord(12, 10, 15, EdgeMode::Stretch, &v));
    EXPECT_EQ(12, v);
    EXPECT_TRUE(EdgeSample::coord(12, 10, 15, EdgeMode::Black, &v));
    EXPECT_EQ(12, v);

    EXPECT_TRUE(EdgeSample::coord(10 - 3, 10, 15, EdgeMode::Stretch, &v));
    EXPECT_EQ(10, v);
    EXPECT_TRUE(EdgeSample::coord(15 + 3, 10, 15, EdgeMode::Stretch, &v));
    EXPECT_EQ(14, v);
    EXPECT_TRUE(EdgeSample::coord(15, 10, 15, EdgeMode::Stretch, &v));
    EXPECT_EQ(14, v);

    EXPECT_FALSE(EdgeSample::coord(10 - 3, 10, 15, EdgeMode::Black, &v));
    EXPECT_FALSE(EdgeSample::coord(15 + 3, 10, 15, EdgeMode::Black, &v));
    EXPECT_FALSE(EdgeSample::coord(15, 10, 15, EdgeMode::Black, &v));
    EXPECT_FALSE(EdgeSample::coord(9, 10, 15, EdgeMode::Black, &v));

    EXPECT_FALSE(EdgeSample::coord(10, 10, 10, EdgeMode::Stretch, &v));
    EXPECT_FALSE(EdgeSample::coord(10, 10, 10, EdgeMode::Black, &v));
    EXPECT_FALSE(EdgeSample::coord(10, 12, 10, EdgeMode::Stretch, &v));
}

TEST(EdgeSampleTest, PointReadsBeyondEachSide)
{
    const EdgeSampleImage src;
    const RectI& b = kBounds;
    const int midY = b.y1 + 1;

    EXPECT_EQ(valueAt(b.x1 + 2, midY, kChannel), readAt(src, b, b.x1 + 2, midY, EdgeMode::Stretch));
    EXPECT_EQ(valueAt(b.x1 + 2, midY, kChannel), readAt(src, b, b.x1 + 2, midY, EdgeMode::Black));

    EXPECT_EQ(valueAt(b.x1, midY, kChannel), readAt(src, b, b.x1 - 3, midY, EdgeMode::Stretch));
    EXPECT_EQ(valueAt(b.x2 - 1, midY, kChannel), readAt(src, b, b.x2 + 3, midY, EdgeMode::Stretch));
    EXPECT_EQ(0.f, readAt(src, b, b.x1 - 3, midY, EdgeMode::Black));
    EXPECT_EQ(0.f, readAt(src, b, b.x2 + 3, midY, EdgeMode::Black));
}

TEST(EdgeSampleTest, PointReadsBeyondEachCorner)
{
    const EdgeSampleImage src;
    const RectI& b = kBounds;
    struct Corner {
        int x, y, nearestX, nearestY;
    };
    const Corner corners[] = {
        { b.x1 - 3, b.y1 - 3, b.x1, b.y1 },
        { b.x2 + 3, b.y1 - 3, b.x2 - 1, b.y1 },
        { b.x1 - 3, b.y2 + 3, b.x1, b.y2 - 1 },
        { b.x2 + 3, b.y2 + 3, b.x2 - 1, b.y2 - 1 },
    };
    for (const Corner& c : corners) {
        EXPECT_EQ(valueAt(c.nearestX, c.nearestY, kChannel), readAt(src, b, c.x, c.y, EdgeMode::Stretch)) << "at (" << c.x << "," << c.y << ")";
        EXPECT_EQ(0.f, readAt(src, b, c.x, c.y, EdgeMode::Black)) << "at (" << c.x << "," << c.y << ")";
    }
}

TEST(EdgeSampleTest, RowReadsBeyondBothEnds)
{
    const EdgeSampleImage src;
    const RectI& b = kBounds;
    const int x0 = b.x1 - 3;
    const int width = b.width() + 6;
    const int midY = b.y1 + 2;

    const std::vector<float> stretched = readRow(src, b, x0, midY, width, EdgeMode::Stretch);
    const std::vector<float> black = readRow(src, b, x0, midY, width, EdgeMode::Black);
    for (int i = 0; i < width; ++i) {
        const int x = x0 + i;
        const int nearestX = (x < b.x1) ? b.x1 : ((x >= b.x2) ? (b.x2 - 1) : x);
        EXPECT_EQ(valueAt(nearestX, midY, kChannel), stretched[(std::size_t)i]) << "x " << x;
        const bool inside = (x >= b.x1) && (x < b.x2);
        EXPECT_EQ(inside ? valueAt(x, midY, kChannel) : 0.f, black[(std::size_t)i]) << "x " << x;
    }

    // Rows past the top and bottom read the nearest row for Stretch, with its corners.
    const int rows[] = { b.y1 - 3, b.y2 + 3 };
    for (int y : rows) {
        const int nearestY = (y < b.y1) ? b.y1 : (b.y2 - 1);
        const std::vector<float> s = readRow(src, b, x0, y, width, EdgeMode::Stretch);
        const std::vector<float> z = readRow(src, b, x0, y, width, EdgeMode::Black);
        for (int i = 0; i < width; ++i) {
            const int x = x0 + i;
            const int nearestX = (x < b.x1) ? b.x1 : ((x >= b.x2) ? (b.x2 - 1) : x);
            EXPECT_EQ(valueAt(nearestX, nearestY, kChannel), s[(std::size_t)i]) << "(" << x << "," << y << ")";
            EXPECT_EQ(0.f, z[(std::size_t)i]) << "(" << x << "," << y << ")";
        }
    }
}

TEST(EdgeSampleTest, EmptyOrMissingSourceReadsZero)
{
    const EdgeSampleImage src;
    const RectI empty(kBounds.x1, kBounds.y1, kBounds.x1, kBounds.y1);
    const EdgeMode modes[] = { EdgeMode::Stretch, EdgeMode::Black };
    for (EdgeMode mode : modes) {
        EXPECT_EQ(0.f, readAt(src, empty, kBounds.x1, kBounds.y1, mode));
        EXPECT_EQ(0.f, readAt(src, empty, kBounds.x1 - 3, kBounds.y1 - 3, mode));
        const std::vector<float> row = readRow(src, empty, kBounds.x1 - 3, kBounds.y1, kBounds.width() + 6, mode);
        for (float v : row) {
            EXPECT_EQ(0.f, v);
        }

        EXPECT_EQ(0.f, PassExec::readChannelAt(NULL, NULL, kBounds, kNComps, kChannel, kBounds.x1, kBounds.y1, mode));
        std::vector<float> none(4, -1.f);
        PassExec::readChannelRow(NULL, NULL, kBounds, kNComps, kChannel, kBounds.x1, kBounds.y1, 4, mode, none.data());
        for (float v : none) {
            EXPECT_EQ(0.f, v);
        }
    }
}

TEST(EdgeSampleTest, ResamplerNearestReadMatchesTheHelper)
{
    std::vector<float> data((std::size_t)kBounds.area() * kNComps);
    for (int y = kBounds.y1; y < kBounds.y2; ++y) {
        for (int x = kBounds.x1; x < kBounds.x2; ++x) {
            for (int c = 0; c < kNComps; ++c) {
                data[((std::size_t)(y - kBounds.y1) * kBounds.width() + (x - kBounds.x1)) * kNComps + c] = valueAt(x, y, c);
            }
        }
    }
    const Resampler::SourceImage source(data.data(), kBounds, kNComps);
    const EdgeSampleImage src;
    const int xs[] = { kBounds.x1 - 3, kBounds.x1 + 1, kBounds.x2 + 3 };
    const int ys[] = { kBounds.y1 - 3, kBounds.y1 + 1, kBounds.y2 + 3 };
    const EdgeMode modes[] = { EdgeMode::Stretch, EdgeMode::Black };
    for (EdgeMode mode : modes) {
        for (int y : ys) {
            for (int x : xs) {
                float out[kNComps];
                Resampler::interpolate(Resampler::eFilterImpulse, false, x + 0.5, y + 0.5, source, mode, out);
                EXPECT_EQ(readAt(src, kBounds, x, y, mode), out[kChannel]) << "(" << x << "," << y << ") mode " << (int)mode;
            }
        }
    }
}
