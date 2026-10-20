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

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Channel/RemoveLayers.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

struct ExpectedNode {
    const char* id;
    const char* label;
    std::vector<const char*> inputLabels;
};

const double kTime = 1.;

class NativeChannelNodesSpecTest
    : public BaseTest {
protected:
    void resetProject()
    {
        getApp()->getProject()->reset(false, true);
    }

    NativeEffectBase* createEffect(const char* pluginID,
                                   NodePtr* node)
    {
        resetProject();
        *node = createNode(QString::fromUtf8(pluginID));
        EXPECT_TRUE(bool(*node)) << pluginID;
        if (!*node) {
            return NULL;
        }
        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>((*node)->getEffectInstance().get());
        EXPECT_TRUE(effect != NULL) << pluginID;

        return effect;
    }

    void checkSpec(const ExpectedNode& expected)
    {
        NodePtr node;
        NativeEffectBase* effect = createEffect(expected.id, &node);

        ASSERT_TRUE(effect != NULL);
        EXPECT_TRUE(effect->nodeSpec() != NULL) << expected.id;
        EXPECT_EQ(std::string(expected.id), effect->getPluginID());
        EXPECT_EQ(std::string(expected.label), effect->getPluginLabel());
        EXPECT_FALSE(effect->getPluginDescription().empty()) << expected.id;
        EXPECT_EQ(1, effect->getMajorVersion());
        EXPECT_EQ(0, effect->getMinorVersion());
        EXPECT_FALSE(effect->isWriter());

        std::list<std::string> grouping;
        effect->getPluginGrouping(&grouping);
        ASSERT_EQ(1u, grouping.size()) << expected.id;
        EXPECT_EQ(std::string(PLUGIN_GROUP_CHANNEL), grouping.front());

        ASSERT_EQ((int)expected.inputLabels.size(), effect->getNInputs()) << expected.id;
        for (int i = 0; i < effect->getNInputs(); ++i) {
            EXPECT_EQ(std::string(expected.inputLabels[i]), effect->getInputLabel(i)) << expected.id << " input " << i;
            EXPECT_TRUE(effect->isInputOptional(i)) << expected.id << " input " << i;
            EXPECT_FALSE(effect->isInputMask(i)) << expected.id << " input " << i;
            EXPECT_EQ(eDataKindImage, effect->getInputDataKind(i)) << expected.id << " input " << i;
        }
        EXPECT_EQ(eDataKindImage, effect->getOutputDataKind());
        EXPECT_TRUE(effect->isMultiPlanar());
    }

    bool identityOf(const char* pluginID,
                    int* inputNb,
                    double* inputTime)
    {
        NodePtr node;
        NativeEffectBase* effect = createEffect(pluginID, &node);

        if (!effect) {
            return false;
        }
        ViewIdx inputView(0);

        *inputNb = -3;
        *inputTime = 0.;

        return effect->isIdentity_public(false, 0, kTime, RenderScale::identity, RectI(0, 0, 8, 8), ViewIdx(0), inputTime, &inputView, inputNb);
    }
};

} // namespace

TEST_F(NativeChannelNodesSpecTest, ShuffleDescribesItself)
{
    checkSpec({ PLUGINID_NATRON_SHUFFLE, "Shuffle", { "Source" } });
}

TEST_F(NativeChannelNodesSpecTest, ShuffleCopyDescribesItself)
{
    checkSpec({ PLUGINID_NATRON_SHUFFLECOPY, "ShuffleCopy", { "2", "1" } });
}

TEST_F(NativeChannelNodesSpecTest, AddLayersDescribesItself)
{
    checkSpec({ PLUGINID_NATRON_ADDLAYERS, "AddLayers", { "Source" } });
}

TEST_F(NativeChannelNodesSpecTest, RemoveLayersDescribesItself)
{
    checkSpec({ PLUGINID_NATRON_REMOVELAYERS, "RemoveLayers", { "Source" } });
}

TEST_F(NativeChannelNodesSpecTest, AddLayersWithNothingToAddRoutesToItsInput)
{
    int inputNb;
    double inputTime;

    EXPECT_TRUE(identityOf(PLUGINID_NATRON_ADDLAYERS, &inputNb, &inputTime));
    EXPECT_EQ(0, inputNb);
    EXPECT_EQ(kTime, inputTime);
}

TEST_F(NativeChannelNodesSpecTest, RemoveLayersWithNothingToRemoveRoutesToItsInput)
{
    int inputNb;
    double inputTime;

    EXPECT_TRUE(identityOf(PLUGINID_NATRON_REMOVELAYERS, &inputNb, &inputTime));
    EXPECT_EQ(0, inputNb);
    EXPECT_EQ(kTime, inputTime);
}

TEST_F(NativeChannelNodesSpecTest, DefaultShuffleRoutesToItsInput)
{
    int inputNb;
    double inputTime;

    EXPECT_TRUE(identityOf(PLUGINID_NATRON_SHUFFLE, &inputNb, &inputTime));
    EXPECT_EQ(0, inputNb);
    EXPECT_EQ(kTime, inputTime);
}

TEST_F(NativeChannelNodesSpecTest, DefaultShuffleCopyComputesItsOutput)
{
    int inputNb;
    double inputTime;

    EXPECT_FALSE(identityOf(PLUGINID_NATRON_SHUFFLECOPY, &inputNb, &inputTime));
}
