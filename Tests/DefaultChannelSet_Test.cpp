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
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kProcessesAllLayers[] = {
    "fr.natron.test.TwoInputCopy",
    "fr.natron.test.MatrixWarp",
    "fr.natron.test.FrameWindow",
    "Transform",
    "net.sf.openfx.TransformMaskedPlugin",
    "Crop",
    "Position",
    "Reformat",
    "net.sf.openfx.Card3D",
    "net.sf.openfx.SpriteSheet",
    "fr.inria.openfx.OIIOResize",
    "net.fxarena.openfx.Reflection",
    "net.fxarena.openfx.Tile",
    "net.fxarena.openfx.Roll",
    "net.fxarena.openfx.Arc",
    "net.fxarena.openfx.Polar",
    "net.fxarena.openfx.Implode",
    "net.fxarena.openfx.Swirl",
    "net.fxarena.openfx.Wave",
    "net.sf.openfx.Deinterlace",
    "Blur",
    "net.sf.cimg.CImgErode",
    "net.sf.cimg.CImgDilate",
    "net.sf.cimg.CImgErodeSmooth",
    "net.sf.cimg.CImgDenoise",
    "net.sf.cimg.CImgSmooth",
    "net.sf.cimg.CImgSharpenInvDiff",
    "net.sf.cimg.CImgSharpenShock",
    "eu.cimg.Inpaint",
    "eu.cimg.ErodeBlur",
    "eu.cimg.Distance",
    "eu.cimg.CImgMatrix3x3",
    "eu.cimg.CImgMatrix5x5",
    "net.fxarena.openfx.Oilpaint",
    "net.fxarena.openfx.Charcoal",
    "net.fxarena.openfx.Edges",
    "net.fxarena.openfx.Sketch",
    "net.fxarena.openfx.Morphology",
    "Dissolve",
    "net.sf.openfx.CopyRectanglePlugin",
    "net.sf.openfx.ContactSheetOFX",
    "net.sf.openfx.LayerContactSheetOFX",
    "net.sf.openfx.NoOpPlugin",
    "net.sf.openfx.sideBySidePlugin",
    "net.sf.openfx.reConvergePlugin",
    "net.sf.openfx.mixViewsPlugin",
};

// Includes the Filter/Transform-grouped plug-ins that interpret R, G and B as colour or run
// user code, which the grouping fallback must not catch.
const char* const kKeepsColorLayer[] = {
    "fr.natron.test.PointMatrix",
    "fr.natron.test.PointMatrix1",
    "Grade",
    "ColorCorrect",
    "Invert",
    "Multiply",
    "Saturation",
    "Clamp",
    "ColorLookup",
    "Merge",
    "Keyer",
    "ChromaKeyer",
    "net.sf.openfx.anaglyphPlugin",
    "net.sf.openfx.ImageStatistics",
    "net.sf.openfx.ClipTestPlugin",
    "net.sf.openfx.Shadertoy",
    "net.sf.openfx.DenoiseSharpen",
    "net.sf.cimg.CImgExpression",
    "net.sf.cimg.CImgChromaBlur",
    "EdgeDetect",
};

bool
isPluginLoaded(const char* pluginID)
{
    try {
        return appPTR->getPluginBinary(QString::fromUtf8(pluginID), -1, -1, false) != NULL;
    } catch (const std::exception&) {
        return false;
    }
}

KnobChannelSetPtr
channelSetOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
}

bool
isColorRow(const ChannelSetRow& row)
{
    return row.mode == ChannelSetRow::eModeLayer && row.layerOrPattern == kNatronColorViewRGBA;
}

} // namespace

class DefaultChannelSetTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // Saves the project into `tmp`, resets it and loads the file back.
    bool saveResetLoad(const QTemporaryDir& tmp)
    {
        ProjectPtr project = getApp()->getProject();
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("default-channel-set.ntpz");
        QString savedFilePath;

        if (!project->saveProject(dirPath, fileName, &savedFilePath) || !QFile::exists(savedFilePath)) {
            return false;
        }
        project->reset(false, true);

        return project->loadProject(dirPath, fileName);
    }
};

