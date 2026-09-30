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

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobSerialization.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/WriteNode.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

// Exercises Project::saveProject()/loadProject() end to end: builds a generator -> writer graph,
// sets a distinctive value on one knob of each kind that Engine/*Serialization.h treats
// differently (Int, Double, Choice, String, File), then checks that node identity, the
// connection between the two nodes, and every one of those values survive a save/reset/load
// cycle.
TEST_F(BaseTest, RoundTripsNodesConnectionsAndKnobValues)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr generator = createNode(_generatorPluginID);
    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(generator) && bool(writer));

    connectNodes(generator, writer, 0, true);

    const std::string generatorName = generator->getScriptName();
    const std::string writerName = writer->getScriptName();

    KnobInt* octaves = dynamic_cast<KnobInt*>(generator->getKnobByName("fbmOctaves").get());
    ASSERT_TRUE(octaves != NULL);
    octaves->setValue(11);

    KnobDouble* zSlope = dynamic_cast<KnobDouble*>(generator->getKnobByName("noiseZSlope").get());
    ASSERT_TRUE(zSlope != NULL);
    zSlope->setValue(0.6125);

    KnobChoice* noiseType = dynamic_cast<KnobChoice*>(generator->getKnobByName("noiseType").get());
    ASSERT_TRUE(noiseType != NULL);
    noiseType->setValueFromID("voronoi", 0);
    const std::string noiseTypeId = noiseType->getActiveEntry().id;
    ASSERT_EQ(std::string("voronoi"), noiseTypeId);

    KnobOutputFile* filename = dynamic_cast<KnobOutputFile*>(writer->getKnobByName("filename").get());
    ASSERT_TRUE(filename != NULL);
    const std::string outputPath("/tmp/natron-roundtrip-test.####.exr");
    filename->setValue(outputPath);

    KnobString* key1 = dynamic_cast<KnobString*>(writer->getKnobByName("key1").get());
    ASSERT_TRUE(key1 != NULL);
    const std::string key1Value("roundtrip-marker");
    key1->setValue(key1Value);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("roundtrip.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(generatorName).get() == NULL);
    ASSERT_TRUE(project->getNodeByName(writerName).get() == NULL);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr generator2 = project->getNodeByName(generatorName);
    NodePtr writer2 = project->getNodeByName(writerName);
    ASSERT_TRUE(bool(generator2));
    ASSERT_TRUE(bool(writer2));
    EXPECT_EQ(generatorName, generator2->getScriptName());
    EXPECT_EQ(writerName, writer2->getScriptName());
    EXPECT_EQ(generator2, writer2->getInput(0));

    KnobInt* octaves2 = dynamic_cast<KnobInt*>(generator2->getKnobByName("fbmOctaves").get());
    ASSERT_TRUE(octaves2 != NULL);
    EXPECT_EQ(11, octaves2->getValue());

    KnobDouble* zSlope2 = dynamic_cast<KnobDouble*>(generator2->getKnobByName("noiseZSlope").get());
    ASSERT_TRUE(zSlope2 != NULL);
    EXPECT_DOUBLE_EQ(0.6125, zSlope2->getValue());

    KnobChoice* noiseType2 = dynamic_cast<KnobChoice*>(generator2->getKnobByName("noiseType").get());
    ASSERT_TRUE(noiseType2 != NULL);
    EXPECT_EQ(noiseTypeId, noiseType2->getActiveEntry().id);

    KnobOutputFile* filename2 = dynamic_cast<KnobOutputFile*>(writer2->getKnobByName("filename").get());
    ASSERT_TRUE(filename2 != NULL);
    EXPECT_EQ(outputPath, filename2->getValue());

    KnobString* key1_2 = dynamic_cast<KnobString*>(writer2->getKnobByName("key1").get());
    ASSERT_TRUE(key1_2 != NULL);
    EXPECT_EQ(key1Value, key1_2->getValue());
}

