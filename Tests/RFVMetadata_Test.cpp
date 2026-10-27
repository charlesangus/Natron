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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kFormatWidth = 64;
const int kFormatHeight = 48;

} // namespace

// What metadata driven by a variable sees: a format and a frame range follow the Set below the
// node, as metadata, regions, layers and time domains are read under the context each input is
// requested under.
class RFVMetadata
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kFormatWidth, kFormatHeight, "rfvMetadataFormat", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A Set node over input, with no variable yet.
    NodePtr createSet(const NodePtr& input)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
        if (!set) {
            ADD_FAILURE() << "no Set node";

            return NodePtr();
        }
        connectNodes(input, set, 0, true);

        return set;
    }

    // Makes knob, a user knob of the Set node, one of its variables.
    bool declare(const NodePtr& set,
                 const KnobIPtr& knob)
    {
        KnobGroupPtr variables = std::dynamic_pointer_cast<KnobGroup>(set->getKnobByName(kReverseFlowVariableSetParamVariables));
        if (!variables || !variables->addKnob(knob)) {
            return false;
        }
        set->getEffectInstance()->recreateUserKnobs(false);

        return true;
    }
};

// A Crop reformatting to a width of 32 while rfv.var is 'a' and 16 otherwise, under Set(var = 'a')
// and Set(var = 'b'): each Set passes on the format its value gives, and each renders the same in
// both schedulers.
TEST_F(RFVMetadata, FormatFollowsTheSetAndRendersInBothSchedulers)
{
    NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);
    ASSERT_TRUE(checker && crop);
    connectNodes(checker, crop, 0, true);
    KnobBoolPtr reformat = std::dynamic_pointer_cast<KnobBool>(crop->getKnobByName(kCropParamReformat));
    KnobDoublePtr bottomLeft = std::dynamic_pointer_cast<KnobDouble>(crop->getKnobByName(kNativeGeneratorParamBottomLeft));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(crop->getKnobByName(kNativeGeneratorParamSize));
    ASSERT_TRUE(reformat && bottomLeft && size);
    reformat->setValue(true);
    bottomLeft->setValue(0., ViewSpec::all(), 0);
    bottomLeft->setValue(0., ViewSpec::all(), 1);
    size->setValue(24., ViewSpec::all(), 1);
    ASSERT_NO_THROW(size->setExpression(0, "32 if rfv.var == 'a' else 16", false, true));

    NodePtr setA = createSet(crop);
    NodePtr setB = createSet(crop);
    ASSERT_TRUE(setA && setB);
    KnobStringPtr varA = setA->getEffectInstance()->createStringKnob("var", "var");
    KnobStringPtr varB = setB->getEffectInstance()->createStringKnob("var", "var");
    ASSERT_TRUE(declare(setA, varA));
    ASSERT_TRUE(declare(setB, varB));
    varA->setValue("a");
    varB->setValue("b");

    const NodePtr sets[2] = { setA, setB };
    const int widths[2] = { 32, 16 };
    for (int b = 0; b < 2; ++b) {
        NodeMetadataConstPtr metadata = sets[b]->getEffectInstance()->getMetadata(1., ViewIdx(0));
        ASSERT_TRUE(bool(metadata));
        EXPECT_EQ(RectI(0, 0, widths[b], 24), metadata->getOutputFormat()) << sets[b]->getScriptName_mt_safe();

        const RenderMismatch m = renderBothWaysDirect(sets[b], 1., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, 16, 24), std::vector<int> { 1, 4 });
        EXPECT_FALSE(m.any) << sets[b]->getScriptName_mt_safe() << ": " << describe(m);
    }
}

// A Checkerboard whose last frame reads rfv.last, with the project's last = 10, under Set(last = 20)
// and Set(last = 30): the bare Checkerboard's time domain is the project's, each Set's follows its
// own value, and each Set renders the same in both schedulers.
TEST_F(RFVMetadata, FrameRangeFollowsTheSetAndRendersInBothSchedulers)
{
    ProjectPtr project = getApp()->getProject();
    KnobDoublePtr projectLast = project->createDoubleKnob("last", "last", 1);
    std::string error;
    ASSERT_TRUE(RFV::addAsVariable(project.get(), projectLast, &error)) << error;
    projectLast->setValue(10.);

    NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    ASSERT_TRUE(bool(checker));
    KnobIntPtr frameRange = std::dynamic_pointer_cast<KnobInt>(checker->getKnobByName(kNativeGeneratorParamFrameRange));
    ASSERT_TRUE(bool(frameRange));
    frameRange->setValue(1, ViewSpec::all(), 0);
    ASSERT_NO_THROW(frameRange->setExpression(1, "int(rfv.last)", false, true));

    NodePtr setA = createSet(checker);
    NodePtr setB = createSet(checker);
    ASSERT_TRUE(setA && setB);
    KnobDoublePtr lastA = setA->getEffectInstance()->createDoubleKnob("last", "last", 1);
    KnobDoublePtr lastB = setB->getEffectInstance()->createDoubleKnob("last", "last", 1);
    ASSERT_TRUE(declare(setA, lastA));
    ASSERT_TRUE(declare(setB, lastB));
    lastA->setValue(20.);
    lastB->setValue(30.);

    const NodePtr nodes[3] = { checker, setA, setB };
    const double lasts[3] = { 10., 20., 30. };
    // Twice: the second pass answers from the actions caches, which must not mix the paths.
    for (int pass = 0; pass < 2; ++pass) {
        for (int n = 0; n < 3; ++n) {
            EffectInstancePtr effect = nodes[n]->getEffectInstance();
            double first = 0., last = 0.;
            effect->getFrameRange_public(effect->getRenderHash(), &first, &last);
            EXPECT_EQ(1., first) << nodes[n]->getScriptName_mt_safe() << " pass " << pass;
            EXPECT_EQ(lasts[n], last) << nodes[n]->getScriptName_mt_safe() << " pass " << pass;
        }
    }

    const NodePtr sets[2] = { setA, setB };
    for (const NodePtr& set : sets) {
        const RenderMismatch m = renderBothWaysDirect(set, 1., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kFormatWidth, kFormatHeight), std::vector<int> { 1, 4 });
        EXPECT_FALSE(m.any) << set->getScriptName_mt_safe() << ": " << describe(m);
    }
}
