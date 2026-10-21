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
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Serialization/KnobSerialization.h"

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
    NodePtr writer = createNativeWriter();
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

// The node hash is derived from content, so a node saved and reloaded must hash identically, and
// the saved file must not carry a knobs age.
TEST_F(BaseTest, RoundTripKeepsNodeHashAndWritesNoKnobsAge)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr generator = createNode(_generatorPluginID);
    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(generator) && bool(writer));
    connectNodes(generator, writer, 0, true);

    KnobInt* octaves = dynamic_cast<KnobInt*>(generator->getKnobByName("fbmOctaves").get());
    ASSERT_TRUE(octaves != NULL);
    octaves->setValue(7);
    octaves->setValue(9);

    const std::string generatorName = generator->getScriptName();
    const std::string writerName = writer->getScriptName();
    const U64 generatorHash = generator->getHashValue();
    const U64 writerHash = writer->getHashValue();
    ASSERT_NE(U64(0), generatorHash);
    ASSERT_NE(U64(0), writerHash);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("hash-roundtrip.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    QFile saved(savedFilePath);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const QByteArray contents = saved.readAll();
    saved.close();
    EXPECT_TRUE(contents.contains("<KnobsAge>0</KnobsAge>"));
    EXPECT_FALSE(contents.contains("<KnobsAge>1</KnobsAge>"));

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr generator2 = project->getNodeByName(generatorName);
    NodePtr writer2 = project->getNodeByName(writerName);
    ASSERT_TRUE(bool(generator2) && bool(writer2));
    EXPECT_EQ(generatorHash, generator2->getHashValue());
    EXPECT_EQ(writerHash, writer2->getHashValue());
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
    // The project outlives this test in a single-process run; the scoped receiver drops the
    // connection before layersChangedCount goes out of scope.
    QObject connectionScope;
    QObject::connect(project.get(), &Project::projectLayersChanged, &connectionScope, [&layersChangedCount]() { ++layersChangedCount; }, Qt::DirectConnection);
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
    NodePtr premult = createNode(QString::fromUtf8(kNatronTestMultiPlanePluginID), kNatronTestMultiPlaneMajor);
    NodePtr idistort = createNode(QString::fromUtf8(kNatronTestMatrixWarpPluginID), kNatronTestMatrixWarpMajor);
    NodePtr unpremult = createNode(QString::fromUtf8(kNatronTestMultiPlanePluginID), kNatronTestMultiPlaneMajor);
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

class DimensionsFoldStateProjectTest
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

    KnobDoublePtr createBlurSize(std::string* blurName)
    {
        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
        if (!blur) {
            return KnobDoublePtr();
        }
        *blurName = blur->getScriptName();

        return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    }

    static std::size_t savedDimensionCount(const KnobDoublePtr& knob)
    {
        Serialization::KnobSerialization serialization;
        knob->toSerialization(&serialization);

        return serialization.dimensions.size();
    }

    // Saves the project, resets it and loads it back, then returns the reloaded Blur's size.
    KnobDoublePtr saveResetAndReload(const std::string& blurName)
    {
        ProjectPtr project = getApp()->getProject();
        QTemporaryDir tmp;
        if (!tmp.isValid()) {
            ADD_FAILURE() << "no temporary directory";

            return KnobDoublePtr();
        }
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("fold-state.ntp");
        QString savedFilePath;
        if (!project->saveProject(dirPath, fileName, &savedFilePath)) {
            ADD_FAILURE() << "saveProject() failed";

            return KnobDoublePtr();
        }
        project->reset(false, true);
        if (!project->loadProject(dirPath, fileName)) {
            ADD_FAILURE() << "loadProject() failed";

            return KnobDoublePtr();
        }
        NodePtr blur = project->getNodeByName(blurName);
        if (!blur) {
            ADD_FAILURE() << "the reloaded project has no " << blurName;

            return KnobDoublePtr();
        }

        return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    }
};

TEST_F(DimensionsFoldStateProjectTest, FoldedKnobSavesOneValueAndLoadsFolded)
{
    std::string blurName;
    KnobDoublePtr size = createBlurSize(&blurName);
    ASSERT_TRUE(bool(size));
    ASSERT_EQ(2, size->getDimension());
    size->setValue(5., ViewSpec::all(), 0);
    size->setValue(5., ViewSpec::all(), 1);
    size->setDimensionsUnfoldedInGui(false);
    EXPECT_EQ(1u, savedDimensionCount(size));

    KnobDoublePtr reloaded = saveResetAndReload(blurName);
    ASSERT_TRUE(bool(reloaded));
    EXPECT_TRUE(reloaded->hasDimensionsFoldState());
    EXPECT_FALSE(reloaded->getDimensionsUnfoldedInGui());
    EXPECT_DOUBLE_EQ(5., reloaded->getValue(0));
    EXPECT_DOUBLE_EQ(5., reloaded->getValue(1));
    EXPECT_EQ(1u, savedDimensionCount(reloaded));
}

