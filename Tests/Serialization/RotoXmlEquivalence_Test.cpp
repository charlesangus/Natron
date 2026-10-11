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

#include <exception>
#include <list>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoContextSerialization.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"

#include "Serialization/RotoSerialization.h"
#include "Serialization/BoostXml/RotoXml.h"

NATRON_NAMESPACE_USING

namespace {

std::string
saveWithOldPath(const RotoContextPtr& context)
{
    std::stringstream buffer;
    {
        RotoContextSerialization serialization;
        context->save(&serialization);
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("RotoContext", serialization);
    }

    return buffer.str();
}

std::string
saveWithNewPath(const Serialization::RotoContextSerialization& serialization)
{
    std::stringstream buffer;
    {
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("RotoContext", serialization);
    }

    return buffer.str();
}

bool
loadWithNewPath(const std::string& xml,
                Serialization::RotoContextSerialization* serialization)
{
    try {
        std::stringstream buffer(xml);
        boost::archive::xml_iarchive archive(buffer);
        archive >> boost::serialization::make_nvp("RotoContext", *serialization);
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Loading through Serialization/BoostXml failed: " << e.what();

        return false;
    }

    return true;
}

// A fresh shape has a single, unkeyed position: keying it at its creation time and then moving
// a control point and a feather point at a later time is what animates their curves.
void
animateSquare(const BezierPtr& square)
{
    square->setKeyframe(1.);
    square->setPointAtIndex(false, 0, 10., 5., 45., 5., 45., 5., 45.);
    square->setPointAtIndex(true, 2, 10., 30., 25., 28., 25., 32., 25.);
}

} // anonymous namespace

class RotoXmlEquivalenceTest
    : public BaseTest
{
protected:
    RotoContextPtr createRotoPaintContext()
    {
        NodePtr reader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
        NodePtr rotoPaint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));

        if (!reader || !rotoPaint) {
            return RotoContextPtr();
        }
        connectNodes(reader, rotoPaint, 0, true);

        return rotoPaint->getRotoContext();
    }

    // RotoContext::makeStroke() inserts the item into its layer before calling createNodes(),
    // which null-derefs the not-yet-created merge node; this creates the stroke the way
    // RotoPaintInteract::makeStroke() does instead, nodes first.
    RotoStrokeItemPtr addStroke(const RotoContextPtr& context,
                                const RotoLayerPtr& layer,
                                const std::string& name)
    {
        RotoStrokeItemPtr stroke( new RotoStrokeItem( eRotoStrokeTypeSolid, context, name, RotoLayerPtr() ) );

        stroke->createNodes();
        context->addItem(layer, 0, stroke, RotoItem::eSelectionReasonOther);
        stroke->appendPoint( true, RotoPoint(10., 10., 1., 1.) );
        stroke->appendPoint( false, RotoPoint(40., 60., 0.5, 2.) );
        stroke->appendPoint( true, RotoPoint(70., 15., 0.75, 3.) );
        stroke->appendPoint( false, RotoPoint(90., 35., 1., 4.) );
        stroke->setStrokeFinished();

        return stroke;
    }

    // Checks that both paths write the same XML for the context, and that loading that XML
    // through the new path into a fresh context reproduces it.
    void expectOldAndNewPathsMatch(const RotoContextPtr& source,
                                   Serialization::RotoContextSerialization* loaded)
    {
        const std::string oldXml = saveWithOldPath(source);

        Serialization::RotoContextSerialization saved;
        source->toSerialization(&saved);
        const std::string newXml = saveWithNewPath(saved);
        EXPECT_EQ(oldXml, newXml);

        ASSERT_TRUE( loadWithNewPath(newXml, loaded) );
        EXPECT_EQ( newXml, saveWithNewPath(*loaded) );

        RotoContextPtr restored = createRotoPaintContext();
        ASSERT_TRUE( bool(restored) );
        restored->fromSerialization(*loaded);
        EXPECT_EQ( oldXml, saveWithOldPath(restored) );
    }
};

TEST_F(RotoXmlEquivalenceTest, Stroke)
{
    RotoContextPtr context = createRotoPaintContext();

    ASSERT_TRUE( bool(context) );

    RotoStrokeItemPtr stroke = addStroke(context, context->getOrCreateBaseLayer(), "Stroke1");
    ASSERT_TRUE( bool(stroke) );
    // A loaded selection only ever selects beziers again, so a selected stroke would not
    // survive the round trip through either path.
    context->clearSelection(RotoItem::eSelectionReasonOther);

    Serialization::RotoContextSerialization loaded;
    expectOldAndNewPathsMatch(context, &loaded);

    ASSERT_EQ( 1u, loaded.baseLayer.children.size() );
    const Serialization::RotoLayerChildSerialization& child = loaded.baseLayer.children.front();
    EXPECT_EQ(Serialization::eRotoLayerChildTypeStroke, child.type);
    EXPECT_EQ("Stroke1", child.stroke.name);
    EXPECT_EQ( (int)eRotoStrokeTypeSolid, child.stroke.brushType );
    ASSERT_EQ( 2u, child.stroke.strokes.size() );
    EXPECT_EQ( 2u, child.stroke.strokes.front().xCurve.keyframes.size() );
    EXPECT_FALSE( child.stroke.knobs.empty() );
}

