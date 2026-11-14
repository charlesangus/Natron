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

#include "Engine/Nodes/Scene/SceneRaster.h"

#include "Global/Macros.h"

#include <list>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QPointF>
#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoStrokeItem.h"

NATRON_NAMESPACE_USING

namespace {

// The script names of the brush knobs RotoPaint has always had, which TexturePaint now has too.
const char* const kBrushKnobNames[] = {
    "multiStrokeEnabledButton",
    "strokeColorButton",
    "blendingModeButton",
    "opacitySpinbox",
    "pressureOpacityButton",
    "sizeSpinbox",
    "pressureSizeButton",
    "hardnessSpinbox",
    "pressureHardnessButton",
    "buildUpButton",
    "autoConnectViewerButton",
    "effectSpinbox",
    "timeOffsetSpinbox",
    "sourceTypeChoice",
    "resetCloneOffsetButton",
    "PaintBrushToolButton",
    "PaintSolidTool",
    "PencilTool",
    "EraserTool",
    "CloneToolButton",
    "CloneTool",
    "RevealTool",
    "EffectToolButton",
    "BlurTool",
    "SmearTool",
    "MergeToolButton",
    "DodgeTool",
    "BurnTool",
};

void
pickPaintTool(const RotoPaintInteractPtr& interact,
              const KnobButtonPtr& tool,
              const KnobGroupPtr& role)
{
    interact->setCurrentTool(tool);
    interact->onRoleChangedInternal(role);
    interact->onToolChangedInternal(tool);
}

// Lets the previous stroke's neat render land, so that the next stroke reaches the engine at once.
void
landNeatRender()
{
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
    }
}

/**
 * Paints a solid brush stroke and an eraser stroke through the builder into layer, or into the
 * host's base layer when layer is null, and checks they carry the toolbar's settings.
 **/
void
paintThroughBuilder(const RotoPaintInteractPtr& interact,
                    const RotoContextPtr& context,
                    const RotoLayerPtr& layer)
{
    const KnobButtonPtr brush = interact->brushAction.lock();
    const KnobButtonPtr eraser = interact->eraserAction.lock();
    const KnobGroupPtr paintRole = interact->paintBrushToolGroup.lock();
    const KnobDoublePtr size = interact->sizeSpinbox.lock();
    const KnobButtonPtr pressureSize = interact->pressureSizeButton.lock();
    ASSERT_TRUE(brush && eraser && paintRole && size && pressureSize);

    const double time = context->getTimelineCurrentTime();
    const RotoLayerPtr expectedLayer = layer ? layer : context->getOrCreateBaseLayer();

    pickPaintTool(interact, brush, paintRole);
    size->setValue(37.);
    pressureSize->setValue(true);

    EXPECT_FALSE(interact->continuePaintStroke(QPointF(0., 0.), 1., 0.)) << "no stroke is open yet";
    ASSERT_TRUE(interact->beginPaintStroke(layer, QPointF(100., 100.), 0.5, time, 1.));
    EXPECT_TRUE(interact->continuePaintStroke(QPointF(120., 100.), 0.75, 2.));
    const RotoStrokeItemPtr solid = interact->finishPaintStroke();
    ASSERT_TRUE(bool(solid));
    EXPECT_FALSE(bool(interact->finishPaintStroke())) << "the stroke is already finished";

    EXPECT_EQ(expectedLayer, solid->getParentLayer());
    EXPECT_EQ(eRotoStrokeTypeSolid, solid->getBrushType());
    EXPECT_DOUBLE_EQ(37., solid->getBrushSizeKnob()->getValue());
    EXPECT_TRUE(solid->getPressureSizeKnob()->getValue());

    std::list<std::list<std::pair<Point, double>>> strokes;
    solid->evaluateStroke(0, time, &strokes);
    ASSERT_FALSE(strokes.empty());
    ASSERT_FALSE(strokes.front().empty());
    EXPECT_NEAR(100., strokes.front().front().first.x, 1e-3) << "points are taken as given";
    EXPECT_NEAR(100., strokes.front().front().first.y, 1e-3);
    for (const std::pair<Point, double>& point : strokes.front()) {
        EXPECT_GE(point.second, 0.5 - 1e-3);
        EXPECT_LE(point.second, 0.75 + 1e-3);
    }

    landNeatRender();

    pickPaintTool(interact, eraser, paintRole);
    ASSERT_TRUE(interact->beginPaintStroke(layer, QPointF(110., 90.), 1., time, 3.));
    EXPECT_TRUE(interact->continuePaintStroke(QPointF(110., 130.), 1., 4.));
    const RotoStrokeItemPtr erased = interact->finishPaintStroke();
    ASSERT_TRUE(bool(erased));
    EXPECT_NE(solid, erased);
    EXPECT_EQ(expectedLayer, erased->getParentLayer());
    EXPECT_EQ(eRotoStrokeTypeEraser, erased->getBrushType());

    landNeatRender();

    int found = 0;
    for (const RotoItemPtr& item : expectedLayer->getItems_mt_safe()) {
        if ((item == solid) || (item == erased)) {
            ++found;
        }
    }
    EXPECT_EQ(2, found);
} // paintThroughBuilder

} // anonymous namespace

class RotoPaintHostTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }
};

TEST_F(RotoPaintHostTest, TexturePaintPaintsSessionStrokesWithTheSharedBrushKnobs)
{
    getApp()->getProject()->reset(false, true);

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(bool(node));
    TexturePaint* effect = dynamic_cast<TexturePaint*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);

    for (const char* name : kBrushKnobNames) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }

    const RotoPaintInteractPtr interact = effect->getPaintInteract();
    ASSERT_TRUE(bool(interact));
    const RotoContextPtr context = node->getRotoContext();
    ASSERT_TRUE(bool(context));
    const RotoLayerPtr session = effect->addSession(SceneRaster::ExplicitCamera());
    ASSERT_TRUE(bool(session));

    paintThroughBuilder(interact, context, session);

    getApp()->getProject()->reset(false, true);
}

TEST_F(RotoPaintHostTest, RotoPaintPaintsBuilderStrokesIntoTheBaseLayer)
{
    getApp()->getProject()->reset(false, true);

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(bool(node));
    RotoPaint* effect = dynamic_cast<RotoPaint*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);

    for (const char* name : kBrushKnobNames) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }

    const RotoContextPtr context = node->getRotoContext();
    ASSERT_TRUE(bool(context));

    paintThroughBuilder(effect->getInteract(), context, RotoLayerPtr());

    getApp()->getProject()->reset(false, true);
}