TEST_F(DefaultChannelSetTest, ChannelAgnosticNodesDefaultToAll)
{
    int checked = 0;

    for (std::size_t i = 0; i < sizeof(kProcessesAllLayers) / sizeof(kProcessesAllLayers[0]); ++i) {
        const char* pluginID = kProcessesAllLayers[i];
        if (!isPluginLoaded(pluginID)) {
            continue;
        }
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        ASSERT_TRUE(bool(node)) << pluginID;
        EXPECT_TRUE(node->getEffectInstance()->defaultProcessesAllLayers()) << pluginID;

        // Multi-planar plug-ins (LayerContactSheet) own their planes and get no channel set.
        KnobChannelSetPtr channels = channelSetOf(node);
        if (!channels) {
            continue;
        }
        std::vector<ChannelSetRow> rows = channels->getRows();
        ASSERT_EQ(1u, rows.size()) << pluginID;
        EXPECT_EQ(ChannelSetRow::eModeAll, rows[0].mode) << pluginID;
        EXPECT_FALSE(channels->hasModifications()) << pluginID;
        ++checked;
    }

    EXPECT_TRUE(isPluginLoaded("Blur"));
    EXPECT_TRUE(isPluginLoaded("Transform"));
    EXPECT_TRUE(isPluginLoaded("fr.natron.test.TwoInputCopy"));
    EXPECT_GE(checked, 3);
}

TEST_F(DefaultChannelSetTest, ColourNodesKeepTheColorLayerDefault)
{
    int checked = 0;

    for (std::size_t i = 0; i < sizeof(kKeepsColorLayer) / sizeof(kKeepsColorLayer[0]); ++i) {
        const char* pluginID = kKeepsColorLayer[i];
        if (!isPluginLoaded(pluginID)) {
            continue;
        }
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        ASSERT_TRUE(bool(node)) << pluginID;
        EXPECT_FALSE(node->getEffectInstance()->defaultProcessesAllLayers()) << pluginID;

        KnobChannelSetPtr channels = channelSetOf(node);
        if (!channels) {
            continue;
        }
        std::vector<ChannelSetRow> rows = channels->getRows();
        ASSERT_FALSE(rows.empty()) << pluginID;
        EXPECT_TRUE(isColorRow(rows[0])) << pluginID;
        ++checked;
    }

    EXPECT_TRUE(isPluginLoaded("Grade"));
    EXPECT_GE(checked, 1);
}

TEST_F(DefaultChannelSetTest, RotoKeepsItsAlphaTargetDefault)
{
    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));

    ASSERT_TRUE(bool(roto));
    EXPECT_FALSE(roto->getEffectInstance()->defaultProcessesAllLayers());
    EXPECT_FALSE(bool(channelSetOf(roto)));

    KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(roto->getKnobByName(kNodeParamLayerSelect));
    ASSERT_TRUE(bool(layer));
    EXPECT_EQ(std::string(kNatronColorViewRGBA), layer->getLayer());
    std::vector<std::string> alpha;
    alpha.push_back("A");
    EXPECT_EQ(alpha, layer->getChannels());
}

TEST_F(DefaultChannelSetTest, SetChannelsOnAllDefaultBlurPinsToColor)
{
    if (!isPluginLoaded("Blur")) {
        std::cerr << "Skipping: Blur is not loaded." << std::endl;
        return;
    }

    NodePtr blur = createNode(QString::fromUtf8("Blur"));
    ASSERT_TRUE(bool(blur));
    KnobChannelSetPtr channels = channelSetOf(blur);
    ASSERT_TRUE(bool(channels));
    ASSERT_EQ(ChannelSetRow::eModeAll, channels->getRows()[0].mode);

    std::vector<std::string> rgba;
    rgba.push_back("R");
    rgba.push_back("G");
    rgba.push_back("B");
    rgba.push_back("A");
    channels->setChannels(0, rgba);

    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_TRUE(isColorRow(rows[0]));
    EXPECT_EQ(rgba, rows[0].channels);
}

