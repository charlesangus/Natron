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

#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawHighlights.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 16;
const int kHeight = 16;

ImageMetadata
sourceMetadata(const std::string& pattern,
               std::optional<double> linearResponseLimit)
{
    ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStagePrepared);

    RawMetadata::setWhiteLevel(&metadata, 1.);
    if (linearResponseLimit) {
        RawMetadata::setLinearResponseLimit(&metadata, *linearResponseLimit);
    }

    return metadata;
}
} // namespace

class NativeRawHighlightsDefaultsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createSource(const std::string& pattern,
                         int nComps,
                         std::optional<double> linearResponseLimit)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (source) {
            dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), nComps, [](int, int, int) { return 0.5f; }, sourceMetadata(pattern, linearResponseLimit));
        }

        return source;
    }

    NodePtr createHighlights(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));

        EXPECT_TRUE(bool(node));
        if (node) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    static double clipOf(const NodePtr& node)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamClip)->getValue();
    }
};

TEST_F(NativeRawHighlightsDefaultsTest, ClipDefaultsToTheLinearResponseLimit)
{
    NodePtr limited = createHighlights(createSource("RGGB", 1, 0.8));
    ASSERT_TRUE(bool(limited));
    EXPECT_DOUBLE_EQ(0.8, clipOf(limited));

    NodePtr unlimited = createHighlights(createSource("RGGB", 1, std::nullopt));
    ASSERT_TRUE(bool(unlimited));
    EXPECT_DOUBLE_EQ(1., clipOf(unlimited));

    NodePtr above = createHighlights(createSource("RGGB", 1, 1.4));
    ASSERT_TRUE(bool(above));
    EXPECT_DOUBLE_EQ(1., clipOf(above));
}

TEST_F(NativeRawHighlightsDefaultsTest, TheShownClipFollowsTheInputUntilOverridden)
{
    NodePtr first = createSource("RGGB", 1, 0.8);
    NodePtr node = createHighlights(first);
    ASSERT_TRUE(bool(node));
    EXPECT_DOUBLE_EQ(0.8, clipOf(node));

    disconnectNodes(first, node, true);
    connectNodes(createSource("RGGB", 1, 0.6), node, 0, true);
    EXPECT_DOUBLE_EQ(0.6, clipOf(node));
}

TEST_F(NativeRawHighlightsDefaultsTest, AnOverrideSticksAcrossInputChanges)
{
    NodePtr first = createSource("RGGB", 1, 0.8);
    NodePtr node = createHighlights(first);
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    effect->getKnobByNameAndType<KnobBool>(kRawHighlightsParamClipOverride)->setValue(true);
    effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamClip)->setValue(0.5);
    EXPECT_DOUBLE_EQ(0.5, clipOf(node));

    disconnectNodes(first, node, true);
    connectNodes(createSource("RGGB", 1, 0.6), node, 0, true);
    EXPECT_DOUBLE_EQ(0.5, clipOf(node));

    effect->getKnobByNameAndType<KnobBool>(kRawHighlightsParamClipOverride)->setValue(false);
    EXPECT_DOUBLE_EQ(0.6, clipOf(node));
}

TEST_F(NativeRawHighlightsDefaultsTest, LChWithoutACfaFallsBackToOpposedWithAWarning)
{
    NodePtr node = createHighlights(createSource("", 3, std::nullopt));
    ASSERT_TRUE(bool(node));
    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeLCh);

    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("falling back to inpaint opposed")));

    NodePtr mosaic = createHighlights(createSource("RGGB", 1, std::nullopt));
    ASSERT_TRUE(bool(mosaic));
    mosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeLCh);
    EXPECT_FALSE(mosaic->hasPersistentMessage());
}

TEST(NativeRawHighlightsModeRules, ModesDeclareTheSensorTheyNeed)
{
    const RawLayout bayer = *RawLayout::fromPattern("RGGB");
    const RawLayout linear;

    EXPECT_TRUE(rawHighlightsModeAvailable(eRawHighlightsModeLCh, bayer));
    EXPECT_FALSE(rawHighlightsModeAvailable(eRawHighlightsModeLCh, linear));
    EXPECT_TRUE(rawHighlightsModeAvailable(eRawHighlightsModeOpposed, linear));
    EXPECT_TRUE(rawHighlightsModeAvailable(eRawHighlightsModeClip, linear));

    bool fellBack = false;
    EXPECT_EQ(eRawHighlightsModeOpposed, sanitiseRawHighlightsMode(eRawHighlightsModeLCh, linear, &fellBack));
    EXPECT_TRUE(fellBack);
    EXPECT_EQ(eRawHighlightsModeLCh, sanitiseRawHighlightsMode(eRawHighlightsModeLCh, bayer, &fellBack));
    EXPECT_FALSE(fellBack);
}