static ImageLayerDesc
makeThreeChannelLayer(const std::string& id)
{
    std::vector<std::string> channels;

    channels.push_back("R");
    channels.push_back("G");
    channels.push_back("B");
    return ImageLayerDesc(id, id, "", channels);
}

// Exercises the project-level LayerRegistry (Project::addLayer/removeLayer/findLayer/
// getLayerRegistrySnapshot) through a save/reset/load cycle: built-ins are never written
// to the .ntp, non-built-in layers (both user- and file-origin) are, project reset drops
// everything back to built-ins + depth, and loading restores exactly what was saved while
// emitting projectLayersChanged() exactly once.
TEST_F(BaseTest, RoundTripsLayerRegistry)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded,
              project->addLayer(makeThreeChannelLayer("diffuse"), LayerRegistryEntry::eOriginUser, &error))
        << error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded,
              project->addLayer(makeThreeChannelLayer("specular"), LayerRegistryEntry::eOriginFile, &error))
        << error;

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("layers-roundtrip.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    {
        QFile f(savedFilePath);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString contents = QString::fromUtf8(f.readAll());
        EXPECT_TRUE(contents.contains(QString::fromUtf8("<Layers")));
        EXPECT_TRUE(contents.contains(QString::fromUtf8("diffuse")));
        EXPECT_FALSE(contents.contains(QString::fromUtf8(kNatronColorLayerID)));
        EXPECT_FALSE(contents.contains(QString::fromUtf8("Backward")));
    }

    project->reset(false, true);

    {
        std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();
        EXPECT_EQ((std::size_t)9, snapshot->size()); // rgba, rgb, alpha, xy, DisparityLeft, DisparityRight, Backward, Forward + depth
        ImageLayerDesc unused;
        EXPECT_FALSE(project->findLayer("diffuse", &unused));
        EXPECT_FALSE(project->findLayer("specular", &unused));
    }

    int layersChangedCount = 0;
    QObject::connect(project.get(), &Project::projectLayersChanged, [&layersChangedCount]() {
        ++layersChangedCount;
    });
    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    EXPECT_EQ(1, layersChangedCount);

    {
        std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();
        int diffuseIdx = -1, specularIdx = -1;
        for (std::size_t i = 0; i < snapshot->size(); ++i) {
            if ((*snapshot)[i].desc.getLayerID() == "diffuse") {
                diffuseIdx = (int)i;
            }
            if ((*snapshot)[i].desc.getLayerID() == "specular") {
                specularIdx = (int)i;
            }
        }
        ASSERT_GE(diffuseIdx, 0);
        ASSERT_GE(specularIdx, 0);
        EXPECT_LT(diffuseIdx, specularIdx);
        EXPECT_EQ(LayerRegistryEntry::eOriginUser, (*snapshot)[diffuseIdx].origin);
        EXPECT_EQ(LayerRegistryEntry::eOriginFile, (*snapshot)[specularIdx].origin);
    }

    project->reset(false, true);
}

