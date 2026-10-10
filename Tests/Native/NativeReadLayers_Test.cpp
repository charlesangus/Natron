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
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QDir>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kPartWidth = 16;
const int kPartHeight = 12;

struct Part {
    const char* name;
    std::vector<std::string> channels;
};

float
partValue(int x,
          int y,
          int channel)
{
    return (float)((x * 37 + y * 11 + channel * 5) % 97) / 64.f;
}

// One file channel to compare with: the subimage that holds it and its name there.
struct FileChannel {
    int subimage;
    std::string name;
};

// OIIO's own read of one channel, as float, top row first.
bool
readChannelWithOiio(const std::string& path,
                    const FileChannel& channel,
                    OIIO::ImageSpec* spec,
                    std::vector<float>* pixels)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    if (!input || !input->seek_subimage(channel.subimage, 0)) {
        return false;
    }
    *spec = input->spec();
    const std::vector<std::string>::const_iterator found = std::find(spec->channelnames.begin(), spec->channelnames.end(), channel.name);
    if (found == spec->channelnames.end()) {
        input->close();

        return false;
    }
    const int index = (int)(found - spec->channelnames.begin());
    pixels->resize((std::size_t)spec->width * (std::size_t)spec->height);
    const bool ok = input->read_image(channel.subimage, 0, index, index + 1, OIIO::TypeDesc::FLOAT, pixels->data());
    input->close();

    return ok;
}

std::vector<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (!it->isColorLayer()) {
            ids.push_back(it->getLayerID());
        }
    }

    return ids;
}

const ImageLayerDesc*
findProduced(const std::list<ImageLayerDesc>& layers,
             const std::string& id)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->getLayerID() == id) {
            return &*it;
        }
    }

    return NULL;
}
} // namespace

class NativeReadLayersTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // The file is given at creation, as the Read is created from a file browser or a drop.
    NodePtr createRead(const std::string& path)
    {
        CreateNodeArgs args(PLUGINID_NATRON_READ, getApp()->getProject());
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_READ, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, 0, 1);
        args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, path);
        NodePtr node = getApp()->createNode(args);
        EXPECT_TRUE(bool(node));
        EXPECT_TRUE(node && dynamic_cast<NativeRead*>(node->getEffectInstance().get()));
        if (node) {
            // These tests compare with the raw pixels, so the file is read without conversion.
            KnobStringBase* inputSpace = dynamic_cast<KnobStringBase*>(node->getKnobByName("ocioInputSpace").get());
            EXPECT_TRUE(inputSpace != NULL);
            if (inputSpace) {
                inputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
            }
        }

        return node;
    }

    std::list<ImageLayerDesc> producedLayers(const NodePtr& node)
    {
        EffectInstance::ComponentsNeededMap comps;
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime = 0.;
        int passThroughView = 0;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInputNb = -1;

        node->getEffectInstance()->getComponentsNeededAndProduced_public(node->getHashValue(), 1., ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

        return comps[-1];
    }

    // A temporary directory under the build tree, which ctest runs in.
    static QString temporaryTemplate()
    {
        return QDir::current().absoluteFilePath(QString::fromUtf8("NativeReadLayers-XXXXXX"));
    }

    std::string writeMultiPart(const QTemporaryDir& dir,
                               const std::vector<Part>& parts)
    {
        const std::string path = (dir.path() + QString::fromUtf8("/multi-part.exr")).toStdString();
        std::vector<OIIO::ImageSpec> specs;
        std::vector<std::vector<float>> pixels;
        int channelBase = 0;
        for (std::size_t p = 0; p < parts.size(); ++p) {
            const int nChannels = (int)parts[p].channels.size();
            OIIO::ImageSpec spec(kPartWidth, kPartHeight, nChannels, OIIO::TypeDesc::FLOAT);
            spec.channelnames = parts[p].channels;
            spec.attribute("oiio:subimagename", parts[p].name);
            specs.push_back(spec);

            std::vector<float> values((std::size_t)kPartWidth * (std::size_t)kPartHeight * (std::size_t)nChannels);
            for (int y = 0; y < kPartHeight; ++y) {
                for (int x = 0; x < kPartWidth; ++x) {
                    for (int c = 0; c < nChannels; ++c) {
                        values[((std::size_t)y * kPartWidth + (std::size_t)x) * (std::size_t)nChannels + (std::size_t)c] = partValue(x, y, channelBase + c);
                    }
                }
            }
            pixels.push_back(values);
            channelBase += nChannels;
        }

        OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
        EXPECT_TRUE(bool(out));
        if (!out) {
            return std::string();
        }
        EXPECT_TRUE(out->supports("multiimage"));
        if (!out->open(path, (int)specs.size(), specs.data())) {
            ADD_FAILURE() << out->geterror();

            return std::string();
        }
        for (std::size_t p = 0; p < specs.size(); ++p) {
            if (p > 0 && !out->open(path, specs[p], OIIO::ImageOutput::AppendSubimage)) {
                ADD_FAILURE() << out->geterror();

                return std::string();
            }
            if (!out->write_image(OIIO::TypeDesc::FLOAT, pixels[p].data())) {
                ADD_FAILURE() << out->geterror();

                return std::string();
            }
        }
        EXPECT_TRUE(out->close()) << out->geterror();

        return path;
    }

    // Renders `layer` of `node` over the file's data window and compares each of its channels
    // with OIIO's own read of the file channel it comes from.
    void expectPlaneMatchesOiio(const NodePtr& node,
                                const std::string& path,
                                const ImageLayerDesc& layer,
                                const std::vector<FileChannel>& sources)
    {
        SCOPED_TRACE(layer.getLayerID());
        ASSERT_EQ((std::size_t)layer.getNumComponents(), sources.size());

        std::vector<OIIO::ImageSpec> specs(sources.size());
        std::vector<std::vector<float>> reference(sources.size());
        for (std::size_t c = 0; c < sources.size(); ++c) {
            ASSERT_TRUE(readChannelWithOiio(path, sources[c], &specs[c], &reference[c])) << sources[c].name;
        }
        const RectI window = OiioReadSupport::dataWindowOf(specs[0]);

        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, layer), &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        const std::size_t nComps = sources.size();
        ASSERT_EQ(nComps, planes[0].channels.size());
        const std::size_t width = (std::size_t)window.width();
        const std::size_t height = (std::size_t)window.height();
        ASSERT_EQ(width * height * nComps, planes[0].pixels.size());

        std::size_t mismatches = 0;
        for (std::size_t row = 0; row < height; ++row) {
            const std::size_t fileRow = height - 1 - row;
            for (std::size_t col = 0; col < width; ++col) {
                for (std::size_t c = 0; c < nComps; ++c) {
                    const float got = planes[0].pixels[(row * width + col) * nComps + c];
                    const float want = reference[c][fileRow * width + col];
                    if (got != want && ++mismatches <= 5) {
                        ADD_FAILURE() << "pixel " << col << "," << row << " channel " << sources[c].name << ": got " << got << ", want " << want;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
    }

    void expectRegistered(const std::string& id,
                          const std::vector<std::string>& channels)
    {
        ImageLayerDesc registered;
        ASSERT_TRUE(getApp()->getProject()->findLayer(id, &registered)) << id;
        EXPECT_EQ(channels, registered.getChannels()) << id;
    }
};

TEST_F(NativeReadLayersTest, EveryLayerOfASinglePartFileIsProducedBitForBit)
{
    const std::string path = std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr");
    NodePtr node = createRead(path);
    ASSERT_TRUE(bool(node));

    const std::list<ImageLayerDesc> produced = producedLayers(node);
    EXPECT_EQ(std::vector<std::string>({ "diffuse", "specular" }), layerIDs(produced));

    const char* const ids[] = { "diffuse", "specular" };
    for (const char* id : ids) {
        const ImageLayerDesc* layer = findProduced(produced, id);
        ASSERT_TRUE(layer != NULL) << id;
        EXPECT_EQ(std::vector<std::string>({ "R", "G", "B" }), layer->getChannels());
        std::vector<FileChannel> sources;
        for (const char* channel : { "R", "G", "B" }) {
            sources.push_back(FileChannel { 0, std::string(id) + "." + channel });
        }
        expectPlaneMatchesOiio(node, path, *layer, sources);
    }

    std::vector<FileChannel> colour;
    for (const char* channel : { "R", "G", "B", "A" }) {
        colour.push_back(FileChannel { 0, channel });
    }
    expectPlaneMatchesOiio(node, path, ImageLayerDesc::getRGBAComponents(), colour);
}

TEST_F(NativeReadLayersTest, AFileWithoutColourProducesItsLayersAndABlackColourPlane)
{
    const std::string path = std::string(NATRON_TESTS_FIXTURES_DIR "/flat-no-color-layers.exr");
    NodePtr node = createRead(path);
    ASSERT_TRUE(bool(node));

    const std::list<ImageLayerDesc> produced = producedLayers(node);
    EXPECT_EQ(std::vector<std::string>({ "diffuse", "specular" }), layerIDs(produced));

    const char* const ids[] = { "diffuse", "specular" };
    for (const char* id : ids) {
        const ImageLayerDesc* layer = findProduced(produced, id);
        ASSERT_TRUE(layer != NULL) << id;
        std::vector<FileChannel> sources;
        for (const char* channel : { "R", "G", "B" }) {
            sources.push_back(FileChannel { 0, std::string(id) + "." + channel });
        }
        expectPlaneMatchesOiio(node, path, *layer, sources);
    }

    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 8, 8), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
        EXPECT_EQ(0.f, planes[0].pixels[i]) << i;
    }
}

TEST_F(NativeReadLayersTest, PartNamesNameTheLayersOfAMultiPartFile)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());
    std::vector<Part> parts;
    parts.push_back(Part { "rgba", { "R", "G", "B", "A" } });
    parts.push_back(Part { "key", { "R", "G", "B" } });
    parts.push_back(Part { "extra", { "light.R", "light.G", "mask" } });
    parts.push_back(Part { "wide", { "c0", "c1", "c2", "c3", "c4" } });
    const std::string path = writeMultiPart(dir, parts);
    ASSERT_FALSE(path.empty());

    NodePtr node = createRead(path);
    ASSERT_TRUE(bool(node));

    // The five channels of "wide" are over the cap, so like the registry the Read refuses it.
    const std::list<ImageLayerDesc> produced = producedLayers(node);
    // OIIO's EXR reader decides the channel order within a part, so only the set of layers is meaningful.
    std::vector<std::string> ids = layerIDs(produced);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::vector<std::string>({ "extra", "key", "light" }), ids);

    const ImageLayerDesc* key = findProduced(produced, "key");
    ASSERT_TRUE(key != NULL);
    EXPECT_EQ(std::vector<std::string>({ "R", "G", "B" }), key->getChannels());
    expectPlaneMatchesOiio(node, path, *key, { FileChannel { 1, "R" }, FileChannel { 1, "G" }, FileChannel { 1, "B" } });

    const ImageLayerDesc* light = findProduced(produced, "light");
    ASSERT_TRUE(light != NULL);
    EXPECT_EQ(std::vector<std::string>({ "R", "G" }), light->getChannels());
    expectPlaneMatchesOiio(node, path, *light, { FileChannel { 2, "light.R" }, FileChannel { 2, "light.G" } });

    const ImageLayerDesc* extra = findProduced(produced, "extra");
    ASSERT_TRUE(extra != NULL);
    EXPECT_EQ(std::vector<std::string>({ "mask" }), extra->getChannels());
    expectPlaneMatchesOiio(node, path, *extra, { FileChannel { 2, "mask" } });

    expectPlaneMatchesOiio(node, path, ImageLayerDesc::getRGBAComponents(), { FileChannel { 0, "R" }, FileChannel { 0, "G" }, FileChannel { 0, "B" }, FileChannel { 0, "A" } });
}

