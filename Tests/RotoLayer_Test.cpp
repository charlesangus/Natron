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

CLANG_DIAG_OFF(deprecated)
#include <QFile>
#include <QString>
#include <QTemporaryDir>
CLANG_DIAG_ON(deprecated)

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING

namespace {

// Tests/fixtures/flat-three-layers.exr: 8x8, every pixel Color (1, 0, 0, 1), diffuse (0, 1, 0),
// specular (0, 0, 1).
const int kSize = 8;

// The square drawn by addSquare(): control points (1, 6), (5, 6), (5, 2), (1, 2) in canonical
// coordinates, so it covers the pixel columns 1..4 and rows 2..5 exactly, with no feather.
const double kSquareX = 1.;
const double kSquareTop = 6.;
const double kSquareSize = 4.;
const int kInsideX = 2;
const int kInsideY = 3;
const int kOutsideX = 7;
const int kOutsideY = 7;

// EXR rows run top-down: the file's row 0 is Natron's y == height - 1.
float
at(const FlatExrImage& image,
   int x,
   int natronY,
   const std::string& channel)
{
    return image.at(x, image.height - 1 - natronY, channel);
}

void
expectChannelEverywhere(const FlatExrImage& image,
                        const std::string& channel,
                        float value)
{
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            EXPECT_EQ(value, at(image, x, y, channel)) << channel << " at (" << x << ", " << y << ")";
        }
    }
}

void
expectFixtureColor(const FlatExrImage& image)
{
    expectChannelEverywhere(image, "R", 1.f);
    expectChannelEverywhere(image, "G", 0.f);
    expectChannelEverywhere(image, "B", 0.f);
    expectChannelEverywhere(image, "A", 1.f);
}

void
expectFixtureSpecular(const FlatExrImage& image)
{
    expectChannelEverywhere(image, "specular.R", 0.f);
    expectChannelEverywhere(image, "specular.G", 0.f);
    expectChannelEverywhere(image, "specular.B", 1.f);
}

} // namespace

class RotoLayerTest
    : public BaseTest {
protected:
    NodePtr createRotoOnReader(const char* pluginID,
                               KnobLayerSelectPtr* layer)
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
        NodePtr reader = getApp()->createNode(readerArgs);
        if (!reader) {
            return NodePtr();
        }
        NodePtr roto = createNode(QString::fromUtf8(pluginID));
        if (!roto) {
            return NodePtr();
        }
        connectNodes(reader, roto, 0, true);
        *layer = std::dynamic_pointer_cast<KnobLayerSelect>(roto->getLayerKnob());

        return roto;
    }

    BezierPtr addSquare(const NodePtr& roto)
    {
        BezierPtr square = roto->getRotoContext()->makeSquare(kSquareX, kSquareTop, kSquareSize, 1.);
        if (square) {
            square->getFeatherKnob()->setValue(0.);
        }

        return square;
    }

    // Appends a WriteOIIO writing every layer of `node` as one 32-bit float part and renders
    // frame 1 into `tmp`, parsing the result into `image`.
    bool renderAllLayers(const NodePtr& node,
                         const QTemporaryDir& tmp,
                         FlatExrImage* image,
                         std::string* error)
    {
        NodePtr writer = createNode(_writeOIIOPluginID);
        if (!writer) {
            *error = "writer creation failed";

            return false;
        }
        connectNodes(node, writer, 0, true);

        KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
        KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
        KnobChannelSet* writerChannels = dynamic_cast<KnobChannelSet*>(writer->getKnobByName(kNodeParamChannelSet).get());
        if (!partSplitting || !bitDepth || !compression || !writerChannels) {
            *error = "writer knobs missing";

            return false;
        }
        partSplitting->setValueFromID("single", 0);
        bitDepth->setValueFromID("32f", 0);
        compression->setValueFromID("none", 0);
        writerChannels->setAll();

        const std::string path = (tmp.path() + QLatin1String("/out.exr")).toStdString();
        writer->setOutputFilesForWriter(path);
        QFile::remove(QString::fromStdString(path));

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        if (!writerEffect) {
            *error = "writer has no OutputEffectInstance";

            return false;
        }
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);

        if (!QFile::exists(QString::fromStdString(path))) {
            *error = "frame was not rendered: " + path;

            return false;
        }

        return readFlatExr(path, image, error);
    }
};

TEST_F(RotoLayerTest, RotoPaintWritesOneChannelOfDiffuseInsideTheShape)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTOPAINT, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("diffuse");
    std::vector<std::string> r(1, "R");
    layer->setChannels(r);

    // The tree renders the target plane in place: every node of it resolves "diffuse" against
    // the registry, whether or not its own inputs carry that plane.
    NodePtr bottomMerge = roto->getRotoContext()->getRotoPaintBottomMergeNode();
    ASSERT_TRUE(bool(bottomMerge));
    KnobChannelSetPtr mergeChannels = std::dynamic_pointer_cast<KnobChannelSet>(bottomMerge->getLayerKnob());
    ASSERT_TRUE(bool(mergeChannels));
    EXPECT_TRUE(bottomMerge->isTargetLayerKnob(mergeChannels));
    std::vector<ChannelSetRow> rows = mergeChannels->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string("diffuse"), rows[0].layerOrPattern);
    EXPECT_TRUE(rows[0].channels.empty());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(roto, tmp, &image, &error)) << error;

    EXPECT_EQ(1.f, at(image, kInsideX, kInsideY, "diffuse.R"));
    EXPECT_EQ(0.f, at(image, kOutsideX, kOutsideY, "diffuse.R"));
    EXPECT_EQ(0.f, at(image, 0, 0, "diffuse.R"));
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
    expectFixtureColor(image);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

