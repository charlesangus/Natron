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

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

RenderContextPtr
tileContext(std::int64_t tile)
{
    return RenderContext::Builder().set(RenderContextName::udimTile(), RenderContextValue(tile)).build();
}

} // namespace

class RenderContextUdim
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
        appPTR->clearAllCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        appPTR->clearAllCaches();
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A counting node caching its output, its value knob set to value or, given one, to expression.
    NodePtr createCounting(double value,
                           const std::string& expression = std::string())
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
        KnobDoublePtr valueKnob = node ? std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("value")) : KnobDoublePtr();
        KnobBoolPtr cacheOutput = node ? std::dynamic_pointer_cast<KnobBool>(node->getKnobByName("cacheOutput")) : KnobBoolPtr();
        if (!valueKnob || !cacheOutput) {
            ADD_FAILURE() << "no counting node";

            return NodePtr();
        }
        cacheOutput->setValue(true);
        valueKnob->setValue(value);
        if (!expression.empty()) {
            EXPECT_NO_THROW(valueKnob->setExpression(0, expression, false, true)) << expression;
        }

        return node;
    }

    // The first pixel of node rendered over its square under context, -1 when the render fails.
    static float renderTile(const NodePtr& node,
                            const RenderContextPtr& context)
    {
        const RectI window(0, 0, kCountingTestSize, kCountingTestSize);
        ImagePtr image;
        RectI rendered;
        if (!renderNodeImageOverWindow(node, kTime, ViewIdx(0), ImageLayerDesc::getRGBAComponents(), window, &image, &rendered, context) || !image) {
            return -1.f;
        }
        Image::ReadAccess access = image->getReadRights();
        const float* pixel = (const float*)access.pixelAt(0, 0);

        return pixel ? pixel[0] : -1.f;
    }
};

// udim.tile reads the tile, and udim used any other way reads it too.
TEST_F(RenderContextUdim, ExpressionsReadTheTileThroughUdim)
{
    EXPECT_TRUE(KnobHelper::getRFVExpressionReads("udim.tile or 0").contains(RenderContextName::udimTile()));
    EXPECT_TRUE(KnobHelper::getRFVExpressionReads("getattr(udim, 'tile')").contains(RenderContextName::udimTile()));
    EXPECT_TRUE(KnobHelper::getRFVExpressionReads("thisNode.udim.tile + udims").empty());
    EXPECT_TRUE(KnobHelper::getRFVExpressionReads("'udim.tile'").empty());
}

// The node reading the tile renders once per tile with that tile's value; its input, which reads
// nothing, collapses to the empty context under both tiles and renders once.
TEST_F(RenderContextUdim, OnlyNodesReadingTheTileRenderPerTile)
{
    NodePtr upstream = createCounting(1.);
    NodePtr reader = createCounting(0., "udim.tile or 0");
    ASSERT_TRUE(upstream && reader);
    connectNodes(upstream, reader, 0, true);

    const RenderContextPtr tile1001 = tileContext(1001);
    const RenderContextPtr tile1002 = tileContext(1002);
    EXPECT_EQ(0u, restrictContextToNode(tile1001, upstream, nullptr));
    EXPECT_NE(0u, restrictContextToNode(tile1001, reader, nullptr));
    EXPECT_NE(restrictContextToNode(tile1001, reader, nullptr), restrictContextToNode(tile1002, reader, nullptr));

    EXPECT_EQ(1002.f, renderTile(reader, tile1001));
    EXPECT_EQ(1003.f, renderTile(reader, tile1002));
    EXPECT_EQ(2, CountingTestRegistry::renders(reader));
    EXPECT_EQ(1, CountingTestRegistry::renders(upstream));

    // Each tile's image is cached under its own key.
    EXPECT_EQ(1002.f, renderTile(reader, tile1001));
    EXPECT_EQ(2, CountingTestRegistry::renders(reader));
}

// A node that reads nothing renders under a tile context as under none: one render for every tile.
TEST_F(RenderContextUdim, ARootReadingNothingRendersOnceForEveryTile)
{
    NodePtr plain = createCounting(3.);
    ASSERT_TRUE(bool(plain));

    EXPECT_EQ(3.f, renderTile(plain, tileContext(1001)));
    EXPECT_EQ(3.f, renderTile(plain, tileContext(1002)));
    EXPECT_EQ(3.f, renderTile(plain, RenderContextPtr()));
    EXPECT_EQ(1, CountingTestRegistry::renders(plain));
}
