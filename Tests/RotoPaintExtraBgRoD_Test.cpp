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
#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kFormatWidth = 64;
const int kFormatHeight = 48;

class RotoPaintExtraBgRoDTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kFormatWidth, kFormatHeight, "rotoPaintExtraBg64x48", 1.));
    }

    NodePtr createConstant(double r,
                           double g,
                           double b)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        if (!constant) {
            return constant;
        }
        KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
        if (!color) {
            return NodePtr();
        }
        color->setValue(r, ViewSpec::all(), 0);
        color->setValue(g, ViewSpec::all(), 1);
        color->setValue(b, ViewSpec::all(), 2);
        color->setValue(1., ViewSpec::all(), 3);

        return constant;
    }

    // The main background input and the first extra one, by label: input indices move as inputs
    // are added.
    static void findBgInputs(const NodePtr& node,
                             int* mainBg,
                             int* extraBg)
    {
        *mainBg = -1;
        *extraBg = -1;
        const EffectInstancePtr effect = node->getEffectInstance();
        for (int i = 0; i < node->getNInputs(); ++i) {
            if (effect->isInputMask(i)) {
                continue;
            }
            const std::string label = node->getInputLabel(i);
            if (label == "Bg") {
                if (*mainBg == -1) {
                    *mainBg = i;
                }
            } else if ((label.compare(0, 2, "Bg") == 0) && (*extraBg == -1)) {
                *extraBg = i;
            }
        }
    }

    static RectD regionOf(const NodePtr& node)
    {
        const EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;
        if (effect->getRegionOfDefinition_public(effect->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
            return RectD();
        }

        return rod;
    }

    // An extra Bg input must leave the output covering the main Bg with the main Bg showing
    // outside the shape, whether that input has an image or answers with an empty region.
    void checkExtraBgKeepsMainBg(bool extraBgEmpty)
    {
        resetProject();
        NodePtr mainBg = createConstant(1., 0., 0.);
        NodePtr extraBg = createConstant(0., 1., 0.);
        ASSERT_TRUE(mainBg && extraBg);
        if (extraBgEmpty) {
            // A disabled node with nothing to pass through has an empty region.
            extraBg->getDisabledKnob()->setValue(true);
        }

        NodePtr paint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
        ASSERT_TRUE(bool(paint));
        int mainBgInput = -1;
        int extraBgInput = -1;
        findBgInputs(paint, &mainBgInput, &extraBgInput);
        ASSERT_NE(-1, mainBgInput);
        ASSERT_NE(-1, extraBgInput);
        connectNodes(mainBg, paint, mainBgInput, true);

        // Corners (10, 30) and (18, 22): well inside the format, away from the pixel read below.
        BezierPtr square = paint->getRotoContext()->makeSquare(10., 30., 8., 1.);
        ASSERT_TRUE(bool(square));
        square->getFeatherKnob()->setValue(0.);

        connectNodes(extraBg, paint, extraBgInput, true);

        const RectD mainRoD = regionOf(mainBg);
        ASSERT_FALSE(mainRoD.isNull());
        const RectD paintRoD = regionOf(paint);
        EXPECT_EQ(mainRoD.x1, paintRoD.x1);
        EXPECT_EQ(mainRoD.y1, paintRoD.y1);
        EXPECT_EQ(mainRoD.x2, paintRoD.x2);
        EXPECT_EQ(mainRoD.y2, paintRoD.y2);

        const RectI window(0, 0, kFormatWidth, kFormatHeight);
        std::list<ImageLayerDesc> layers;
        layers.push_back(ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(paint, 1., ViewIdx(0), 0, window, layers, &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        const RenderedPlane& plane = planes.front();
        ASSERT_EQ(4u, plane.channels.size());
        ASSERT_EQ((std::size_t)window.area() * 4, plane.pixels.size());

        const auto pixel = [&](int x, int y, int c) {
            return plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4 + c];
        };
        const int outsideX = 50;
        const int outsideY = 5;
        EXPECT_EQ(1.f, pixel(outsideX, outsideY, 0));
        EXPECT_EQ(0.f, pixel(outsideX, outsideY, 1));
        EXPECT_EQ(0.f, pixel(outsideX, outsideY, 2));
        EXPECT_EQ(1.f, pixel(outsideX, outsideY, 3));
        // The shape is drawn over the main Bg, not cut out of black.
        EXPECT_GT(pixel(14, 26, 1), 0.5f);
    }
};

} // namespace

TEST_F(RotoPaintExtraBgRoDTest, ExtraBgWithAnImageKeepsTheMainBg)
{
    checkExtraBgKeepsMainBg(false);
}

TEST_F(RotoPaintExtraBgRoDTest, ExtraBgWithAnEmptyRegionKeepsTheMainBg)
{
    checkExtraBgKeepsMainBg(true);
}