// Removal is refused for built-ins regardless of references, and allowed for a
// non-built-in layer with no users.
TEST_F(BaseTest, RemoveLayerRefusesBuiltinsAllowsUnused)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::string error;
    EXPECT_FALSE(project->removeLayer(kNatronColorViewRGBA, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    ASSERT_EQ(LayerRegistry::eAddResultAdded,
              project->addLayer(makeThreeChannelLayer("diffuse"), LayerRegistryEntry::eOriginUser, &error))
        << error;

    error.clear();
    EXPECT_TRUE(project->removeLayer("diffuse", &error));
    EXPECT_TRUE(error.empty());

    ImageLayerDesc unused;
    EXPECT_FALSE(project->findLayer("diffuse", &unused));

    project->reset(false, true);
}

TEST_F(BaseTest, SavedProjectContainsNoUserComponents)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("no-usercomponents.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    QFile f(savedFilePath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString contents = QString::fromUtf8(f.readAll());
    f.close();
    EXPECT_FALSE(contents.contains(QString::fromUtf8("UserComponents")))
        << "Saved project should not contain UserComponents element";
}

namespace {

const char* const kLegacyColorWarning = "Colour layer from an older project was reset to rgba";

bool
hasLegacyColorWarning(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return type == eMessageTypeWarning && message == QString::fromUtf8(kLegacyColorWarning);
}

bool
ownsChannelSelectorMessage(const NodePtr& node)
{
    NodesList owners;

    Node::getNodesOwningChannelSelectorMessage(&owners);

    return std::find(owners.begin(), owners.end(), node) != owners.end();
}

} // namespace

// Tests/fixtures/m65-legacy-color.ntp is hand-written at node serialization version 16 and
// Natron 2.2. Grade1 saves channels = Color {R,G,B} (with Color as its saved default) and its
// mask on Color.A; Shuffle1 reads and writes Color; Blur1 saves no channel set, so the pre-v17
// gate lands it on Color; Write1 saves Natron 2.2's outputChannels option "RGBA".
class LegacyColorProjectTest
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

    bool loadFixture(const QTemporaryDir& tmp)
    {
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("m65-legacy-color.ntp");

        if (!QFile::copy(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/m65-legacy-color.ntp"), dirPath + fileName)) {
            return false;
        }

        return getApp()->getProject()->loadProject(dirPath, fileName);
    }
};

TEST_F(LegacyColorProjectTest, ColorLayerValuesResetToRgbaKeepingChannelsWithAWarning)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    bool loaded = false;
    EXPECT_NO_THROW(loaded = loadFixture(tmp));
    ASSERT_TRUE(loaded);

    ProjectPtr project = getApp()->getProject();
    NodePtr grade = project->getNodeByName("Grade1");
    NodePtr shuffle = project->getNodeByName("Shuffle1");
    NodePtr blur = project->getNodeByName("Blur1");
    ASSERT_TRUE(bool(grade) && bool(shuffle) && bool(blur));

    KnobChannelSetPtr gradeChannels = std::dynamic_pointer_cast<KnobChannelSet>(grade->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(gradeChannels));
    std::vector<ChannelSetRow> rows = gradeChannels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorViewRGBA), rows[0].layerOrPattern);
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    EXPECT_EQ(rgb, rows[0].channels);

    // The saved default named Color too; it goes back to what a new Grade defaults to.
    {
        NodePtr freshGrade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));
        ASSERT_TRUE(bool(freshGrade));
        KnobChannelSetPtr freshChannels = std::dynamic_pointer_cast<KnobChannelSet>(freshGrade->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(freshChannels));
        EXPECT_EQ(freshChannels->getDefaultValue(0), gradeChannels->getDefaultValue(0));
        EXPECT_EQ(std::string::npos, gradeChannels->getDefaultValue(0).find(kNatronColorLayerID));
    }

    KnobChannelSelectPtr mask = std::dynamic_pointer_cast<KnobChannelSelect>(grade->getKnobByName(std::string(kMaskChannelKnobName) + "_Mask"));
    ASSERT_TRUE(bool(mask));
    EXPECT_EQ(std::string(kNatronColorViewRGBA ".A"), mask->get());
    EXPECT_EQ(std::string::npos, mask->getDefaultValue(0).find(kNatronColorLayerID));
    EXPECT_TRUE(hasLegacyColorWarning(grade));

    KnobLayerSelectPtr in1 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamIn1));
    KnobLayerSelectPtr out1 = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(kShuffleParamOut1));
    ASSERT_TRUE(bool(in1) && bool(out1));
    EXPECT_EQ(std::string(kNatronColorViewRGBA), in1->getLayer());
    EXPECT_TRUE(in1->getChannels().empty());
    EXPECT_EQ(std::string(kNatronColorViewRGBA), out1->getLayer());
    EXPECT_TRUE(out1->getChannels().empty());
    EXPECT_TRUE(hasLegacyColorWarning(shuffle));

    // Blur defaults to All, but the pre-v17 gate first restores the Color it defaulted to
    // back then; that value resets to rgba (not All, which would process other layers too) and
    // warns, while "Reset to default" still gives All.
    KnobChannelSetPtr blurChannels = std::dynamic_pointer_cast<KnobChannelSet>(blur->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(blurChannels));
    rows = blurChannels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorViewRGBA), rows[0].layerOrPattern);
    EXPECT_TRUE(rows[0].channels.empty());
    std::vector<ChannelSetRow> blurDefault = blurChannels->decodeRows(blurChannels->getDefaultValue(0));
    ASSERT_EQ(1u, blurDefault.size());
    EXPECT_EQ(ChannelSetRow::eModeAll, blurDefault[0].mode);
    EXPECT_TRUE(hasLegacyColorWarning(blur));
}

