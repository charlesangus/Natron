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
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QLatin1Char>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

TEST(LayerRegistry, BuiltinOrder)
{
    LayerRegistry registry;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snap = registry.snapshot();

    ASSERT_EQ(6u, snap->size());
    EXPECT_EQ(std::string(kNatronColorLayerID), (*snap)[0].desc.getLayerID());
    EXPECT_EQ(LayerRegistryEntry::eOriginBuiltin, (*snap)[0].origin);
    EXPECT_EQ(std::string(kNatronDisparityLeftLayerID), (*snap)[1].desc.getLayerID());
    EXPECT_EQ(std::string(kNatronDisparityRightLayerID), (*snap)[2].desc.getLayerID());
    EXPECT_EQ(std::string(kNatronBackwardMotionVectorsLayerID), (*snap)[3].desc.getLayerID());
    EXPECT_EQ(std::string(kNatronForwardMotionVectorsLayerID), (*snap)[4].desc.getLayerID());
    EXPECT_EQ("depth", (*snap)[5].desc.getLayerID());
    EXPECT_EQ(LayerRegistryEntry::eOriginUser, (*snap)[5].origin);
}

TEST(LayerRegistry, RgbaAliasReturnsColorAndRegistersNothing)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch(1, "R");
    ImageLayerDesc rgba("rgba", "rgba", "", ch);

    EXPECT_EQ(LayerRegistry::eAddResultUnchanged, registry.add(rgba, LayerRegistryEntry::eOriginUser, &error));

    ImageLayerDesc found;
    EXPECT_FALSE(registry.find("rgba", &found));
    EXPECT_TRUE(registry.find(kNatronColorLayerID, &found));
}

TEST(LayerRegistry, ReservedNamesRefused)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch(1, "A");

    ImageLayerDesc none("none", "none", "", ch);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(none, LayerRegistryEntry::eOriginUser, &error));

    ImageLayerDesc all("all", "all", "", ch);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(all, LayerRegistryEntry::eOriginUser, &error));

    ImageLayerDesc backward("Backward", "Backward", "", ch);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(backward, LayerRegistryEntry::eOriginUser, &error));
}

TEST(LayerRegistry, DottedIdRefusedUnlessFromFile)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch(1, "R");
    ImageLayerDesc dotted("light1.diffuse", "light1.diffuse", "", ch);

    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(dotted, LayerRegistryEntry::eOriginUser, &error));
    EXPECT_EQ(LayerRegistry::eAddResultAdded, registry.add(dotted, LayerRegistryEntry::eOriginFile, &error));
    EXPECT_TRUE(registry.contains("light1.diffuse"));
}

TEST(LayerRegistry, FiveChannelsRefused)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch;
    ch.push_back("R");
    ch.push_back("G");
    ch.push_back("B");
    ch.push_back("A");
    ch.push_back("Z");
    ImageLayerDesc five("five", "five", "", ch);

    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(five, LayerRegistryEntry::eOriginUser, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LayerRegistry, DuplicateIdenticalIsUnchanged)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch;
    ch.push_back("R");
    ch.push_back("G");
    ch.push_back("B");
    ImageLayerDesc diffuse("diffuse", "diffuse", "", ch);

    ASSERT_EQ(LayerRegistry::eAddResultAdded, registry.add(diffuse, LayerRegistryEntry::eOriginFile, &error));
    EXPECT_EQ(LayerRegistry::eAddResultUnchanged, registry.add(diffuse, LayerRegistryEntry::eOriginFile, &error));
}

TEST(LayerRegistry, FileUnionGrowsChannels)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    ImageLayerDesc diffuseRgb("diffuse", "diffuse", "", rgb);
    ASSERT_EQ(LayerRegistry::eAddResultAdded, registry.add(diffuseRgb, LayerRegistryEntry::eOriginFile, &error));

    std::vector<std::string> rgba;
    rgba.push_back("R");
    rgba.push_back("G");
    rgba.push_back("B");
    rgba.push_back("A");
    ImageLayerDesc diffuseRgba("diffuse", "diffuse", "", rgba);
    EXPECT_EQ(LayerRegistry::eAddResultGrown, registry.add(diffuseRgba, LayerRegistryEntry::eOriginFile, &error));

    ImageLayerDesc found;
    ASSERT_TRUE(registry.find("diffuse", &found));
    ASSERT_EQ(4, found.getNumComponents());
    EXPECT_EQ("R", found.getChannels()[0]);
    EXPECT_EQ("G", found.getChannels()[1]);
    EXPECT_EQ("B", found.getChannels()[2]);
    EXPECT_EQ("A", found.getChannels()[3]);
}

