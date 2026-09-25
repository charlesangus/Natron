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

#include <algorithm>
#include <bitset>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "FlatExrReader.h"
#include "MultiplanarTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

struct NeededComponents {
    U64 hash;
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInputNb;

    NeededComponents()
        : hash(0)
        , comps()
        , passThroughLayers()
        , processChannels()
        , processChannelsPerPlane()
        , passThroughInputNb(-1)
    {
    }
};

NeededComponents
queryNeededComponents(const NodePtr& node)
{
    NeededComponents ret;
    EffectInstancePtr effect = node->getEffectInstance();
    double passThroughTime = 0.;
    int passThroughView = 0;

    ret.hash = effect->getRenderHash();
    effect->getComponentsNeededAndProduced_public(ret.hash, 0, ViewIdx(0), &ret.comps, &ret.passThroughLayers, &passThroughTime, &passThroughView, &ret.processChannels, &ret.processChannelsPerPlane, &ret.passThroughInputNb);

    return ret;
}

std::vector<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        ids.push_back(it->getLayerID());
    }

    return ids;
}

std::vector<std::string>
outputLayerIDs(const NeededComponents& needed)
{
    EffectInstance::ComponentsNeededMap::const_iterator found = needed.comps.find(-1);

    return found == needed.comps.end() ? std::vector<std::string>() : layerIDs(found->second);
}

std::bitset<4>
bits(bool r,
     bool g,
     bool b,
     bool a)
{
    std::bitset<4> ret;

    ret[0] = r;
    ret[1] = g;
    ret[2] = b;
    ret[3] = a;

    return ret;
}

std::bitset<4>
planeBits(const NeededComponents& needed,
          const std::string& layerID)
{
    for (EffectInstance::ProcessChannelsPerPlaneMap::const_iterator it = needed.processChannelsPerPlane.begin(); it != needed.processChannelsPerPlane.end(); ++it) {
        if (it->first.getLayerID() == layerID) {
            return it->second;
        }
    }

    return std::bitset<4>();
}

// The fixture is spatially constant, so any in-bounds pixel stands for the whole plane.
const int32_t kCheckX = 1;
const int32_t kCheckY = 1;

void
expectPlane(const FlatExrImage& image,
            const std::string& prefix,
            float r,
            float g,
            float b)
{
    EXPECT_NEAR(r, image.at(kCheckX, kCheckY, prefix + "R"), 1e-4f) << prefix << "R";
    EXPECT_NEAR(g, image.at(kCheckX, kCheckY, prefix + "G"), 1e-4f) << prefix << "G";
    EXPECT_NEAR(b, image.at(kCheckX, kCheckY, prefix + "B"), 1e-4f) << prefix << "B";
}

void
expectColor(const FlatExrImage& image,
            float r,
            float g,
            float b,
            float a)
{
    expectPlane(image, "", r, g, b);
    EXPECT_NEAR(a, image.at(kCheckX, kCheckY, "A"), 1e-4f) << "A";
}

} // namespace