// The colour storage ID is not a valid layer value, so Natron <= 2.2 colour option IDs must pass through unfiltered.
TEST_F(LegacyColorProjectTest, LegacyColourChoiceOptionFallsBackWithoutThrowing)
{
    std::string option("RGBA");
    EXPECT_FALSE(filterKnobChoiceOptionCompat("fr.inria.openfx.WriteOIIO", 1, 0, 2, 2, 0, "outputChannels", &option));
    EXPECT_EQ(std::string("RGBA"), option);

    option = "a";
    EXPECT_FALSE(filterKnobChoiceOptionCompat("net.sf.openfx.GradePlugin", 2, 0, 2, 2, 0, "maskChannel", &option));
    EXPECT_EQ(std::string("a"), option);

    option = "Backward.Motion";
    EXPECT_TRUE(filterKnobChoiceOptionCompat("fr.inria.openfx.WriteOIIO", 1, 0, 2, 2, 0, "outputChannels", &option));
    EXPECT_EQ(std::string(kNatronBackwardMotionVectorsLayerID "." kNatronMotionComponentsLabel), option);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    bool loaded = false;
    EXPECT_NO_THROW(loaded = loadFixture(tmp));
    ASSERT_TRUE(loaded);

    NodePtr write = getApp()->getProject()->getNodeByName("Write1");
    ASSERT_TRUE(bool(write));
    WriteNode* container = dynamic_cast<WriteNode*>(write->getEffectInstance().get());
    ASSERT_TRUE(container != NULL);
    NodePtr encoder = container->getEmbeddedWriter();
    ASSERT_TRUE(bool(encoder));
    KnobChoice* outputChannels = dynamic_cast<KnobChoice*>(encoder->getKnobByName("outputChannels").get());
    ASSERT_TRUE(outputChannels != NULL);
    EXPECT_EQ(outputChannels->getDefaultValue(0), outputChannels->getValue());
}

TEST_F(LegacyColorProjectTest, LegacyColorWarningSurvivesARender)
{
    QTemporaryDir tmp;

    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(loadFixture(tmp));

    NodePtr grade = getApp()->getProject()->getNodeByName("Grade1");
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(hasLegacyColorWarning(grade));
    EXPECT_FALSE(ownsChannelSelectorMessage(grade));

    CreateNodeArgs readArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    NodePtr reader = getApp()->createNode(readArgs);
    ASSERT_TRUE(bool(reader));

    int sourceInput = -1;
    for (int i = 0; i < grade->getNInputs(); ++i) {
        if (grade->getInputLabel(i) == "Source") {
            sourceInput = i;
        }
    }
    ASSERT_GE(sourceInput, 0);
    connectNodes(reader, grade, sourceInput, true);

    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(writer));
    connectNodes(grade, writer, 0, true);

    const std::string path = (tmp.path() + QString::fromUtf8("/legacy-color-render.exr")).toStdString();
    writer->setOutputFilesForWriter(path);
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
    getApp()->startWritersRendering(false, works);
    EXPECT_TRUE(QFile::exists(QString::fromStdString(path)));

    EXPECT_TRUE(hasLegacyColorWarning(grade));

    // A preview render clears the node's other persistent messages first.
    const int size = 32;
    std::vector<unsigned int> buffer(size * size, 0);
    int width = size;
    int height = size;
    grade->makePreviewImage(1, &width, &height, &buffer[0]);
    EXPECT_TRUE(hasLegacyColorWarning(grade));

    // What a completed render calls to drop a channel-selector error it no longer reproduces.
    grade->clearChannelSelectorMessage();
    EXPECT_TRUE(hasLegacyColorWarning(grade));
}