// Natron's Write names the colour part "Color", a name the registry refuses for a layer.
TEST_F(NativeReadLayersTest, APartNamedAfterTheColourPlaneIsNotAlsoALayer)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());
    std::vector<Part> parts;
    parts.push_back(Part { "Color", { "R", "G", "B", "A" } });
    parts.push_back(Part { "key", { "R", "G", "B" } });
    const std::string path = writeMultiPart(dir, parts);
    ASSERT_FALSE(path.empty());

    NodePtr node = createRead(path);
    ASSERT_TRUE(bool(node));

    EXPECT_EQ(std::vector<std::string>({ "key" }), layerIDs(producedLayers(node)));
    expectPlaneMatchesOiio(node, path, ImageLayerDesc::getRGBAComponents(), { FileChannel { 0, "R" }, FileChannel { 0, "G" }, FileChannel { 0, "B" }, FileChannel { 0, "A" } });
}

TEST_F(NativeReadLayersTest, TheRegistryListsTheFilesLayersAfterCreation)
{
    EXPECT_FALSE(getApp()->getProject()->findLayer("diffuse", NULL));
    EXPECT_FALSE(getApp()->getProject()->findLayer("specular", NULL));

    NodePtr single = createRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));
    ASSERT_TRUE(bool(single));
    expectRegistered("diffuse", { "R", "G", "B" });
    expectRegistered("specular", { "R", "G", "B" });

    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());
    std::vector<Part> parts;
    parts.push_back(Part { "rgba", { "R", "G", "B", "A" } });
    parts.push_back(Part { "key", { "R", "G", "B" } });
    parts.push_back(Part { "wide", { "c0", "c1", "c2", "c3", "c4" } });
    const std::string path = writeMultiPart(dir, parts);
    ASSERT_FALSE(path.empty());
    EXPECT_FALSE(getApp()->getProject()->findLayer("key", NULL));

    NodePtr multi = createRead(path);
    ASSERT_TRUE(bool(multi));
    expectRegistered("key", { "R", "G", "B" });
    EXPECT_FALSE(getApp()->getProject()->findLayer("wide", NULL));
}