TEST(LayerRegistry, UserConflictRefused)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    ImageLayerDesc diffuseRgb("diffuse", "diffuse", "", rgb);
    ASSERT_EQ(LayerRegistry::eAddResultAdded, registry.add(diffuseRgb, LayerRegistryEntry::eOriginUser, &error));

    std::vector<std::string> rgba;
    rgba.push_back("R");
    rgba.push_back("G");
    rgba.push_back("B");
    rgba.push_back("A");
    ImageLayerDesc diffuseRgba("diffuse", "diffuse", "", rgba);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(diffuseRgba, LayerRegistryEntry::eOriginUser, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LayerRegistry, RemoveBuiltinRefused)
{
    LayerRegistry registry;
    std::string error;

    EXPECT_FALSE(registry.remove(kNatronColorLayerID, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LayerRegistry, RemoveDepthSucceeds)
{
    LayerRegistry registry;
    std::string error;

    EXPECT_TRUE(registry.remove("depth", &error));
    EXPECT_FALSE(registry.contains("depth"));
}

TEST(LayerRegistry, GroupChannelNames)
{
    std::vector<std::string> flat;
    flat.push_back("R");
    flat.push_back("G");
    flat.push_back("B");
    flat.push_back("A");
    flat.push_back("Z");
    flat.push_back("diffuse.R");
    flat.push_back("diffuse.G");

    std::vector<ImageLayerDesc> layers;
    LayerRegistry::groupChannelNames(flat, &layers);

    ASSERT_EQ(3u, layers.size());

    EXPECT_EQ(std::string(kNatronColorLayerID), layers[0].getLayerID());
    ASSERT_EQ(4, layers[0].getNumComponents());
    EXPECT_EQ("R", layers[0].getChannels()[0]);
    EXPECT_EQ("G", layers[0].getChannels()[1]);
    EXPECT_EQ("B", layers[0].getChannels()[2]);
    EXPECT_EQ("A", layers[0].getChannels()[3]);

    EXPECT_EQ("depth", layers[1].getLayerID());
    ASSERT_EQ(1, layers[1].getNumComponents());
    EXPECT_EQ("Z", layers[1].getChannels()[0]);

    EXPECT_EQ("diffuse", layers[2].getLayerID());
    ASSERT_EQ(2, layers[2].getNumComponents());
    EXPECT_EQ("R", layers[2].getChannels()[0]);
    EXPECT_EQ("G", layers[2].getChannels()[1]);
}

TEST(LayerRegistry, SnapshotIsStableAcrossAdd)
{
    LayerRegistry registry;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> before = registry.snapshot();
    std::size_t sizeBefore = before->size();

    std::string error;
    std::vector<std::string> ch(1, "R");
    ImageLayerDesc newLayer("extra", "extra", "", ch);
    registry.add(newLayer, LayerRegistryEntry::eOriginUser, &error);

    EXPECT_EQ(sizeBefore, before->size());

    std::shared_ptr<const std::vector<LayerRegistryEntry>> after = registry.snapshot();
    EXPECT_EQ(sizeBefore + 1, after->size());
}

namespace {

bool
findOrigin(const ProjectPtr& project,
           const std::string& id,
           LayerRegistryEntry::OriginEnum* origin)
{
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();

    for (std::vector<LayerRegistryEntry>::const_iterator it = snapshot->begin(); it != snapshot->end(); ++it) {
        if (it->desc.getLayerID() == id) {
            *origin = it->origin;

            return true;
        }
    }

    return false;
}

} // namespace

// A Read node's produced (non-Color) planes are registered at the project level as soon as the
// node exists, with origin eOriginFile; pointing the same Read at a file that no longer carries
// those planes must not unregister them (Nuke behaviour: a channel, once known, stays known).
TEST_F(BaseTest, ReadRegistersFileLayers)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    LayerRegistryEntry::OriginEnum origin;
    ASSERT_TRUE(findOrigin(project, "diffuse", &origin));
    EXPECT_EQ(LayerRegistryEntry::eOriginFile, origin);
    ASSERT_TRUE(findOrigin(project, "specular", &origin));
    EXPECT_EQ(LayerRegistryEntry::eOriginFile, origin);

    KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(fileKnob));
    fileKnob->setValue(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    reader->forceRefreshAllInputRelatedData();

    EXPECT_TRUE(findOrigin(project, "diffuse", &origin));
    EXPECT_TRUE(findOrigin(project, "specular", &origin));

    project->reset(false, true);
} // TEST_F(BaseTest, ReadRegistersFileLayers)

// Loading a project whose Read nodes' files have not changed must restore the exact same
// registry that was saved, and emit projectLayersChanged() exactly once (not once per node whose
// produced layers get re-registered during the load).
TEST_F(BaseTest, LoadWithUnchangedFilesDoesNotChangeRegistry)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    LayerRegistryEntry::OriginEnum origin;
    ASSERT_TRUE(findOrigin(project, "diffuse", &origin));
    ASSERT_TRUE(findOrigin(project, "specular", &origin));

    std::shared_ptr<const std::vector<LayerRegistryEntry>> beforeSnapshot = project->getLayerRegistrySnapshot();
    const std::vector<LayerRegistryEntry> before(*beforeSnapshot);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("unchanged-files.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);

    int layersChangedCount = 0;
    QObject::connect(project.get(), &Project::projectLayersChanged, [&layersChangedCount]() {
        ++layersChangedCount;
    });

    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    EXPECT_EQ(1, layersChangedCount);

    std::shared_ptr<const std::vector<LayerRegistryEntry>> afterSnapshot = project->getLayerRegistrySnapshot();
    ASSERT_EQ(before.size(), afterSnapshot->size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].desc.getLayerID(), (*afterSnapshot)[i].desc.getLayerID());
        EXPECT_EQ(before[i].origin, (*afterSnapshot)[i].origin);
        EXPECT_EQ(before[i].desc.getNumComponents(), (*afterSnapshot)[i].desc.getNumComponents());
    }

    project->reset(false, true);
} // TEST_F(BaseTest, LoadWithUnchangedFilesDoesNotChangeRegistry)

