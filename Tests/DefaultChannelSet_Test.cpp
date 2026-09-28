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
#include "Engine/NodeSerialization.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kProcessesAllLayers[] = {
    "net.sf.openfx.TransformPlugin",
    "net.sf.openfx.TransformMaskedPlugin",
    "net.sf.openfx.DirBlur",
    "net.sf.openfx.CornerPinPlugin",
    "net.sf.openfx.CornerPinMaskedPlugin",
    "net.sf.openfx.CropPlugin",
    "net.sf.openfx.Position",
    "net.sf.openfx.Reformat",
    "net.sf.openfx.Card3D",
    "net.sf.openfx.AdjustRoDPlugin",
    "net.sf.openfx.SpriteSheet",
    "net.sf.openfx.Mirror",
    "net.sf.openfx.IDistort",
    "net.sf.openfx.STMap",
    "net.sf.openfx.LensDistortion",
    "fr.inria.openfx.OIIOResize",
    "net.fxarena.openfx.Reflection",
    "net.fxarena.openfx.Tile",
    "net.fxarena.openfx.Roll",
    "net.fxarena.openfx.Arc",
    "net.fxarena.openfx.Polar",
    "net.fxarena.openfx.Implode",
    "net.fxarena.openfx.Swirl",
    "net.fxarena.openfx.Wave",
    "net.sf.openfx.FrameHold",
    "net.sf.openfx.timeOffset",
    "net.sf.openfx.FrameRange",
    "net.sf.openfx.AppendClip",
    "net.sf.openfx.Retime",
    "net.sf.openfx.FrameBlend",
    "net.sf.openfx.TimeBlur",
    "net.sf.openfx.NoTimeBlurPlugin",
    "net.sf.openfx.SlitScan",
    "net.sf.openfx.Deinterlace",
    "net.sf.openfx.TimeBufferRead",
    "net.sf.openfx.TimeBufferWrite",
    "net.sf.cimg.CImgBlur",
    "net.sf.cimg.CImgLaplacian",
    "net.sf.cimg.CImgSharpen",
    "net.sf.cimg.CImgSoften",
    "net.sf.cimg.CImgBloom",
    "net.sf.cimg.CImgMedian",
    "net.sf.cimg.CImgErode",
    "net.sf.cimg.CImgDilate",
    "net.sf.cimg.CImgErodeSmooth",
    "net.sf.cimg.CImgBilateral",
    "net.sf.cimg.CImgBilateralGuided",
    "net.sf.cimg.CImgGuided",
    "net.sf.cimg.CImgDenoise",
    "net.sf.cimg.CImgSmooth",
    "net.sf.cimg.CImgRollingGuidance",
    "net.sf.cimg.CImgSharpenInvDiff",
    "net.sf.cimg.CImgSharpenShock",
    "eu.cimg.Inpaint",
    "eu.cimg.ErodeBlur",
    "eu.cimg.Distance",
    "eu.cimg.CImgMatrix3x3",
    "eu.cimg.CImgMatrix5x5",
    "net.sf.openfx.GodRays",
    "net.fxarena.openfx.Oilpaint",
    "net.fxarena.openfx.Charcoal",
    "net.fxarena.openfx.Edges",
    "net.fxarena.openfx.Sketch",
    "net.fxarena.openfx.Morphology",
    "net.sf.openfx.switchPlugin",
    "net.sf.openfx.DissolvePlugin",
    "net.sf.openfx.TimeDissolvePlugin",
    "net.sf.openfx.KeyMix",
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
    "net.sf.openfx.GradePlugin",
    "net.sf.openfx.ColorCorrectPlugin",
    "net.sf.openfx.Invert",
    "net.sf.openfx.MultiplyPlugin",
    "net.sf.openfx.SaturationPlugin",
    "net.sf.openfx.Clamp",
    "net.sf.openfx.ColorLookupPlugin",
    "net.sf.openfx.HSVToolPlugin",
    "net.sf.openfx.MergePlugin",
    "net.sf.openfx.KeyerPlugin",
    "net.sf.openfx.ChromaKeyerPlugin",
    "net.sf.openfx.anaglyphPlugin",
    "net.sf.openfx.ImageStatistics",
    "net.sf.openfx.ClipTestPlugin",
    "net.sf.openfx.Shadertoy",
    "net.sf.openfx.DenoiseSharpen",
    "net.sf.cimg.CImgExpression",
    "net.sf.cimg.CImgChromaBlur",
    "eu.cimg.EdgeDetect",
    "eu.cimg.EdgeExtend",
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

// A row naming the retired colour storage ID, which loading an old project must never leave.
bool
isLegacyColorRow(const ChannelSetRow& row)
{
    return row.mode == ChannelSetRow::eModeLayer && row.layerOrPattern == kNatronColorLayerID;
}