TEST_F(NativeReadLayersTest, AGradeOnAFileLayerProcessesThatLayer)
{
    const std::string path = std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr");
    NodePtr read = createRead(path);
    ASSERT_TRUE(bool(read));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));
    connectNodes(read, grade, 0, true);

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(grade->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, "diffuse", NULL);
    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName(kGradeParamMultiply).get());
    ASSERT_TRUE(multiply != NULL);
    multiply->setValues(0.5, 0.5, 0.5, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    ImageLayerDesc diffuse;
    ASSERT_TRUE(getApp()->getProject()->findLayer("diffuse", &diffuse));
    const RectI window(0, 0, 8, 8);
    std::list<ImageLayerDesc> layers;
    layers.push_back(diffuse);
    layers.push_back(ImageLayerDesc::getRGBAComponents());

    std::vector<RenderedPlane> readPlanes;
    std::vector<RenderedPlane> gradePlanes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(read, 1., ViewIdx(0), 0, window, layers, &readPlanes, &error)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(grade, 1., ViewIdx(0), 0, window, layers, &gradePlanes, &error)) << error;
    ASSERT_EQ(2u, readPlanes.size());
    ASSERT_EQ(2u, gradePlanes.size());

    // The fixture's diffuse is (0, 1, 0) everywhere, so the graded plane is (0, 0.5, 0).
    ASSERT_EQ(readPlanes[0].pixels.size(), gradePlanes[0].pixels.size());
    ASSERT_EQ((std::size_t)window.width() * (std::size_t)window.height() * 3, gradePlanes[0].pixels.size());
    for (std::size_t i = 0; i < gradePlanes[0].pixels.size(); ++i) {
        EXPECT_EQ(i % 3 == 1 ? 1.f : 0.f, readPlanes[0].pixels[i]) << i;
        EXPECT_NEAR(0.5f * readPlanes[0].pixels[i], gradePlanes[0].pixels[i], 1e-6f) << i;
    }

    ASSERT_EQ(readPlanes[1].pixels.size(), gradePlanes[1].pixels.size());
    for (std::size_t i = 0; i < gradePlanes[1].pixels.size(); ++i) {
        EXPECT_EQ(readPlanes[1].pixels[i], gradePlanes[1].pixels[i]) << i;
    }
}
