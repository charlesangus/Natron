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

#include <map>
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxNatron.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawDevelop.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Project.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kRawPath = "/shots/raw/IMG_0001.CR2";

std::map<std::string, int>
countByPlugin(const ProjectPtr& project)
{
    std::map<std::string, int> counts;

    for (const NodePtr& node : project->getNodes()) {
        ++counts[node->getPluginID()];
    }

    return counts;
}
} // namespace

class RawDevelopTest
    : public BaseTest {
protected:
    NodePtr createRawRead()
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ));

        EXPECT_TRUE(bool(read));
        if (read) {
            KnobFilePtr file = read->getEffectInstance()->getKnobByNameAndType<KnobFile>(kOfxImageEffectFileParamName);
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(std::string(kRawPath));
            }
        }

        return read;
    }
};

TEST_F(RawDevelopTest, BuildsTheFiveNodesInOrderInABackdrop)
{
    NodePtr read = createRawRead();
    ASSERT_TRUE(bool(read));
    EXPECT_EQ(eDataKindRaw, read->getEffectiveOutputDataKind());

    const RawDevelop::Chain chain = RawDevelop::buildChain(read);
    ASSERT_TRUE(chain.complete());

    EXPECT_EQ(std::string(PLUGINID_NATRON_RAWPREPARE), chain.prepare->getPluginID());
    EXPECT_EQ(std::string(PLUGINID_NATRON_RAWTEMPERATURE), chain.temperature->getPluginID());
    EXPECT_EQ(std::string(PLUGINID_NATRON_RAWHIGHLIGHTS), chain.highlights->getPluginID());
    EXPECT_EQ(std::string(PLUGINID_NATRON_RAWDEMOSAIC), chain.demosaic->getPluginID());
    EXPECT_EQ(std::string(PLUGINID_NATRON_RAWINPUTCOLOR), chain.inputColor->getPluginID());
    EXPECT_EQ(std::string(PLUGINID_NATRON_BACKDROP), chain.backdrop->getPluginID());
    EXPECT_EQ(std::string(kRawDevelopBackdropTitle), chain.backdrop->getLabel());

    EXPECT_EQ(read, chain.prepare->getInput(0));
    EXPECT_EQ(chain.prepare, chain.temperature->getInput(0));
    EXPECT_EQ(chain.temperature, chain.highlights->getInput(0));
    EXPECT_EQ(chain.highlights, chain.demosaic->getInput(0));
    EXPECT_EQ(chain.demosaic, chain.inputColor->getInput(0));
    EXPECT_EQ(eDataKindImage, chain.inputColor->getEffectiveOutputDataKind());

    // Loose nodes in the Read's own group, not a group of their own.
    for (const NodePtr& node : { chain.prepare, chain.temperature, chain.highlights, chain.demosaic, chain.inputColor, chain.backdrop }) {
        EXPECT_EQ(read->getGroup(), node->getGroup());
    }

    // Every node at its defaults, which are darktable's.
    EXPECT_EQ((int)eRawTemperatureModeAsShot, chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->getValue());
    EXPECT_EQ((int)eRawHighlightsModeOpposed, chain.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->getValue());
    EXPECT_EQ((int)eRawDemosaicBayerRCD, chain.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod)->getValue());
    EXPECT_EQ((int)eRawDemosaicXTransMarkesteijn1, chain.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamXTransMethod)->getValue());
}

// Only a node the user creates in the interface asks for the chain: never Python's createNode(),
// which is silent, nor a project load or a paste, which carry a serialization.
TEST_F(RawDevelopTest, OnlyInteractiveCreationAsksForTheChain)
{
    CreateNodeArgs interactive(PLUGINID_NATRON_READ, getApp()->getProject());
    EXPECT_TRUE(RawDevelop::isInteractiveArgs(interactive));

    CreateNodeArgs python(PLUGINID_NATRON_READ, getApp()->getProject());
    python.setProperty<bool>(kCreateNodeArgsPropSilent, true);
    EXPECT_FALSE(RawDevelop::isInteractiveArgs(python));

    CreateNodeArgs loaded(PLUGINID_NATRON_READ, getApp()->getProject());
    loaded.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, std::make_shared<Serialization::NodeSerialization>());
    EXPECT_FALSE(RawDevelop::isInteractiveArgs(loaded));

    CreateNodeArgs preview(PLUGINID_NATRON_READ, getApp()->getProject());
    preview.setProperty<bool>(kCreateNodeArgsPropOutOfProject, true);
    EXPECT_FALSE(RawDevelop::isInteractiveArgs(preview));

    // This application has no interface, so even the interface's arguments ask for nothing.
    EXPECT_FALSE(RawDevelop::isInteractiveCreation(getApp(), interactive));
    const std::map<std::string, int> before = countByPlugin(getApp()->getProject());
    NodePtr read = createRawRead();
    ASSERT_TRUE(bool(read));
    std::map<std::string, int> after = countByPlugin(getApp()->getProject());
    EXPECT_EQ(0, after[PLUGINID_NATRON_RAWPREPARE]);
    EXPECT_EQ(before.size() + 1, after.size());
}

TEST_F(RawDevelopTest, LoadingASavedProjectDoesNotAddASecondChain)
{
    NodePtr read = createRawRead();
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(RawDevelop::buildChain(read).complete());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    QString saved;
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(project->saveProject(dirPath, QString::fromUtf8("develop.ntp"), &saved));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, QString::fromUtf8("develop.ntp")));

    std::map<std::string, int> counts = countByPlugin(project);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_READ]);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_RAWPREPARE]);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_RAWTEMPERATURE]);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_RAWHIGHLIGHTS]);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_RAWDEMOSAIC]);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_RAWINPUTCOLOR]);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_BACKDROP]);
}