class LayerKnobsRenderTest
    : public BaseTest {
protected:
    NodePtr createReader(const std::string& fixture = "flat-three-layers.exr")
    {
        CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);

        return getApp()->createNode(readerArgs);
    }

    NodePtr createInvertOnReader(KnobChannelSetPtr* channels,
                                 const std::string& fixture = "flat-three-layers.exr")
    {
        NodePtr reader = createReader(fixture);
        if (!reader) {
            return NodePtr();
        }
        NodePtr invert = createNode(QString::fromUtf8("net.sf.openfx.Invert"));
        if (!invert) {
            return NodePtr();
        }
        connectNodes(reader, invert, 0, true);
        *channels = std::dynamic_pointer_cast<KnobChannelSet>(invert->getKnobByName(kNodeParamChannelSet));

        return invert;
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

    // True when the node reports itself an identity of its input 0 at frame 0.
    bool isIdentityOfSource(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI window(0, 0, 8, 8);
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;
        const bool identity = effect->isIdentity_public(false, effect->getRenderHash(), 0, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

        return identity && inputNb == 0;
    }
};

TEST_F(LayerKnobsRenderTest, ChannelSetRowsSelectOutputAndInputPlanesWithPerPlaneBits)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    const U64 defaultHash = queryNeededComponents(invert).hash;

    channels->setLayer(0, kNatronColorLayerID, NULL);
    std::vector<std::string> rg;
    rg.push_back("R");
    rg.push_back("G");
    channels->addLayer("diffuse", &rg);

    NeededComponents needed = queryNeededComponents(invert);
    EXPECT_NE(defaultHash, needed.hash);

    std::vector<std::string> expected;
    expected.push_back(kNatronColorLayerID);
    expected.push_back("diffuse");
    EXPECT_EQ(expected, outputLayerIDs(needed));

    EffectInstance::ComponentsNeededMap::const_iterator output = needed.comps.find(-1);
    ASSERT_NE(needed.comps.end(), output);
    EXPECT_EQ(4, output->second.front().getNumComponents());

    EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
    ASSERT_NE(needed.comps.end(), source);
    EXPECT_EQ(output->second, source->second);

    ASSERT_EQ(2u, needed.processChannelsPerPlane.size());
    EXPECT_EQ(bits(true, true, true, true), planeBits(needed, kNatronColorLayerID));
    EXPECT_EQ(bits(true, true, false, false), planeBits(needed, "diffuse"));
    EXPECT_EQ(bits(true, true, true, true), needed.processChannels);

    // Lookups go by layer ID, so a descriptor carrying another channel list still finds its entry.
    EffectInstancePtr effect = invert->getEffectInstance();
    ImageLayerDesc diffuse("diffuse", "diffuse", "RG", rg);
    EXPECT_EQ(bits(true, true, false, false), effect->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), diffuse));
    ImageLayerDesc specular("specular", "specular", "RG", rg);
    EXPECT_EQ(bits(true, true, true, true), effect->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), specular));
}

TEST_F(LayerKnobsRenderTest, ChannelSetAllSelectsEveryPresentPlane)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    channels->setAll();

    NeededComponents needed = queryNeededComponents(invert);

    std::vector<std::string> expected;
    expected.push_back(kNatronColorLayerID);
    expected.push_back("diffuse");
    expected.push_back("specular");
    EXPECT_EQ(expected, outputLayerIDs(needed));

    EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
    ASSERT_NE(needed.comps.end(), source);
    EXPECT_EQ(expected, layerIDs(source->second));

    ASSERT_EQ(3u, needed.processChannelsPerPlane.size());
    EXPECT_EQ(bits(true, true, true, true), planeBits(needed, kNatronColorLayerID));
    EXPECT_EQ(bits(true, true, true, false), planeBits(needed, "diffuse"));
    EXPECT_EQ(bits(true, true, true, false), planeBits(needed, "specular"));
    EXPECT_EQ(bits(true, true, true, true), needed.processChannels);
}

// A selection resolving to nothing falls back to the metadata Color plane with no per-plane
// entry, so the plane is processed on every channel until the node reports identity for it.
TEST_F(LayerKnobsRenderTest, EmptySelectionFallsBackToMetadataColor)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);

    channels->setRegex(0, "nothing.*");
    {
        NeededComponents needed = queryNeededComponents(invert);
        EXPECT_EQ(colorOnly, outputLayerIDs(needed));
        EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
        ASSERT_NE(needed.comps.end(), source);
        EXPECT_EQ(colorOnly, layerIDs(source->second));
        EXPECT_TRUE(needed.processChannelsPerPlane.empty());
        EXPECT_EQ(bits(true, true, true, true), needed.processChannels);
        EXPECT_EQ(bits(true, true, true, true), invert->getEffectInstance()->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), ImageLayerDesc::getRGBAComponents()));
    }

    channels->setNone();
    {
        NeededComponents needed = queryNeededComponents(invert);
        EXPECT_EQ(colorOnly, outputLayerIDs(needed));
        EXPECT_TRUE(needed.processChannelsPerPlane.empty());
        EXPECT_EQ(bits(true, true, true, true), needed.processChannels);
    }
}

TEST_F(LayerKnobsRenderTest, GradeDefaultRowMasksAlphaThroughTheColorBits)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));
    ASSERT_TRUE(bool(grade));
    connectNodes(reader, grade, 0, true);

    NeededComponents needed = queryNeededComponents(grade);

    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);
    EXPECT_EQ(colorOnly, outputLayerIDs(needed));
    EXPECT_EQ(bits(true, true, true, false), planeBits(needed, kNatronColorLayerID));
    EXPECT_EQ(bits(true, true, true, false), needed.processChannels);
}

