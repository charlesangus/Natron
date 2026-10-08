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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const double kTime = 1.;

void
setConstantColor(const NodePtr& constant,
                 double value)
{
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());

    ASSERT_TRUE(color != NULL);
    for (int i = 0; i < color->getDimension(); ++i) {
        color->setValue(value, ViewSpec::all(), i, eValueChangedReasonUserEdited, NULL);
    }
}

std::string
persistentMessage(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
}

bool
render(const NodePtr& node,
       std::vector<RenderedPlane>* planes)
{
    std::string error;
    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, RectI(0, 0, 8, 8), layers, planes, &error);

    EXPECT_TRUE(ok) << error;

    return ok;
}
} // namespace

class NativeNaNWarningTest
    : public BaseTest {
};

// A colour too large for a float renders as infinity, and multiplying infinity by zero is NaN.
TEST_F(NativeNaNWarningTest, NaNPixelsPostAWarningThatTheNextChangeOfTheNodeClears)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr multiply = createNode(QString::fromUtf8(PLUGINID_NATRON_MULTIPLY));
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(multiply));
    connectNodes(constant, multiply, 0, true);
    setConstantColor(constant, 1e300);
    ASSERT_TRUE(setKnobValues(multiply, kColorMathParamValue, { 0. }));

    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(render(multiply, &planes));
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ(1.f, planes[0].pixels[0]) << "NaN is converted to 1";
    EXPECT_NE(std::string::npos, persistentMessage(multiply).find("NaN")) << persistentMessage(multiply);

    ASSERT_TRUE(setKnobValues(multiply, kColorMathParamValue, { 1. }));
    EXPECT_EQ(std::string(), persistentMessage(multiply));

    ASSERT_TRUE(render(multiply, &planes));
    EXPECT_EQ(std::string(), persistentMessage(multiply));
}

TEST_F(NativeNaNWarningTest, AnUpstreamChangeClearsTheWarning)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr invert = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(invert));
    connectNodes(constant, invert, 0, true);

    invert->setNaNWarning("Invert1: contains NaN values.", invert->getHashValue());
    EXPECT_EQ(std::string("Invert1: contains NaN values."), persistentMessage(invert));

    setConstantColor(constant, 0.25);
    EXPECT_EQ(std::string(), persistentMessage(invert));
}

TEST_F(NativeNaNWarningTest, ARenderStartedBeforeTheLastChangePostsNothing)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    const U64 before = constant->getHashValue();

    setConstantColor(constant, 0.75);
    ASSERT_NE(before, constant->getHashValue());
    constant->setNaNWarning("Constant1: contains NaN values.", before);
    EXPECT_EQ(std::string(), persistentMessage(constant));
}

TEST_F(NativeNaNWarningTest, AnotherMessagePostedSinceIsNotClearedByAChange)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));

    constant->setNaNWarning("Constant1: contains NaN values.", constant->getHashValue());
    constant->setPersistentMessage(eMessageTypeError, "Constant1: something else");
    setConstantColor(constant, 0.75);
    EXPECT_EQ(std::string("Constant1: something else"), persistentMessage(constant));
}