TEST_F(LegacyColorProjectTest, ResavingWritesRgbaNeverTheStorageID)
{
    QTemporaryDir legacyDir;

    ASSERT_TRUE(legacyDir.isValid());
    ASSERT_TRUE(loadFixture(legacyDir));

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("m65-resaved.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    {
        QFile f(savedFilePath);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString contents = QString::fromUtf8(f.readAll());
        EXPECT_FALSE(contents.contains(QString::fromUtf8("&lt;Layer&gt;" kNatronColorLayerID)));
        EXPECT_FALSE(contents.contains(QString::fromUtf8("&lt;Channel&gt;" kNatronColorLayerID)));
        EXPECT_TRUE(contents.contains(QString::fromUtf8("&lt;Layer&gt;" kNatronColorViewRGBA "&lt;")));
    }

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    const char* const names[] = { "Grade1", "Shuffle1", "Blur1" };
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        NodePtr node = project->getNodeByName(names[i]);
        ASSERT_TRUE(bool(node)) << names[i];
        EXPECT_FALSE(hasLegacyColorWarning(node)) << names[i];
    }
}

namespace {

std::string
activeOptionID(const NodePtr& node,
               const char* pluginKnob)
{
    KnobChoice* choice = dynamic_cast<KnobChoice*>(node->getKnobByName(pluginKnob).get());

    return choice ? choice->getActiveEntry().id : std::string("<missing>");
}

bool
boolValue(const NodePtr& node,
          const char* name)
{
    KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());

    return knob && knob->getValue();
}

} // namespace

class MultiplaneTwinProjectTest
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
};

