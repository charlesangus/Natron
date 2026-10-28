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
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxNatron.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/DenoiseProfiled.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawDevelop.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
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

TEST_F(RawDevelopTest, BuildsTheSixNodesInOrderInABackdrop)
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
    EXPECT_EQ(std::string(PLUGINID_NATRON_RAWCOLORCALIBRATION), chain.colorCalibration->getPluginID());
    EXPECT_EQ(std::string(PLUGINID_NATRON_BACKDROP), chain.backdrop->getPluginID());
    EXPECT_EQ(std::string(kRawDevelopBackdropTitle), chain.backdrop->getLabel());

    EXPECT_EQ(read, chain.prepare->getInput(0));
    EXPECT_EQ(chain.prepare, chain.temperature->getInput(0));
    EXPECT_EQ(chain.temperature, chain.highlights->getInput(0));
    EXPECT_EQ(chain.highlights, chain.demosaic->getInput(0));
    EXPECT_EQ(chain.demosaic, chain.inputColor->getInput(0));
    EXPECT_EQ(chain.inputColor, chain.colorCalibration->getInput(0));
    EXPECT_EQ(eDataKindImage, chain.inputColor->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindImage, chain.colorCalibration->getEffectiveOutputDataKind());

    // Loose nodes in the Read's own group, not a group of their own.
    for (const NodePtr& node : { chain.prepare, chain.temperature, chain.highlights, chain.demosaic, chain.inputColor, chain.colorCalibration, chain.backdrop }) {
        EXPECT_EQ(read->getGroup(), node->getGroup());
    }

    // Every node at its defaults, which are darktable's under its scene-referred workflow.
    EXPECT_EQ((int)eRawTemperatureModeAsShotToReference, chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->getValue());
    EXPECT_EQ((int)eRawHighlightsModeOpposed, chain.highlights->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->getValue());
    EXPECT_EQ((int)eRawDemosaicBayerRCD, chain.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod)->getValue());
    EXPECT_EQ((int)eRawDemosaicXTransMarkesteijn1, chain.demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamXTransMethod)->getValue());
}

// The developRawDenoise preference puts DenoiseProfiled where darktable's pipe runs its denoise
// (profiled): on camera RGB, after the demosaic and before the input colour profile.
TEST_F(RawDevelopTest, DenoisePreferenceInsertsDenoiseProfiledAfterTheDemosaic)
{
    KnobBoolPtr preference = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobBool>("developRawDenoise");
    ASSERT_TRUE(bool(preference));
    EXPECT_FALSE(preference->getDefaultValue(0));
    EXPECT_FALSE(appPTR->getCurrentSettings()->isDevelopRawDenoiseEnabled());

    NodePtr plainRead = createRawRead();
    ASSERT_TRUE(bool(plainRead));
    const RawDevelop::Chain plain = RawDevelop::buildChain(plainRead);
    ASSERT_TRUE(plain.complete());
    EXPECT_FALSE(bool(plain.denoise));
    EXPECT_EQ(plain.demosaic, plain.inputColor->getInput(0));

    preference->setValue(true);
    NodePtr read = createRawRead();
    RawDevelop::Chain chain;
    if (read) {
        chain = RawDevelop::buildChain(read);
    }
    preference->setValue(false);
    ASSERT_TRUE(bool(read));
    ASSERT_TRUE(chain.complete());
    ASSERT_TRUE(bool(chain.denoise));

    EXPECT_EQ(std::string(PLUGINID_NATRON_DENOISEPROFILED), chain.denoise->getPluginID());
    EXPECT_EQ(chain.highlights, chain.demosaic->getInput(0));
    EXPECT_EQ(chain.demosaic, chain.denoise->getInput(0));
    EXPECT_EQ(chain.denoise, chain.inputColor->getInput(0));
    EXPECT_EQ(chain.inputColor, chain.colorCalibration->getInput(0));
    EXPECT_EQ(read->getGroup(), chain.denoise->getGroup());
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
    const std::map<std::string, int> after = countByPlugin(getApp()->getProject());
    EXPECT_EQ(0u, after.count(PLUGINID_NATRON_RAWPREPARE));
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
    EXPECT_EQ(1, counts[PLUGINID_NATRON_RAWCOLORCALIBRATION]);
    EXPECT_EQ(1, counts[PLUGINID_NATRON_BACKDROP]);
}

// RawTemperature defaults to As Shot to Reference wherever it is created, darktable's default
// under its default scene-referred workflow, not only in the Develop chain.
TEST_F(RawDevelopTest, AFreshRawTemperatureDefaultsToAsShotToReference)
{
    NodePtr temperature = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWTEMPERATURE));
    ASSERT_TRUE(bool(temperature));
    KnobChoicePtr mode = temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode);
    ASSERT_TRUE(bool(mode));
    EXPECT_EQ((int)eRawTemperatureModeAsShotToReference, mode->getValue());
    EXPECT_EQ((int)eRawTemperatureModeAsShotToReference, mode->getDefaultValue(0));
}

// Without an interface every node is 0 x 0 at the origin, so the layout buildChain() applies is
// checked on its own, with the sizes a node graph gives.
TEST_F(RawDevelopTest, TheLayoutKeepsTheSixNodesInOrderInsideTheBackdrop)
{
    const double readX = 100., readY = -40., readW = 104., readH = 34.;
    const std::vector<RawDevelop::NodeSize> sizes = { { 104., 34. }, { 120., 34. }, { 104., 50. }, { 104., 34. }, { 140., 34. }, { 150., 40. } };
    const RawDevelop::ChainLayout layout = RawDevelop::layoutChain(readX, readY, readW, readH, sizes);

    ASSERT_EQ(sizes.size(), layout.nodes.size());
    const RawDevelop::GraphRect& b = layout.backdrop;
    // Below the Read, which stays outside.
    EXPECT_GE(b.y, readY + readH);
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const RawDevelop::GraphRect& n = layout.nodes[i];
        EXPECT_EQ(sizes[i].width, n.width) << i;
        EXPECT_EQ(sizes[i].height, n.height) << i;
        // Inside the backdrop, under its title bar.
        EXPECT_GT(n.y, b.y) << i;
        EXPECT_GE(n.x, b.x) << i;
        EXPECT_LE(n.x + n.width, b.x + b.width) << i;
        EXPECT_LE(n.y + n.height, b.y + b.height) << i;
        // Centred under the Read.
        EXPECT_DOUBLE_EQ(readX + readW / 2., n.x + n.width / 2.) << i;
        // Each below the one before, without overlapping it.
        if (i > 0) {
            const RawDevelop::GraphRect& above = layout.nodes[i - 1];
            EXPECT_GT(n.y, above.y + above.height) << i;
        }
    }
}