TEST_F(RotoXmlEquivalenceTest, AnimatedFeatheredBezierWithBrokenTangent)
{
    RotoContextPtr context = createRotoPaintContext();

    ASSERT_TRUE( bool(context) );

    BezierPtr square = context->makeSquare(1., 50., 20., 1.);
    ASSERT_TRUE( bool(square) );
    animateSquare(square);

    const std::list<BezierCPPtr> controlPoints = square->getControlPoints_mt_safe();
    ASSERT_EQ( 4u, controlPoints.size() );
    controlPoints.front()->setBroken(true);
    square->setLeftBezierPoint(0, 10., 2., 52.);

    KnobDoublePtr featherKnob = square->getFeatherKnob();
    ASSERT_TRUE( bool(featherKnob) );
    featherKnob->setValueAtTime(1, 0., ViewSpec::all(), 0);
    featherKnob->setValueAtTime(10, 6., ViewSpec::all(), 0);

    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(square, RotoItem::eSelectionReasonOther);

    Serialization::RotoContextSerialization loaded;
    expectOldAndNewPathsMatch(context, &loaded);

    ASSERT_EQ( 1u, loaded.baseLayer.children.size() );
    const Serialization::RotoLayerChildSerialization& child = loaded.baseLayer.children.front();
    EXPECT_EQ(Serialization::eRotoLayerChildTypeBezier, child.type);
    ASSERT_EQ( 4u, child.bezier.controlPoints.size() );
    EXPECT_EQ( 4u, child.bezier.featherPoints.size() );
    EXPECT_TRUE(child.bezier.closed);
    EXPECT_FALSE(child.bezier.isOpenBezier);
    EXPECT_EQ( 2u, child.bezier.controlPoints.front().xCurve.keyframes.size() );
    EXPECT_EQ( 2u, child.bezier.controlPoints.front().leftXCurve.keyframes.size() );
    // The project format has never kept the broken flag of a bezier's points.
    EXPECT_FALSE(child.bezier.controlPoints.front().broken);
    ASSERT_EQ( 1u, loaded.selectedItems.size() );
    EXPECT_EQ( square->getScriptName(), loaded.selectedItems.front() );
}

TEST_F(RotoXmlEquivalenceTest, NestedLayers)
{
    RotoContextPtr context = createRotoPaintContext();

    ASSERT_TRUE( bool(context) );

    // A fresh context selects nothing, so the first layer goes under the base layer and each
    // later layer or shape goes under the layer created just before it.
    RotoLayerPtr baseLayer = context->getOrCreateBaseLayer();
    RotoLayerPtr outerLayer = context->addLayer();
    RotoLayerPtr innerLayer = context->addLayer();
    ASSERT_TRUE( bool(outerLayer) && bool(innerLayer) );
    BezierPtr innerSquare = context->makeSquare(5., 40., 10., 1.);
    ASSERT_TRUE( bool(innerSquare) );
    animateSquare(innerSquare);

    RotoStrokeItemPtr outerStroke = addStroke(context, outerLayer, "OuterStroke");
    ASSERT_TRUE( bool(outerStroke) );

    context->clearSelection(RotoItem::eSelectionReasonOther);
    BezierPtr baseEllipse = context->makeEllipse(60., 60., 20., true, 1.);
    ASSERT_TRUE( bool(baseEllipse) );

    context->clearSelection(RotoItem::eSelectionReasonOther);
    context->select(innerSquare, RotoItem::eSelectionReasonOther);

    Serialization::RotoContextSerialization loaded;
    expectOldAndNewPathsMatch(context, &loaded);

    const std::list<Serialization::RotoLayerChildSerialization>& baseChildren = loaded.baseLayer.children;
    ASSERT_EQ( 2u, baseChildren.size() );
    EXPECT_EQ(Serialization::eRotoLayerChildTypeBezier, baseChildren.front().type);
    EXPECT_EQ( baseEllipse->getScriptName(), baseChildren.front().bezier.name );
    ASSERT_EQ(Serialization::eRotoLayerChildTypeLayer, baseChildren.back().type);

    const Serialization::RotoLayerSerialization& outerData = baseChildren.back().layer;
    EXPECT_EQ( outerLayer->getScriptName(), outerData.name );
    EXPECT_EQ( baseLayer->getScriptName(), outerData.parentLayerName );
    ASSERT_EQ( 2u, outerData.children.size() );
    EXPECT_EQ(Serialization::eRotoLayerChildTypeStroke, outerData.children.front().type);
    ASSERT_EQ(Serialization::eRotoLayerChildTypeLayer, outerData.children.back().type);

    const Serialization::RotoLayerSerialization& innerData = outerData.children.back().layer;
    EXPECT_EQ( innerLayer->getScriptName(), innerData.name );
    EXPECT_EQ( outerLayer->getScriptName(), innerData.parentLayerName );
    ASSERT_EQ( 1u, innerData.children.size() );
    EXPECT_EQ(Serialization::eRotoLayerChildTypeBezier, innerData.children.front().type);
    EXPECT_EQ( innerSquare->getScriptName(), innerData.children.front().bezier.name );
}