TEST_F(RotoLayerTest, RotoPaintWritesATwoChannelRegistryLayerTheInputLacks)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> ab;
    ab.push_back("A");
    ab.push_back("B");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask2", "mask2", "", ab), LayerRegistryEntry::eOriginUser, &error)) << error;

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTOPAINT, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("mask2");

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    ASSERT_TRUE(renderAllLayers(roto, tmp, &image, &error)) << error;

    ASSERT_NE(-1, image.channelIndex("mask2.A"));
    ASSERT_NE(-1, image.channelIndex("mask2.B"));
    EXPECT_EQ(1.f, at(image, kInsideX, kInsideY, "mask2.A"));
    EXPECT_EQ(1.f, at(image, kInsideX, kInsideY, "mask2.B"));
    EXPECT_EQ(0.f, at(image, kOutsideX, kOutsideY, "mask2.A"));
    EXPECT_EQ(0.f, at(image, kOutsideX, kOutsideY, "mask2.B"));
    expectFixtureColor(image);
    expectChannelEverywhere(image, "diffuse.R", 0.f);
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

// The fixture is opaque everywhere, so "over" would leave alpha at 1 inside and out; the
// stencil operator (B(1-a)) punches the shape out of the alpha instead, which shows the
// shape reaching the alpha channel while the host copies R, G and B back from the input.
TEST_F(RotoLayerTest, RotoWritesAlphaOnlyIntoColor)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTO, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    EXPECT_EQ(std::string(kNatronColorLayerID), layer->getLayer());
    EXPECT_EQ(std::vector<std::string>(1, "A"), layer->getChannels());

    BezierPtr square = addSquare(roto);
    ASSERT_TRUE(bool(square));
    square->getOperatorKnob()->setValueFromID(Merge::getOperatorString(eMergeStencil), 0);
    roto->getRotoContext()->refreshRotoPaintTree();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(roto, tmp, &image, &error)) << error;

    EXPECT_EQ(0.f, at(image, kInsideX, kInsideY, "A"));
    EXPECT_EQ(1.f, at(image, kOutsideX, kOutsideY, "A"));
    EXPECT_EQ(1.f, at(image, 0, 0, "A"));
    expectChannelEverywhere(image, "R", 1.f);
    expectChannelEverywhere(image, "G", 0.f);
    expectChannelEverywhere(image, "B", 0.f);
    expectChannelEverywhere(image, "diffuse.R", 0.f);
    expectChannelEverywhere(image, "diffuse.G", 1.f);
    expectChannelEverywhere(image, "diffuse.B", 0.f);
    expectFixtureSpecular(image);

    project->reset(false, true);
}

// The RotoPaint container is the only knowing reference to "mask": its internal per-item
// Effect/Merge nodes and its global-merge node only follow the container via
// Node::retargetLayerKnob and must not each count as a separate user.
TEST_F(RotoLayerTest, LayerUsersCountOnlyTheRotoPaintContainer)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> a(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", a), LayerRegistryEntry::eOriginUser, &error)) << error;

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTOPAINT, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("mask");

    std::list<NodePtr> users;
    project->getLayerUsers("mask", &users);
    ASSERT_EQ(1u, users.size());
    EXPECT_EQ(roto.get(), users.front().get());

    EXPECT_FALSE(project->removeLayer("mask", &error));
    const std::string rotoName = roto->getScriptName_mt_safe();
    ASSERT_NE(std::string::npos, error.find(rotoName)) << error;
    EXPECT_EQ(error.find(rotoName), error.rfind(rotoName)) << error;
    EXPECT_EQ(std::string::npos, error.find("_Effect")) << error;
    EXPECT_EQ(std::string::npos, error.find("_Merge")) << error;
    EXPECT_EQ(std::string::npos, error.find("globalMerge")) << error;

    project->reset(false, true);
}

// Roto (single-shape, non-paint) has the same internal Effect/Merge nodes as RotoPaint but no
// global-merge node; the container is still the sole user.
TEST_F(RotoLayerTest, LayerUsersCountOnlyTheRotoContainer)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> a(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", a), LayerRegistryEntry::eOriginUser, &error)) << error;

    KnobLayerSelectPtr layer;
    NodePtr roto = createRotoOnReader(PLUGINID_NATRON_ROTO, &layer);
    ASSERT_TRUE(bool(roto));
    ASSERT_TRUE(bool(layer));
    ASSERT_TRUE(bool(addSquare(roto)));

    layer->setLayer("mask");

    std::list<NodePtr> users;
    project->getLayerUsers("mask", &users);
    ASSERT_EQ(1u, users.size());
    EXPECT_EQ(roto.get(), users.front().get());

    EXPECT_FALSE(project->removeLayer("mask", &error));
    const std::string rotoName = roto->getScriptName_mt_safe();
    ASSERT_NE(std::string::npos, error.find(rotoName)) << error;
    EXPECT_EQ(error.find(rotoName), error.rfind(rotoName)) << error;
    EXPECT_EQ(std::string::npos, error.find("_Effect")) << error;
    EXPECT_EQ(std::string::npos, error.find("_Merge")) << error;

    project->reset(false, true);
}