// Selecting a registered layer on a downstream node's Output Layer choice makes that node a
// "user" of the layer (Node::getReferencedLayerIDs()); Project::removeLayer() must then refuse
// and name the referencing node.
TEST_F(BaseTest, ReferencedLayerCannotBeRemoved)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    ASSERT_TRUE(bool(blur));

    connectNodes(reader, blur, 0, true);

    KnobChoicePtr outputLayerKnob = std::dynamic_pointer_cast<KnobChoice>(blur->getKnobByName(kOutputChannelsKnobName));
    ASSERT_TRUE(bool(outputLayerKnob));
    outputLayerKnob->setValueFromID("diffuse", 0);

    std::string error;
    EXPECT_FALSE(project->removeLayer("diffuse", &error));
    EXPECT_NE(std::string::npos, error.find(blur->getScriptName_mt_safe()));

    std::list<NodePtr> users;
    project->getLayerUsers("diffuse", &users);
    bool foundBlur = false;
    for (std::list<NodePtr>::const_iterator it = users.begin(); it != users.end(); ++it) {
        if (*it == blur) {
            foundBlur = true;
        }
    }
    EXPECT_TRUE(foundBlur);

    project->reset(false, true);
} // TEST_F(BaseTest, ReferencedLayerCannotBeRemoved)

static bool
findLayersKnobRow(const KnobTablePtr& knob, const std::string& label, std::vector<std::string>* row)
{
    std::list<std::vector<std::string>> table;

    knob->getTable(&table);
    for (std::list<std::vector<std::string>>::const_iterator it = table.begin(); it != table.end(); ++it) {
        if ((*it)[0] == label) {
            *row = *it;

            return true;
        }
    }

    return false;
}