// The plugin choices a twin replaces are not saved, so without the twins being pushed into them
// on load the plugins would come back at their defaults. Each value set here differs from its
// plugin default: channelU from the colour R, channelA from the colour A, the Premult quad,
// which defaults to R, G and B only, and Unpremult's all-planes checkbox, which defaults off.
TEST_F(MultiplaneTwinProjectTest, TwinsRoundTripAndArePushedIntoThePluginChoicesOnLoad)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr premult = createNode(QString::fromUtf8("net.sf.openfx.Premult"));
    NodePtr idistort = createNode(QString::fromUtf8("net.sf.openfx.IDistort"));
    NodePtr unpremult = createNode(QString::fromUtf8("net.sf.openfx.Unpremult"));
    ASSERT_TRUE(bool(premult) && bool(idistort) && bool(unpremult));
    const std::string premultName = premult->getScriptName();
    const std::string idistortName = idistort->getScriptName();
    const std::string unpremultName = unpremult->getScriptName();

    {
        KnobLayerSelectPtr inputPlane = std::dynamic_pointer_cast<KnobLayerSelect>(premult->getKnobByName("hostInputLayer"));
        KnobChannelSelectPtr unPremultBy = std::dynamic_pointer_cast<KnobChannelSelect>(premult->getKnobByName("hostUnPremultByChannel"));
        KnobChannelSelectPtr channelU = std::dynamic_pointer_cast<KnobChannelSelect>(idistort->getKnobByName("hostChannelU"));
        KnobChannelSelectPtr channelA = std::dynamic_pointer_cast<KnobChannelSelect>(idistort->getKnobByName("hostChannelA"));
        ASSERT_TRUE(bool(inputPlane) && bool(unPremultBy) && bool(channelU) && bool(channelA));
        ASSERT_FALSE(inputPlane->getChannels().empty());
        ASSERT_FALSE(boolValue(premult, kNatronOfxParamProcessA));

        inputPlane->setChannels(std::vector<std::string>());
        unPremultBy->set(kNatronColorViewRGBA ".R");
        channelU->set("diffuse.R");
        channelA->setNone();

        KnobLayerSelectPtr unpremultLayer = std::dynamic_pointer_cast<KnobLayerSelect>(unpremult->getKnobByName("hostInputLayer"));
        ASSERT_TRUE(bool(unpremultLayer));
        ASSERT_FALSE(boolValue(unpremult, "processAllPlanes"));
        unpremultLayer->setLayer(kNatronLayerSelectAll);
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("multiplane-twins.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    {
        QFile f(savedFilePath);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString contents = QString::fromUtf8(f.readAll());
        EXPECT_TRUE(contents.contains(QString::fromUtf8("<Name>hostChannelU</Name>")));
        EXPECT_FALSE(contents.contains(QString::fromUtf8("<Name>channelU</Name>")));
        EXPECT_FALSE(contents.contains(QString::fromUtf8("<Name>" kNatronOfxParamProcessA "</Name>")));
        EXPECT_FALSE(contents.contains(QString::fromUtf8("<Name>processAllPlanes</Name>")));
    }

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    premult = project->getNodeByName(premultName);
    idistort = project->getNodeByName(idistortName);
    unpremult = project->getNodeByName(unpremultName);
    ASSERT_TRUE(bool(premult) && bool(idistort) && bool(unpremult));

    KnobLayerSelectPtr inputPlane = std::dynamic_pointer_cast<KnobLayerSelect>(premult->getKnobByName("hostInputLayer"));
    KnobChannelSelectPtr unPremultBy = std::dynamic_pointer_cast<KnobChannelSelect>(premult->getKnobByName("hostUnPremultByChannel"));
    KnobChannelSelectPtr channelU = std::dynamic_pointer_cast<KnobChannelSelect>(idistort->getKnobByName("hostChannelU"));
    KnobChannelSelectPtr channelA = std::dynamic_pointer_cast<KnobChannelSelect>(idistort->getKnobByName("hostChannelA"));
    ASSERT_TRUE(bool(inputPlane) && bool(unPremultBy) && bool(channelU) && bool(channelA));

    EXPECT_EQ(std::string(kNatronColorViewRGBA), inputPlane->getLayer());
    EXPECT_TRUE(inputPlane->getChannels().empty());
    EXPECT_EQ(std::string(kNatronColorViewRGBA ".R"), unPremultBy->get());
    EXPECT_EQ(std::string("diffuse.R"), channelU->get());
    EXPECT_TRUE(channelA->isNone());

    EXPECT_EQ(std::string(kNatronColorLayerID), activeOptionID(premult, "inputPlane"));
    EXPECT_EQ(std::string(kNatronColorLayerID ".R"), activeOptionID(premult, "unPremultByChannel"));
    EXPECT_EQ(std::string("diffuse.R"), activeOptionID(idistort, "channelU"));
    EXPECT_EQ(std::string("1"), activeOptionID(idistort, "channelA"));

    KnobLayerSelectPtr unpremultLayer = std::dynamic_pointer_cast<KnobLayerSelect>(unpremult->getKnobByName("hostInputLayer"));
    ASSERT_TRUE(bool(unpremultLayer));
    EXPECT_EQ(std::string(kNatronLayerSelectAll), unpremultLayer->getLayer());
    EXPECT_TRUE(boolValue(unpremult, "processAllPlanes"));
    EXPECT_TRUE(boolValue(premult, kNatronOfxParamProcessR));
    EXPECT_TRUE(boolValue(premult, kNatronOfxParamProcessG));
    EXPECT_TRUE(boolValue(premult, kNatronOfxParamProcessB));
    EXPECT_TRUE(boolValue(premult, kNatronOfxParamProcessA));
}