TEST_F(DefaultChannelSetTest, SetChannelsOnAllDefaultBlurWithSingleChannelPinsToColor)
{
    if (!isPluginLoaded("Blur")) {
        std::cerr << "Skipping: Blur is not loaded." << std::endl;
        return;
    }

    NodePtr blur = createNode(QString::fromUtf8("Blur"));
    ASSERT_TRUE(bool(blur));
    KnobChannelSetPtr channels = channelSetOf(blur);
    ASSERT_TRUE(bool(channels));
    ASSERT_EQ(ChannelSetRow::eModeAll, channels->getRows()[0].mode);

    std::vector<std::string> alpha;
    alpha.push_back("A");
    channels->setChannels(0, alpha);

    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_TRUE(isColorRow(rows[0]));
    EXPECT_EQ(alpha, rows[0].channels);
}

TEST_F(DefaultChannelSetTest, SetChannelsOnNoneRowOfADefaultAllBlurPinsToColor)
{
    if (!isPluginLoaded("Blur")) {
        std::cerr << "Skipping: Blur is not loaded." << std::endl;
        return;
    }

    NodePtr blur = createNode(QString::fromUtf8("Blur"));
    ASSERT_TRUE(bool(blur));
    KnobChannelSetPtr channels = channelSetOf(blur);
    ASSERT_TRUE(bool(channels));

    channels->setNone();
    std::vector<std::string> alpha;
    alpha.push_back("A");
    channels->setChannels(0, alpha);

    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_TRUE(isColorRow(rows[0]));
    EXPECT_EQ(alpha, rows[0].channels);
}

TEST_F(DefaultChannelSetTest, ChannelSetIsSavedEvenOnItsDefault)
{
    NodePtr blur = createNode(QString::fromUtf8("Blur"));
    NodePtr grade = createNode(QString::fromUtf8("Grade"));

    ASSERT_TRUE(bool(blur) && bool(grade));

    const NodePtr nodes[2] = { blur, grade };
    for (int i = 0; i < 2; ++i) {
        KnobChannelSetPtr channels = channelSetOf(nodes[i]);
        ASSERT_TRUE(bool(channels));
        EXPECT_FALSE(channels->hasModifications());

        Serialization::NodeSerialization serialization;
        nodes[i]->toSerialization(&serialization);

        bool saved = false;
        const std::list<Serialization::KnobSerialization>& values = serialization.knobs;
        for (std::list<Serialization::KnobSerialization>::const_iterator it = values.begin(); it != values.end(); ++it) {
            if (it->name == std::string(kNodeParamChannelSet)) {
                saved = true;
            }
        }
        EXPECT_TRUE(saved) << nodes[i]->getPluginID();
    }
}

TEST_F(DefaultChannelSetTest, DefaultBlurRoundTripsAsAll)
{
    NodePtr blur = createNode(QString::fromUtf8("Blur"));

    ASSERT_TRUE(bool(blur));
    const std::string name = blur->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));

    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    KnobChannelSetPtr channels = channelSetOf(loaded);
    ASSERT_TRUE(bool(channels));
    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeAll, rows[0].mode);
}

TEST_F(DefaultChannelSetTest, BlurSetToColorStaysColorAfterRoundTrip)
{
    NodePtr blur = createNode(QString::fromUtf8("Blur"));

    ASSERT_TRUE(bool(blur));
    const std::string name = blur->getScriptName();
    KnobChannelSetPtr channels = channelSetOf(blur);
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, kNatronColorViewRGBA, NULL);
    ASSERT_TRUE(isColorRow(channels->getRows()[0]));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));

    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    KnobChannelSetPtr loadedChannels = channelSetOf(loaded);
    ASSERT_TRUE(bool(loadedChannels));
    std::vector<ChannelSetRow> rows = loadedChannels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_TRUE(isColorRow(rows[0]));
    EXPECT_TRUE(rows[0].channels.empty());
}

TEST_F(DefaultChannelSetTest, DefaultGradeStaysColorAfterRoundTrip)
{
    NodePtr grade = createNode(QString::fromUtf8("Grade"));

    ASSERT_TRUE(bool(grade));
    const std::string name = grade->getScriptName();
    KnobChannelSetPtr channels = channelSetOf(grade);
    ASSERT_TRUE(bool(channels));
    const std::vector<ChannelSetRow> before = channels->getRows();
    ASSERT_EQ(1u, before.size());
    ASSERT_TRUE(isColorRow(before[0]));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));

    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    KnobChannelSetPtr loadedChannels = channelSetOf(loaded);
    ASSERT_TRUE(bool(loadedChannels));
    EXPECT_EQ(before, loadedChannels->getRows());
}
