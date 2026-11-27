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
#include <iostream>
#include <list>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/RotoPaint.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace CacheStress;

namespace {

const RectI kWindow(0, 0, 256, 256);

struct DirectCase {
    MergingFunctionEnum op;
    const char* name;
};

void
PrintTo(const DirectCase& c, std::ostream* os)
{
    *os << c.name;
}

// Bounding box of the samples that differ bit for bit, over the planes' shared window.
void
printDiffExtent(const RenderedPlane& a,
                const RenderedPlane& b,
                const std::string& where)
{
    std::cout << "DIAG " << where << " windows a=" << a.window.x1 << ',' << a.window.y1 << ' ' << a.window.x2 << ',' << a.window.y2
              << " b=" << b.window.x1 << ',' << b.window.y1 << ' ' << b.window.x2 << ',' << b.window.y2
              << " source bounds assumed " << kWindow.x1 << ',' << kWindow.y1 << ' ' << kWindow.x2 << ',' << kWindow.y2 << std::endl;
    const std::size_t ca = a.channels.size();
    const std::size_t cb = b.channels.size();
    if ((ca == 0) || (ca != cb) || (a.window.width() <= 0) || (a.window.x1 != b.window.x1) || (a.window.y1 != b.window.y1) || (a.window.x2 != b.window.x2) || (a.window.y2 != b.window.y2)) {
        return;
    }
    const std::size_t w = (std::size_t)a.window.width();
    const std::size_t n = w * (std::size_t)a.window.height();
    if ((a.pixels.size() != n * ca) || (b.pixels.size() != n * cb)) {
        return;
    }
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    std::size_t count = 0;
    std::size_t insideSource = 0;
    for (std::size_t p = 0; p < n; ++p) {
        bool differs = false;
        for (std::size_t c = 0; c < ca; ++c) {
            if (std::memcmp(&a.pixels[p * ca + c], &b.pixels[p * cb + c], sizeof(float)) != 0) {
                differs = true;
                break;
            }
        }
        if (!differs) {
            continue;
        }
        const int x = a.window.x1 + (int)(p % w);
        const int y = a.window.y1 + (int)(p / w);
        if (count == 0) {
            minX = maxX = x;
            minY = maxY = y;
            std::cout << "DIAG " << where << " first differing pixel (" << x << ',' << y << ") relative to roi origin (" << (x - a.window.x1) << ',' << (y - a.window.y1) << ')' << std::endl;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        if (kWindow.contains(x, y)) {
            ++insideSource;
        }
        ++count;
    }
    std::cout << "DIAG " << where << " differing pixels=" << count << " inside source bounds=" << insideSource << " bbox=" << minX << ',' << minY << ' ' << maxX << ',' << maxY << std::endl;
}

class RotoPaintDirectCompositeTest
    : public BaseTest,
      public ::testing::WithParamInterface<DirectCase> {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        RotoRenderProfile::stop();
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        RotoPaint::setDirectCompositeEnabled(true);
    }

    void TearDown() override
    {
        RotoPaint::setDirectCompositeEnabled(true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    // Every third item is inactive. Unless negativeColours, colours stay non-negative so
    // non-separable operators see valid input; negative ones leave -0 where coverage is zero.
    // With paritySource the input is a parity source, whose depth and components a test can set.
    void buildScene(int itemCount,
                    MergingFunctionEnum op,
                    bool negativeColours = false,
                    bool paritySource = false)
    {
        const AppInstancePtr app = getApp();
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        ASSERT_NE(_roto, nullptr);
        _context = _roto->getRotoContext();
        ASSERT_NE(_context, nullptr);
        _context->setAutoKeyingEnabled(false);
        if (paritySource) {
            _source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        } else {
            _source = createConstantIn(app, app->getProject(), kWindow, { 0.2, 0.45, 0.7, 0.6 });
        }
        ASSERT_NE(_source, nullptr);
        ASSERT_TRUE(_roto->connectInput(_source, 0));

        for (int i = 0; i < itemCount; ++i) {
            const double x = 4. + (i * 37) % 200;
            const double y = 6. + (i * 53) % 190;
            const double size = 24. + (i * 11) % 50;
            const BezierPtr shape = _context->makeSquare(x, y, size, 1.);
            ASSERT_NE(shape, nullptr);
            shape->setFeatherDistance((i % 4) * 5., 1.);
            if (negativeColours) {
                shape->setColor(1., -0.05 - ((i * 29) % 100) / 100., ((i * 61) % 100) / 100. - 0.5, 0.3 - ((i * 17) % 100) / 100.);
            } else {
                shape->setColor(1., ((i * 29) % 100) / 100., ((i * 61) % 100) / 100., ((i * 17) % 100) / 100.);
            }
            shape->setOpacity(0.35 + ((i * 13) % 65) / 100., 1.);
            shape->setCompositingOperator((int)op);
            if (i % 3 == 2) {
                shape->setActivated(false, 1.);
            }
        }
        _context->refreshRotoPaintTree();
    }

    bool render(bool direct,
                unsigned mipmapLevel,
                const RectI& roi,
                const std::list<ImageLayerDesc>& layers,
                std::vector<RenderedPlane>* out,
                std::string* error)
    {
        RotoPaint::setDirectCompositeEnabled(direct);
        clearEverything(getApp());
        const bool ok = renderNodePlanesDirect(_roto, 1., ViewIdx(0), mipmapLevel, roi, layers, out, error, true);
        RotoPaint::setDirectCompositeEnabled(true);

        return ok;
    }

    // Both paths over roi, sample for sample; returns how many direct composites the direct pass ran.
    unsigned long long expectPathsMatch(unsigned mipmapLevel,
                                        const RectI& roi,
                                        const std::list<ImageLayerDesc>& layers,
                                        bool diag = false)
    {
        std::ostringstream where;
        where << "mipmap " << mipmapLevel << " roi " << roi.x1 << ',' << roi.y1 << ' ' << roi.x2 << ',' << roi.y2;
        std::string error;
        std::vector<RenderedPlane> nodePath;
        EXPECT_TRUE(render(false, mipmapLevel, roi, layers, &nodePath, &error)) << where.str() << ": " << error;
        if (diag) {
            std::vector<RenderedPlane> again;
            std::string againError;
            EXPECT_TRUE(render(false, mipmapLevel, roi, layers, &again, &againError)) << where.str() << ": " << againError;
            bool consistent = (again.size() == nodePath.size());
            for (std::size_t i = 0; consistent && i < nodePath.size(); ++i) {
                const std::string msg = compareExact(nodePath[i], again[i]);
                if (!msg.empty()) {
                    consistent = false;
                    ADD_FAILURE() << "DIAG node path not self-consistent: " << where.str() << " plane " << i << ": " << msg;
                    printDiffExtent(nodePath[i], again[i], where.str() + " nodepath-vs-nodepath plane " + std::to_string(i));
                }
            }
            if (consistent) {
                std::cout << "DIAG node path self-consistent (" << where.str() << ")" << std::endl;
            }
        }
        const unsigned long long before = RotoPaint::getDirectCompositeRenderCount();
        std::vector<RenderedPlane> direct;
        EXPECT_TRUE(render(true, mipmapLevel, roi, layers, &direct, &error)) << where.str() << ": " << error;
        const unsigned long long after = RotoPaint::getDirectCompositeRenderCount();
        const unsigned long long ran = after - before;
        if (diag) {
            std::cout << "DIAG direct count before=" << before << " after=" << after << " (" << where.str() << ")" << std::endl;
        }
        EXPECT_EQ(nodePath.size(), direct.size()) << where.str();
        for (std::size_t i = 0; i < nodePath.size() && i < direct.size(); ++i) {
            const std::string msg = compareExact(nodePath[i], direct[i]);
            EXPECT_EQ(std::string(), msg) << where.str() << " plane " << i;
            if (diag && !msg.empty()) {
                printDiffExtent(nodePath[i], direct[i], where.str() + " plane " + std::to_string(i));
            }
        }

        return ran;
    }

    void expectAllWindowsMatch()
    {
        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        EXPECT_GT(expectPathsMatch(0, kWindow, rgba), 0ULL);
        EXPECT_GT(expectPathsMatch(0, RectI(150, 170, 320, 300), rgba), 0ULL);
        EXPECT_GT(expectPathsMatch(1, RectI(0, 0, 128, 128), rgba), 0ULL);
        EXPECT_GT(expectPathsMatch(2, RectI(5, 3, 50, 61), rgba), 0ULL);

        const std::list<ImageLayerDesc> alpha(1, ImageLayerDesc::getAlphaComponents());
        expectPathsMatch(0, kWindow, alpha);
        const std::list<ImageLayerDesc> rgb(1, ImageLayerDesc::getRGBComponents());
        expectPathsMatch(1, RectI(0, 0, 128, 128), rgb);
    }

    NodePtr _roto;
    NodePtr _source;
    RotoContextPtr _context;
};

TEST_P(RotoPaintDirectCompositeTest, ThirtyItemsMatchTheNodePath)
{
    buildScene(30, GetParam().op);
    if (HasFatalFailure()) {
        return;
    }
    expectAllWindowsMatch();
}

// More items than one Merge has A inputs, so the composite spans a chain of Merges.
TEST_P(RotoPaintDirectCompositeTest, MergeChainMatchesTheNodePath)
{
    buildScene(70, GetParam().op);
    if (HasFatalFailure()) {
        return;
    }
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    EXPECT_GT(expectPathsMatch(0, kWindow, rgba), 0ULL);
    EXPECT_GT(expectPathsMatch(1, RectI(20, 10, 100, 120), rgba), 0ULL);
}

// Negative colours make masks hold -0 where an item has no coverage, which the item's effect
// turns into +0. Hue and ColorBurn are left out: they divide by or take the hue of negatives.
TEST_P(RotoPaintDirectCompositeTest, NegativeColoursMatchTheNodePath)
{
    const MergingFunctionEnum op = GetParam().op;
    if ((op == eMergeHue) || (op == eMergeColorBurn)) {
        return;
    }
    buildScene(30, op, true);
    if (HasFatalFailure()) {
        return;
    }
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    EXPECT_GT(expectPathsMatch(0, kWindow, rgba), 0ULL);
    EXPECT_GT(expectPathsMatch(1, RectI(0, 0, 128, 128), rgba), 0ULL);
}

// An input of another depth or with fewer channels reaches the tree converted; whichever path
// renders it, the output matches the node path.
TEST_F(RotoPaintDirectCompositeTest, InputDepthsAndComponentsMatchTheNodePath)
{
    buildScene(30, eMergeOver, false, true);
    if (HasFatalFailure()) {
        return;
    }
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const char* depths[] = { "float", "short", "byte" };
    const char* components[] = { "rgba", "rgb", "alpha" };
    for (const char* depth : depths) {
        for (const char* comps : components) {
            SCOPED_TRACE(std::string(depth) + " " + comps);
            setParitySourceBitDepth(_source, depth);
            setParitySourceComponents(_source, comps);
            const bool integerDepth = (std::string(depth) != "float");
            expectPathsMatch(0, kWindow, rgba, integerDepth);
            expectPathsMatch(1, RectI(0, 0, 128, 128), rgba, integerDepth);
        }
    }
}

// An image without a bitmap claims every pixel of its bounds, so it answers only a request it covers at its
// own level, never a wider one nor, through a downscale, a coarser one.
TEST_F(RotoPaintDirectCompositeTest, UnmarkedImagesServeOnlyWhatTheirBoundsHold)
{
    buildScene(1, eMergeOver, false, true);
    if (HasFatalFailure()) {
        return;
    }
    const EffectInstancePtr effect = _source->getEffectInstance();
    ASSERT_NE(effect, nullptr);
    const ImageLayerDesc& rgba = ImageLayerDesc::getRGBAComponents();
    const RectD rod(0., 0., 64., 64.);
    const RectI fullBounds(0, 0, 64, 64);
    const RectI held(0, 0, 64, 16);
    const ImageKey key(_source.get(), 0x5eedULL, false, 0., ViewIdx(0), 1., false, false);
    const ImagePtr partial = std::make_shared<Image>(key, Image::makeParams(rod, held, 1., 0, false, rgba, eImageBitDepthFloat, eImageFieldingOrderNone));
    ASSERT_FALSE(partial->usesBitMap());
    EffectInstance::InputImagesMap inputImages;
    inputImages[0].push_back(partial);

    const auto lookup = [&](unsigned int mipmapLevel, const RectI& bounds, const RectI& roi) {
        ImagePtr found;
        effect->getImageFromCacheAndConvertIfNeeded(false, eStorageModeRAM, eStorageModeRAM, key, mipmapLevel, &bounds, &rod, roi,
                                                    eImageBitDepthFloat, rgba, inputImages, RenderStatsPtr(), OSGLContextAttacherPtr(), &found);

        return found;
    };

    EXPECT_TRUE(lookup(0, fullBounds, RectI(0, 0, 32, 16)) == partial);
    EXPECT_TRUE(lookup(0, held, RectI(0, 0, 64, 40)) == partial);
    EXPECT_TRUE(lookup(0, fullBounds, fullBounds) == ImagePtr());
    EXPECT_TRUE(lookup(0, fullBounds, RectI(0, 8, 64, 24)) == ImagePtr());
    EXPECT_TRUE(lookup(1, RectI(0, 0, 32, 32), RectI(0, 0, 32, 8)) == ImagePtr());
}

TEST_F(RotoPaintDirectCompositeTest, MixedOperatorsKeepTheNodePath)
{
    buildScene(6, eMergeOver);
    if (HasFatalFailure()) {
        return;
    }
    std::list<RotoDrawableItemPtr> items = _context->getCurvesByRenderOrder(false);
    ASSERT_FALSE(items.empty());
    items.front()->setCompositingOperator((int)eMergePlus);
    _context->refreshRotoPaintTree();
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    EXPECT_EQ(0ULL, expectPathsMatch(0, kWindow, rgba));
}

const DirectCase kCases[] = {
    { eMergeCopy, "Copy" },
    { eMergeOver, "Over" },
    { eMergePlus, "Plus" },
    { eMergeMultiply, "Multiply" },
    { eMergeScreen, "Screen" },
    { eMergeMin, "Min" },
    { eMergeMax, "Max" },
    { eMergeDifference, "Difference" },
    { eMergeStencil, "Stencil" },
    { eMergeATop, "ATop" },
    { eMergeColorBurn, "ColorBurn" },
    { eMergeHue, "Hue" },
};

} // namespace

INSTANTIATE_TEST_CASE_P(Operators, RotoPaintDirectCompositeTest, ::testing::ValuesIn(kCases));