TEST_F(DimensionsFoldStateProjectTest, ExpandedKnobWithEqualValuesSavesEveryValueAndLoadsExpanded)
{
    std::string blurName;
    KnobDoublePtr size = createBlurSize(&blurName);
    ASSERT_TRUE(bool(size));
    size->setValue(5., ViewSpec::all(), 0);
    size->setValue(5., ViewSpec::all(), 1);
    size->setDimensionsUnfoldedInGui(true);
    EXPECT_EQ(2u, savedDimensionCount(size));

    KnobDoublePtr reloaded = saveResetAndReload(blurName);
    ASSERT_TRUE(bool(reloaded));
    EXPECT_TRUE(reloaded->hasDimensionsFoldState());
    EXPECT_TRUE(reloaded->getDimensionsUnfoldedInGui());
    EXPECT_DOUBLE_EQ(5., reloaded->getValue(0));
    EXPECT_DOUBLE_EQ(5., reloaded->getValue(1));
    EXPECT_EQ(2u, savedDimensionCount(reloaded));
}

TEST_F(DimensionsFoldStateProjectTest, ExpandedKnobWithDifferentValuesLoadsExpanded)
{
    std::string blurName;
    KnobDoublePtr size = createBlurSize(&blurName);
    ASSERT_TRUE(bool(size));
    size->setValue(3., ViewSpec::all(), 0);
    size->setValue(7., ViewSpec::all(), 1);
    size->setDimensionsUnfoldedInGui(true);
    EXPECT_EQ(2u, savedDimensionCount(size));

    KnobDoublePtr reloaded = saveResetAndReload(blurName);
    ASSERT_TRUE(bool(reloaded));
    EXPECT_TRUE(reloaded->hasDimensionsFoldState());
    EXPECT_TRUE(reloaded->getDimensionsUnfoldedInGui());
    EXPECT_DOUBLE_EQ(3., reloaded->getValue(0));
    EXPECT_DOUBLE_EQ(7., reloaded->getValue(1));
}

// Values that differ say nothing about what the user folded: they are saved per dimension
// whatever the fold state, so loading never hides one.
TEST_F(DimensionsFoldStateProjectTest, FoldedKnobWithDifferentValuesSavesEveryValue)
{
    std::string blurName;
    KnobDoublePtr size = createBlurSize(&blurName);
    ASSERT_TRUE(bool(size));
    size->setValue(3., ViewSpec::all(), 0);
    size->setValue(7., ViewSpec::all(), 1);
    size->setDimensionsUnfoldedInGui(false);
    EXPECT_EQ(2u, savedDimensionCount(size));

    KnobDoublePtr reloaded = saveResetAndReload(blurName);
    ASSERT_TRUE(bool(reloaded));
    EXPECT_TRUE(reloaded->getDimensionsUnfoldedInGui());
    EXPECT_DOUBLE_EQ(3., reloaded->getValue(0));
    EXPECT_DOUBLE_EQ(7., reloaded->getValue(1));
}

TEST_F(DimensionsFoldStateProjectTest, AnimatedFoldedKnobSavesOneCurveForEveryDimension)
{
    std::string blurName;
    KnobDoublePtr size = createBlurSize(&blurName);
    ASSERT_TRUE(bool(size));
    for (int d = 0; d < 2; ++d) {
        size->setValueAtTime(1, 2., ViewSpec::all(), d);
        size->setValueAtTime(11, 8., ViewSpec::all(), d);
    }
    size->setDimensionsUnfoldedInGui(false);
    EXPECT_EQ(1u, savedDimensionCount(size));

    KnobDoublePtr reloaded = saveResetAndReload(blurName);
    ASSERT_TRUE(bool(reloaded));
    EXPECT_TRUE(reloaded->hasDimensionsFoldState());
    EXPECT_FALSE(reloaded->getDimensionsUnfoldedInGui());
    for (int d = 0; d < 2; ++d) {
        SCOPED_TRACE(d);
        CurvePtr curve = reloaded->getCurve(ViewIdx(0), d);
        ASSERT_TRUE(bool(curve));
        EXPECT_EQ(2, curve->getKeyFramesCount());
        EXPECT_DOUBLE_EQ(2., reloaded->getValueAtTime(1, d));
        EXPECT_DOUBLE_EQ(8., reloaded->getValueAtTime(11, d));
    }
    EXPECT_DOUBLE_EQ(reloaded->getValueAtTime(6, 0), reloaded->getValueAtTime(6, 1));
    EXPECT_EQ(1u, savedDimensionCount(reloaded));
}

TEST_F(DimensionsFoldStateProjectTest, ExpandedKnobAtItsDefaultIsSavedAndLoadsExpanded)
{
    std::string blurName;
    KnobDoublePtr size = createBlurSize(&blurName);
    ASSERT_TRUE(bool(size));
    size->setDimensionsUnfoldedInGui(true);
    Serialization::KnobSerialization serialization;
    EXPECT_TRUE(size->toSerialization(&serialization));
    EXPECT_EQ(2u, serialization.dimensions.size());

    KnobDoublePtr reloaded = saveResetAndReload(blurName);
    ASSERT_TRUE(bool(reloaded));
    EXPECT_TRUE(reloaded->hasDimensionsFoldState());
    EXPECT_TRUE(reloaded->getDimensionsUnfoldedInGui());
}
