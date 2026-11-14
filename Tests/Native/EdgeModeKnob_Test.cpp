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
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/CountingTestEffect.h"
#include "Tests/NativeParity.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Nodes/Transform/Transform.h"

NATRON_NAMESPACE_USING

namespace {

struct ChoiceCase {
    EdgeModeChoice choice;
    const char* id;
};

const ChoiceCase kChoices[] = {
    { EdgeModeChoice::Input, "input" },
    { EdgeModeChoice::Black, "black" },
    { EdgeModeChoice::Stretch, "stretch" },
};

struct NodeCase {
    const char* id;
    int major;
};

const NodeCase kNodes[] = {
    { PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM },
    { PLUGINID_NATRON_REFORMAT, PLUGIN_MAJOR_NATRON_REFORMAT },
    { PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP },
};

const char*
modeName(EdgeMode mode)
{
    return (mode == EdgeMode::Stretch) ? "Stretch" : "Black";
}

EdgeMode
outputEdgeMode(const NodePtr& node)
{
    return node->getEffectInstance()->getTimeInvariantMetadata()->getEdgeMode();
}

} // namespace

static_assert(resolveEdgeMode(EdgeModeChoice::Input, EdgeMode::Stretch) == EdgeMode::Stretch);
static_assert(resolveEdgeMode(EdgeModeChoice::Input, EdgeMode::Black) == EdgeMode::Black);
static_assert(resolveEdgeMode(EdgeModeChoice::Black, EdgeMode::Stretch) == EdgeMode::Black);
static_assert(resolveEdgeMode(EdgeModeChoice::Black, EdgeMode::Black) == EdgeMode::Black);
static_assert(resolveEdgeMode(EdgeModeChoice::Stretch, EdgeMode::Stretch) == EdgeMode::Stretch);
static_assert(resolveEdgeMode(EdgeModeChoice::Stretch, EdgeMode::Black) == EdgeMode::Stretch);
static_assert(edgeModeChoiceFromIndex(0) == EdgeModeChoice::Input);
static_assert(edgeModeChoiceFromIndex(1) == EdgeModeChoice::Black);
static_assert(edgeModeChoiceFromIndex(2) == EdgeModeChoice::Stretch);
static_assert(edgeModeChoiceFromIndex(3) == EdgeModeChoice::Input);
static_assert(edgeModeChoiceFromIndex(-1) == EdgeModeChoice::Input);

TEST(EdgeModeResolver, EveryChoiceAgainstBothInputModes)
{
    for (EdgeMode input : { EdgeMode::Stretch, EdgeMode::Black }) {
        EXPECT_EQ(input, resolveEdgeMode(EdgeModeChoice::Input, input)) << modeName(input);
        EXPECT_EQ(EdgeMode::Black, resolveEdgeMode(EdgeModeChoice::Black, input)) << modeName(input);
        EXPECT_EQ(EdgeMode::Stretch, resolveEdgeMode(EdgeModeChoice::Stretch, input)) << modeName(input);
    }
}

class EdgeModeKnobTest
    : public BaseTest {
protected:
    NodePtr createWithChoice(const NodeCase& n,
                             const char* choiceID)
    {
        NodePtr node = createNode(QString::fromUtf8(n.id), n.major);

        EXPECT_TRUE(bool(node)) << n.id;
        if (node) {
            EXPECT_TRUE(setKnobValue(node, kEdgeModeParam, std::string(choiceID))) << n.id << " " << choiceID;
        }

        return node;
    }
};

TEST_F(EdgeModeKnobTest, TheKnobIsAnUnanimatedChoiceDefaultingToInput)
{
    for (const NodeCase& n : kNodes) {
        NodePtr node = createNode(QString::fromUtf8(n.id), n.major);
        ASSERT_TRUE(bool(node)) << n.id;
        KnobChoice* knob = dynamic_cast<KnobChoice*>(node->getKnobByName(kEdgeModeParam).get());
        ASSERT_TRUE(knob != NULL) << n.id;
        EXPECT_EQ(std::string(kEdgeModeParamLabel), knob->getLabel()) << n.id;
        EXPECT_EQ((int)EdgeModeChoice::Input, knob->getDefaultValue(0)) << n.id;
        EXPECT_EQ((int)EdgeModeChoice::Input, knob->getValue()) << n.id;
        EXPECT_FALSE(knob->isAnimationEnabled()) << n.id;
        EXPECT_TRUE(knob->getIsMetadataSlave()) << n.id;
        const std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();
        ASSERT_EQ(3u, entries.size()) << n.id;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            EXPECT_EQ(std::string(kChoices[i].id), entries[i].id) << n.id;
            EXPECT_EQ((int)kChoices[i].choice, (int)i) << n.id;
        }
    }
}

// The output carries the input's mode under Input and the forced mode otherwise, whatever the
// input carries.
TEST_F(EdgeModeKnobTest, TheOutputCarriesTheResolvedMode)
{
    for (const NodeCase& n : kNodes) {
        for (EdgeMode input : { EdgeMode::Stretch, EdgeMode::Black }) {
            for (const ChoiceCase& c : kChoices) {
                const std::string what = std::string(n.id) + " " + c.id + " over " + modeName(input);
                NodePtr source = createNode(QString::fromUtf8(kTestPluginIDCounting));
                ASSERT_TRUE(bool(source)) << what;
                ASSERT_TRUE(setKnobValues(source, "blackEdge", { (input == EdgeMode::Black) ? 1. : 0. })) << what;
                NodePtr node = createWithChoice(n, c.id);
                ASSERT_TRUE(bool(node)) << what;
                connectNodes(source, node, 0, true);

                EXPECT_EQ(input, outputEdgeMode(source)) << what;
                EXPECT_EQ(resolveEdgeMode(c.choice, input), outputEdgeMode(node)) << what;
            }
        }
    }
}

// Unconnected, a node under Input carries Stretch, and a forced mode still governs the output.
TEST_F(EdgeModeKnobTest, AnUnconnectedNodeResolvesAgainstStretch)
{
    for (const NodeCase& n : kNodes) {
        for (const ChoiceCase& c : kChoices) {
            NodePtr node = createWithChoice(n, c.id);
            ASSERT_TRUE(bool(node)) << n.id;
            EXPECT_EQ(resolveEdgeMode(c.choice, EdgeMode::Stretch), outputEdgeMode(node)) << n.id << " " << c.id;
        }
    }
}

// Under Input the mode follows the source through a chain; a forced node changes it for every
// node downstream of it.
TEST_F(EdgeModeKnobTest, AForcedModeIsPassedDownTheChain)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(setKnobValues(source, "blackEdge", { 1. }));
    NodePtr first = createWithChoice(kNodes[0], "input");
    NodePtr forced = createWithChoice(kNodes[2], "stretch");
    NodePtr last = createWithChoice(kNodes[1], "input");
    ASSERT_TRUE(first && forced && last);
    connectNodes(source, first, 0, true);
    connectNodes(first, forced, 0, true);
    connectNodes(forced, last, 0, true);

    EXPECT_EQ(EdgeMode::Black, outputEdgeMode(first));
    EXPECT_EQ(EdgeMode::Stretch, outputEdgeMode(forced));
    EXPECT_EQ(EdgeMode::Stretch, outputEdgeMode(last));
}
