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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

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
        EXPECT_EQ((std::size_t)6, snapshot->size()); // 5 built-ins + depth
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
    EXPECT_FALSE(project->removeLayer(kNatronColorLayerID, &error));
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