TEST_F(LayerKnobsRenderTest, TargetLayerSelectProducesTheChosenPlane)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr constant = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
    ASSERT_TRUE(bool(constant));
    KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(constant->getKnobByName(kNodeParamLayerSelect));
    ASSERT_TRUE(bool(layer));

    {
        NeededComponents needed = queryNeededComponents(constant);
        std::vector<std::string> colorOnly;
        colorOnly.push_back(kNatronColorLayerID);
        EXPECT_EQ(colorOnly, outputLayerIDs(needed));
        EXPECT_EQ(bits(true, true, true, true), planeBits(needed, kNatronColorLayerID));
    }

    layer->setLayer("depth");
    {
        NeededComponents needed = queryNeededComponents(constant);
        std::vector<std::string> depthOnly;
        depthOnly.push_back("depth");
        EXPECT_EQ(depthOnly, outputLayerIDs(needed));
        ASSERT_EQ(1u, needed.processChannelsPerPlane.size());
        EXPECT_EQ(bits(false, false, false, true), planeBits(needed, "depth"));
        EXPECT_EQ(bits(false, false, false, true), needed.processChannels);
    }

    project->reset(false, true);
}

TEST_F(LayerKnobsRenderTest, NodesWithoutLayerKnobKeepMetadataPlanes)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr premult = createNode(QString::fromUtf8("net.sf.openfx.Premult"));
    ASSERT_TRUE(bool(premult));
    ASSERT_FALSE(bool(premult->getLayerKnob()));
    connectNodes(reader, premult, 0, true);

    NeededComponents needed = queryNeededComponents(premult);

    std::vector<std::string> colorOnly;
    colorOnly.push_back(kNatronColorLayerID);
    EXPECT_EQ(colorOnly, outputLayerIDs(needed));
    EffectInstance::ComponentsNeededMap::const_iterator source = needed.comps.find(0);
    ASSERT_NE(needed.comps.end(), source);
    EXPECT_EQ(colorOnly, layerIDs(source->second));
    EXPECT_TRUE(needed.processChannelsPerPlane.empty());
    EXPECT_EQ(bits(true, true, true, true), premult->getEffectInstance()->getProcessChannelsForPlane(needed.hash, 0, ViewIdx(0), ImageLayerDesc::getRGBAComponents()));
}

// Rendering plane L reads plane L and masks with L's own bits: Color loses only its R, diffuse
// only its G, and the unselected specular plane passes through.
TEST_F(LayerKnobsRenderTest, InvertReadsAndMasksEachPlaneOnItsOwn)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    std::vector<std::string> r;
    r.push_back("R");
    channels->setLayer(0, kNatronColorLayerID, &r);
    std::vector<std::string> g;
    g.push_back("G");
    channels->addLayer("diffuse", &g);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 0.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 0.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(LayerKnobsRenderTest, InvertOnOneNonColorPlaneLeavesTheOthersUntouched)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, "diffuse", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 1.f, 0.f, 1.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// Grade's adopted channel quad is forced on, so its alpha survives only through host masking
