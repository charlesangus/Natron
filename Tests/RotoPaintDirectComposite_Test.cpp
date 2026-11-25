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

#include <list>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
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

class RotoPaintDirectCompositeTest
    : public BaseTest,
      public ::testing::WithParamInterface<DirectCase> {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        RotoRenderProfile::stop();
        _savedMode = appPTR->getRenderSchedulerMode();
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        RotoPaint::setDirectCompositeEnabled(true);
    }

    void TearDown() override
    {
        RotoPaint::setDirectCompositeEnabled(true);
        appPTR->setRenderSchedulerMode(_savedMode);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    // Every third item is inactive and every fifth inverted; colours stay non-negative so
    // non-separable operators see valid input.
    void buildScene(int itemCount,
                    MergingFunctionEnum op)
    {
        const AppInstancePtr app = getApp();
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        ASSERT_NE(_roto, nullptr);
        _context = _roto->getRotoContext();
        ASSERT_NE(_context, nullptr);
        _context->setAutoKeyingEnabled(false);
        const NodePtr constant = createConstantIn(app, app->getProject(), kWindow, { 0.2, 0.45, 0.7, 0.6 });
        ASSERT_NE(constant, nullptr);
        ASSERT_TRUE(_roto->connectInput(constant, 0));

        for (int i = 0; i < itemCount; ++i) {
            const double x = 4. + (i * 37) % 200;
            const double y = 6. + (i * 53) % 190;
            const double size = 24. + (i * 11) % 50;
            const BezierPtr shape = _context->makeSquare(x, y, size, 1.);
            ASSERT_NE(shape, nullptr);
            shape->setFeatherDistance((i % 4) * 5., 1.);
            shape->setColor(1., ((i * 29) % 100) / 100., ((i * 61) % 100) / 100., ((i * 17) % 100) / 100.);
            shape->setOpacity(0.35 + ((i * 13) % 65) / 100., 1.);
            shape->setCompositingOperator((int)op);
            if (i % 3 == 2) {
                shape->setActivated(false, 1.);
            }
            const KnobBoolPtr inverted = shape->getInvertedKnob();
            if (inverted && (i % 5 == 4)) {
                inverted->setValue(true);
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
                                        const std::list<ImageLayerDesc>& layers)
    {
        std::ostringstream where;
        where << "mipmap " << mipmapLevel << " roi " << roi.x1 << ',' << roi.y1 << ' ' << roi.x2 << ',' << roi.y2;
        std::string error;
        std::vector<RenderedPlane> nodePath;
        EXPECT_TRUE(render(false, mipmapLevel, roi, layers, &nodePath, &error)) << where.str() << ": " << error;
        const unsigned long long before = RotoPaint::getDirectCompositeRenderCount();
        std::vector<RenderedPlane> direct;
        EXPECT_TRUE(render(true, mipmapLevel, roi, layers, &direct, &error)) << where.str() << ": " << error;
        const unsigned long long ran = RotoPaint::getDirectCompositeRenderCount() - before;
        EXPECT_EQ(nodePath.size(), direct.size()) << where.str();
        for (std::size_t i = 0; i < nodePath.size() && i < direct.size(); ++i) {
            EXPECT_EQ(std::string(), compareExact(nodePath[i], direct[i])) << where.str() << " plane " << i;
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
    RotoContextPtr _context;

private:
    RenderSchedulerModeEnum _savedMode = eRenderSchedulerModeLegacy;
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