bool
hasLegacyColorWarning(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return type == eMessageTypeWarning && message == QString::fromUtf8("Colour layer from an older project was reset to rgba");
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
        const QString fileName = QString::fromUtf8("default-channel-set.ntp");
        QString savedFilePath;

        if (!project->saveProject(dirPath, fileName, &savedFilePath) || !QFile::exists(savedFilePath)) {
            return false;
        }
        project->reset(false, true);

        return project->loadProject(dirPath, fileName);
    }

    bool loadLegacyFixture(const QTemporaryDir& tmp)
    {
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("channel-set-legacy-defaults.ntp");

        if (!QFile::copy(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/channel-set-legacy-defaults.ntp"), dirPath + fileName)) {
            return false;
        }

        return getApp()->getProject()->loadProject(dirPath, fileName);
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

    EXPECT_TRUE(isPluginLoaded("net.sf.cimg.CImgBlur"));
    EXPECT_TRUE(isPluginLoaded("net.sf.openfx.TransformPlugin"));
    EXPECT_TRUE(isPluginLoaded("net.sf.openfx.switchPlugin"));
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

    EXPECT_TRUE(isPluginLoaded("net.sf.openfx.GradePlugin"));
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
    if (!isPluginLoaded("net.sf.cimg.CImgBlur")) {
        std::cerr << "Skipping: net.sf.cimg.CImgBlur is not loaded." << std::endl;
        return;
    }

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
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
    if (!isPluginLoaded("net.sf.cimg.CImgBlur")) {
        std::cerr << "Skipping: net.sf.cimg.CImgBlur is not loaded." << std::endl;
        return;
    }

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
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
    if (!isPluginLoaded("net.sf.cimg.CImgBlur")) {
        std::cerr << "Skipping: net.sf.cimg.CImgBlur is not loaded." << std::endl;
        return;
    }

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
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
    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));

    ASSERT_TRUE(bool(blur) && bool(grade));

    const NodePtr nodes[2] = { blur, grade };
    for (int i = 0; i < 2; ++i) {
        KnobChannelSetPtr channels = channelSetOf(nodes[i]);
        ASSERT_TRUE(bool(channels));
        EXPECT_FALSE(channels->hasModifications());

        NodeSerialization serialization(nodes[i]);
        EXPECT_EQ(unsigned(NODE_SERIALIZATION_CURRENT_VERSION), serialization.getVersion());

        bool saved = false;
        const NodeSerialization::KnobValues& values = serialization.getKnobsValues();
        for (NodeSerialization::KnobValues::const_iterator it = values.begin(); it != values.end(); ++it) {
            if ((*it)->getName() == std::string(kNodeParamChannelSet)) {
                saved = true;
            }
        }
        EXPECT_TRUE(saved) << nodes[i]->getPluginID();
    }
}

TEST_F(DefaultChannelSetTest, DefaultBlurRoundTripsAsAll)
{
    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));

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
    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));

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
    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));

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

// Tests/fixtures/channel-set-legacy-defaults.ntp is written at node serialization version 16:
// Blur1 and Grade1 store no channel-set value (both were on their Color default), Blur2 stores
// an explicit "specular" row.
TEST_F(DefaultChannelSetTest, LegacyProjectResetsUnsavedColorToRgbaWithAWarning)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(loadLegacyFixture(tmp));

    ProjectPtr project = getApp()->getProject();
    NodePtr blur1 = project->getNodeByName("Blur1");
    NodePtr blur2 = project->getNodeByName("Blur2");
    NodePtr grade1 = project->getNodeByName("Grade1");
    ASSERT_TRUE(bool(blur1) && bool(blur2) && bool(grade1));

    // The pre-v17 gate lands Blur1 on the old Color default, which then resets to rgba rather
    // than to Blur's own All default: All would change which layers it processes.
    KnobChannelSetPtr blur1Channels = channelSetOf(blur1);
    ASSERT_TRUE(bool(blur1Channels));
    std::vector<ChannelSetRow> rows = blur1Channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_TRUE(isColorRow(rows[0]));
    EXPECT_FALSE(isLegacyColorRow(rows[0]));
    EXPECT_TRUE(rows[0].channels.empty());
    EXPECT_TRUE(hasLegacyColorWarning(blur1));

    std::vector<ChannelSetRow> defaultRows = blur1Channels->decodeRows(blur1Channels->getDefaultValue(0));
    ASSERT_EQ(1u, defaultRows.size());
    EXPECT_EQ(ChannelSetRow::eModeAll, defaultRows[0].mode);

    KnobChannelSetPtr blur2Channels = channelSetOf(blur2);
    ASSERT_TRUE(bool(blur2Channels));
    rows = blur2Channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string("specular"), rows[0].layerOrPattern);
    EXPECT_FALSE(hasLegacyColorWarning(blur2));

    // Grade1 is not caught by the pre-v17 legacy-default load gate: that gate only fires for
    // defaultProcessesAllLayers() nodes (Blur1's case). Grade1's unsaved channel set simply
    // falls through to its live per-node default, rgba, so nothing named Color and nothing warns.
    KnobChannelSetPtr grade1Channels = channelSetOf(grade1);
    ASSERT_TRUE(bool(grade1Channels));
    rows = grade1Channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_TRUE(isColorRow(rows[0]));
    EXPECT_FALSE(hasLegacyColorWarning(grade1));
}

TEST_F(DefaultChannelSetTest, LegacyColorResetToRgbaSurvivesResaving)
{
    QTemporaryDir legacyDir;

    ASSERT_TRUE(legacyDir.isValid());
    ASSERT_TRUE(loadLegacyFixture(legacyDir));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));

    {
        QFile f(tmp.path() + QString::fromUtf8("/default-channel-set.ntp"));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString contents = QString::fromUtf8(f.readAll());
        EXPECT_FALSE(contents.contains(QString::fromUtf8("&lt;Layer&gt;" kNatronColorLayerID)));
    }

    NodePtr blur1 = getApp()->getProject()->getNodeByName("Blur1");
    ASSERT_TRUE(bool(blur1));
    KnobChannelSetPtr channels = channelSetOf(blur1);
    ASSERT_TRUE(bool(channels));
    std::vector<ChannelSetRow> rows = channels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_TRUE(isColorRow(rows[0]));
    EXPECT_FALSE(isLegacyColorRow(rows[0]));

    // The re-saved file names rgba and is current, so nothing is reset on this load.
    EXPECT_FALSE(hasLegacyColorWarning(blur1));
}