// with the Color row's default A-off bits.
TEST_F(LayerKnobsRenderTest, GradeDefaultRowKeepsAlphaByHostMasking)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));
    ASSERT_TRUE(bool(grade));
    connectNodes(reader, grade, 0, true);

    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    multiply->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(grade, tmp, &image, &error)) << error;

    expectColor(image, 0.5f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// A constant plane blurred with nearest-pixel borders is itself, so a real (non-identity) blur
// of one plane must leave all three planes equal to the input.
TEST_F(LayerKnobsRenderTest, BlurOnOnePlaneRoutesEveryPlaneThrough)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    ASSERT_TRUE(bool(blur));
    connectNodes(reader, blur, 0, true);

    KnobDouble* size = dynamic_cast<KnobDouble*>(blur->getKnobByName("size").get());
    ASSERT_TRUE(size != NULL);
    size->setValues(3., 3., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    KnobChoice* boundary = dynamic_cast<KnobChoice*>(blur->getKnobByName("boundary").get());
    ASSERT_TRUE(boundary != NULL);
    boundary->setValueFromID("nearest", 0);
    KnobBool* expandRoD = dynamic_cast<KnobBool*>(blur->getKnobByName("expandRoD").get());
    ASSERT_TRUE(expandRoD != NULL);
    expandRoD->setValue(false);

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(blur->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, "specular", NULL);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(blur, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// An identity node hands the caller's planes through from its input: the diffuse plane must
// not come back relabelled as Color.
TEST_F(LayerKnobsRenderTest, IdentityBlurPassesEveryPlaneThroughUnchanged)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr blur = createNode(QString::fromUtf8("net.sf.cimg.CImgBlur"));
    ASSERT_TRUE(bool(blur));
    connectNodes(reader, blur, 0, true);

    KnobDouble* size = dynamic_cast<KnobDouble*>(blur->getKnobByName("size").get());
    ASSERT_TRUE(size != NULL);
    EXPECT_EQ(0., size->getValue(0));
    EXPECT_EQ(0., size->getValue(1));
    EXPECT_TRUE(isIdentityOfSource(blur));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(blur, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(LayerKnobsRenderTest, ChannelSetNoneMakesTheNodeAnIdentity)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    EXPECT_FALSE(isIdentityOfSource(invert));
    channels->setNone();
    EXPECT_FALSE(invert->hasAtLeastOneChannelToProcess(0, ViewIdx(0)));
    EXPECT_TRUE(isIdentityOfSource(invert));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

// A row naming only channels its layer does not have resolves to no channel at all.
TEST_F(LayerKnobsRenderTest, ChannelSetRowWithNoMatchingChannelMakesTheNodeAnIdentity)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels);

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    std::vector<std::string> q;
    q.push_back("Q");
    channels->setLayer(0, kNatronColorLayerID, &q);
    EXPECT_FALSE(invert->hasAtLeastOneChannelToProcess(0, ViewIdx(0)));
    EXPECT_TRUE(isIdentityOfSource(invert));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    expectPlane(image, "diffuse.", 0.f, 1.f, 0.f);
    expectPlane(image, "specular.", 0.f, 0.f, 1.f);
}

TEST_F(LayerKnobsRenderTest, SelectingALayerTheInputLacksMakesTheNodeAnIdentity)
{
    KnobChannelSetPtr channels;
    NodePtr invert = createInvertOnReader(&channels, "flat-rgba-only.exr");

    ASSERT_TRUE(bool(invert));
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, "diffuse", NULL);
    EXPECT_FALSE(invert->hasAtLeastOneChannelToProcess(0, ViewIdx(0)));
    EXPECT_TRUE(isIdentityOfSource(invert));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(renderAllLayers(invert, tmp, &image, &error)) << error;

    expectColor(image, 1.f, 0.f, 0.f, 1.f);
    EXPECT_EQ(-1, image.channelIndex("diffuse.R"));
}

// A multiplanar effect that only ever writes "diffuse" (a Shuffle writing one output layer, in
// shape) must not have Color forced into its produced planes: with the opt-out flag set, Color
// has to come from the pass-through list instead, alongside the other layer the effect ignores.
TEST_F(LayerKnobsRenderTest, MultiplanarEffectCanOptOutOfProducingTheMetadataLayer)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));

    NodePtr shuffle = createNode(QString::fromUtf8(kTestPluginIDMultiplanarDiffuseOnly));
    ASSERT_TRUE(bool(shuffle));
    connectNodes(reader, shuffle, 0, true);

    MultiplanarDiffuseOnlyTestEffect* effect = dynamic_cast<MultiplanarDiffuseOnlyTestEffect*>(shuffle->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    effect->setProducesMetadataLayerImplicitly(false);

    NeededComponents needed = queryNeededComponents(shuffle);

    std::vector<std::string> expectedProduced;
    expectedProduced.push_back("diffuse");
    EXPECT_EQ(expectedProduced, outputLayerIDs(needed));

    const std::vector<std::string> passThrough = layerIDs(needed.passThroughLayers);
    EXPECT_NE(passThrough.end(), std::find(passThrough.begin(), passThrough.end(), std::string(kNatronColorLayerID)));
    EXPECT_NE(passThrough.end(), std::find(passThrough.begin(), passThrough.end(), std::string("specular")));
}

// The default keeps today's behaviour: an effect that does not opt out still gets Color merged
// into its produced planes.
TEST_F(LayerKnobsRenderTest, MultiplanarEffectProducesTheMetadataLayerByDefault)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));

    NodePtr shuffle = createNode(QString::fromUtf8(kTestPluginIDMultiplanarDiffuseOnly));
    ASSERT_TRUE(bool(shuffle));
    connectNodes(reader, shuffle, 0, true);

    NeededComponents needed = queryNeededComponents(shuffle);

    const std::vector<std::string> produced = outputLayerIDs(needed);
    EXPECT_NE(produced.end(), std::find(produced.begin(), produced.end(), std::string(kNatronColorLayerID)));
}