// The project "Layers" page knob is a read-only view of the registry: one row per
// registered layer with its channels and the number of referencing nodes, kept in sync
// with registry mutations and with layer selections on nodes.
TEST_F(BaseTest, LayersKnobMirrorsRegistryAndUsers)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    KnobTablePtr layersKnob = std::dynamic_pointer_cast<KnobTable>(project->getKnobByName("defaultLayers"));
    ASSERT_TRUE(bool(layersKnob));
    EXPECT_EQ(3, layersKnob->getColumnsCount());

    std::vector<std::string> row;
    EXPECT_TRUE(findLayersKnobRow(layersKnob, "Color", &row));
    EXPECT_EQ(std::string("R G B A"), row[1]);
    EXPECT_EQ(std::string("0"), row[2]);
    EXPECT_FALSE(findLayersKnobRow(layersKnob, "diffuse", &row));

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader));

    EXPECT_TRUE(findLayersKnobRow(layersKnob, "diffuse", &row));
    EXPECT_EQ(std::string("0"), row[2]);
    EXPECT_TRUE(findLayersKnobRow(layersKnob, "specular", &row));

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    ASSERT_TRUE(bool(blur));
    connectNodes(reader, blur, 0, true);

    KnobChoicePtr outputLayerKnob = std::dynamic_pointer_cast<KnobChoice>(blur->getKnobByName(kOutputChannelsKnobName));
    ASSERT_TRUE(bool(outputLayerKnob));
    outputLayerKnob->setValueFromID("diffuse", 0);

    EXPECT_TRUE(findLayersKnobRow(layersKnob, "diffuse", &row));
    EXPECT_EQ(std::string("1"), row[2]);

    std::string error;
    EXPECT_FALSE(project->removeLayer("diffuse", &error));
    EXPECT_TRUE(findLayersKnobRow(layersKnob, "diffuse", &row));

    EXPECT_TRUE(project->removeLayer("specular", &error)) << error;
    EXPECT_FALSE(findLayersKnobRow(layersKnob, "specular", &row));

    project->reset(false, true);
    EXPECT_FALSE(findLayersKnobRow(layersKnob, "diffuse", &row));
    EXPECT_TRUE(findLayersKnobRow(layersKnob, "Color", &row));
} // TEST_F(BaseTest, LayersKnobMirrorsRegistryAndUsers)

static bool
containsLayerID(const std::list<ImageLayerDesc>& layers, const std::string& id)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->getLayerID() == id) {
            return true;
        }
    }

    return false;
}

// getPresentLayers()/getAvailableLayers() must key the input's components-needed query on the
// input's own hash, not the caller's: otherwise the answer is cached under the caller's hash in
// the input's ActionsCache, and the input's plane list can go stale (or pollute the input's cache)
// with entries the input's own hash never indexes.
//
// A connected node's hash folds in every upstream hash (Node::computeHashInternal()), so a real
// file change on the reader necessarily changes the Blur's hash too: that part isn't the
// regression to catch here. What this exercises is that querying the input through a downstream
// node (inputNb >= 0) still tracks the input's current metadata, the same way querying it directly
// (inputNb == -1) already did before this fix.
TEST_F(BaseTest, PresentLayersFollowInputMetadataChange)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    ASSERT_TRUE(bool(blur));

    connectNodes(reader, blur, 0, true);

    std::list<ImageLayerDesc> layersBefore;
    blur->getEffectInstance()->getPresentLayers(0, ViewIdx(0), 0, &layersBefore);
    EXPECT_TRUE(containsLayerID(layersBefore, "diffuse"));

    std::list<ImageLayerDesc> readerOwnLayersBefore;
    reader->getEffectInstance()->getPresentLayers(0, ViewIdx(0), -1, &readerOwnLayersBefore);
    EXPECT_TRUE(containsLayerID(readerOwnLayersBefore, "diffuse"));

    KnobFilePtr fileKnob = std::dynamic_pointer_cast<KnobFile>(reader->getKnobByName(kOfxImageEffectFileParamName));
    ASSERT_TRUE(bool(fileKnob));
    fileKnob->setValue(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    reader->forceRefreshAllInputRelatedData();

    std::list<ImageLayerDesc> readerOwnLayersAfter;
    reader->getEffectInstance()->getPresentLayers(0, ViewIdx(0), -1, &readerOwnLayersAfter);
    EXPECT_FALSE(containsLayerID(readerOwnLayersAfter, "diffuse"));
    EXPECT_FALSE(containsLayerID(readerOwnLayersAfter, "specular"));

    std::list<ImageLayerDesc> layersAfter;
    blur->getEffectInstance()->getPresentLayers(0, ViewIdx(0), 0, &layersAfter);
    EXPECT_FALSE(containsLayerID(layersAfter, "diffuse"));
    EXPECT_FALSE(containsLayerID(layersAfter, "specular"));

    project->reset(false, true);
} // TEST_F(BaseTest, PresentLayersFollowInputMetadataChange)
