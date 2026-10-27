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
#include <initializer_list>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kSize = 8;

// One distinct constant per channel, so a channel written under the wrong name or position shows.
struct SourceChannel {
    const char* name;
    float value;
};

const SourceChannel kSourceChannels[] = {
    { "R", 0.125f }, { "G", 0.25f }, { "B", 0.375f }, { "A", 1.f }, { "diffuse.R", 0.5f }, { "diffuse.G", 0.625f }, { "diffuse.B", 0.75f }, { "specular.R", 0.0625f }, { "specular.G", 0.1875f }, { "specular.B", 0.3125f }, { "mask.A", 0.875f }
};

bool
writeSource(const std::string& path,
            std::string* error)
{
    const int n = (int)(sizeof(kSourceChannels) / sizeof(kSourceChannels[0]));
    OIIO::ImageSpec spec(kSize, kSize, n, OIIO::TypeDesc::FLOAT);

    spec.channelnames.clear();
    for (int c = 0; c < n; ++c) {
        spec.channelnames.push_back(kSourceChannels[c].name);
    }
    spec.alpha_channel = 3;
    spec.attribute("compression", "none");
    std::vector<float> pixels((std::size_t)kSize * kSize * n);
    for (int p = 0; p < kSize * kSize; ++p) {
        for (int c = 0; c < n; ++c) {
            pixels[(std::size_t)p * n + c] = kSourceChannels[c].value;
        }
    }
    std::unique_ptr<OIIO::ImageOutput> output = OIIO::ImageOutput::create(path);
    if (!output || !output->open(path, spec) || !output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !output->close()) {
        *error = output ? output->geterror() : OIIO::geterror();

        return false;
    }

    return true;
}

float
sourceValue(const std::string& name)
{
    for (std::size_t i = 0; i < sizeof(kSourceChannels) / sizeof(kSourceChannels[0]); ++i) {
        if (name == kSourceChannels[i].name) {
            return kSourceChannels[i].value;
        }
    }
    ADD_FAILURE() << "no source channel " << name;

    return -1.f;
}

// The file as the native Read sees it: each layer with its channels, and the value of every
// channel written as "layer.channel", or bare for the colour layer.
struct ReadBackLayers {
    std::map<std::string, std::vector<std::string>> channels;
    std::map<std::string, float> values;
};

std::set<std::string>
fileChannelNames(const std::string& path)
{
    std::set<std::string> names;
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);

    if (input) {
        const OIIO::ImageSpec& spec = input->spec();
        names.insert(spec.channelnames.begin(), spec.channelnames.end());
        input->close();
    }

    return names;
}

std::set<std::string>
names(std::initializer_list<const char*> list)
{
    std::set<std::string> ret;

    for (std::initializer_list<const char*>::const_iterator it = list.begin(); it != list.end(); ++it) {
        ret.insert(*it);
    }

    return ret;
}
} // namespace

class NativeWriteLayersTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "nativeWriteLayers", 1.));

        ASSERT_TRUE(_dir.isValid());
        _source = pathOf("source.exr");
        std::string error;
        ASSERT_TRUE(writeSource(_source, &error)) << error;

        NodePtr reader = createRead(_source);
        ASSERT_TRUE(bool(reader));
        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(bool(_writer));
        // The values are compared with the constants that went in, so the file is written without conversion.
        KnobStringBase* outputSpace = dynamic_cast<KnobStringBase*>(_writer->getKnobByName("ocioOutputSpace").get());
        KnobBool* outputSpaceSet = dynamic_cast<KnobBool*>(_writer->getKnobByName("ocioOutputSpaceSet").get());
        ASSERT_TRUE(outputSpace != NULL);
        ASSERT_TRUE(outputSpaceSet != NULL);
        outputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        outputSpaceSet->setValue(true);
        connectNodes(reader, _writer, 0, true);
        _reader = reader;

        _channels = std::dynamic_pointer_cast<KnobChannelSet>(_writer->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(_channels));
        setChoice("bitDepth", "32f");
        setChoice("compression", "none");
    }

    virtual void TearDown() OVERRIDE
    {
        _writer.reset();
        _reader.reset();
        _channels.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    NodePtr createRead(const std::string& path)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            file->setValue(path);
        }
        // The values are compared with the constants that went in, so the file is read without conversion.
        KnobStringBase* inputSpace = dynamic_cast<KnobStringBase*>(node->getKnobByName("ocioInputSpace").get());
        EXPECT_TRUE(inputSpace != NULL);
        if (inputSpace) {
            inputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        }
        node->getEffectInstance()->dropCachedMetadata();

        return node;
    }

    void setChoice(const char* name,
                   const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(_writer->getKnobByName(name).get());

        ASSERT_TRUE(choice != NULL) << name;
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return;
            }
        }
        ADD_FAILURE() << name << " has no option " << id;
    }

    void renderTo(const std::string& path)
    {
        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(_writer->getKnobByName(kOfxImageEffectFileParamName).get());

        ASSERT_TRUE(file != NULL);
        file->setValue(path);
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }

    void readLayers(const std::string& path,
                    ReadBackLayers* out)
    {
        NodePtr read = createRead(path);

        ASSERT_TRUE(bool(read));
        std::list<ImageLayerDesc> present;
        read->getEffectInstance()->getPresentLayers(1., ViewIdx(0), -1, &present);
        for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
            std::list<ImageLayerDesc> request(1, *it);
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesDirect(read, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), request, &planes, &error)) << it->getLayerID() << ": " << error;
            ASSERT_EQ(1u, planes.size());
            const RenderedPlane& plane = planes.front();
            const std::string prefix = it->isColorLayer() ? std::string() : it->getLayerID() + ".";
            out->channels[it->isColorLayer() ? std::string(kNatronColorLayerID) : it->getLayerID()] = it->getChannels();
            ASSERT_EQ((std::size_t)kSize * kSize * plane.channels.size(), plane.pixels.size());
            for (std::size_t c = 0; c < plane.channels.size(); ++c) {
                out->values[prefix + plane.channels[c]] = plane.pixels[c];
            }
        }
    }

    // Checks that `path` holds exactly `expected` as file channels, that the Read regroups them
    // into the layers of `expectedLayers`, and that every channel kept its own value.
    void expectFile(const std::string& path,
                    const std::set<std::string>& expected,
                    const std::map<std::string, std::vector<std::string>>& expectedLayers)
    {
        SCOPED_TRACE(path);
        EXPECT_EQ(expected, fileChannelNames(path));

        ReadBackLayers back;
        readLayers(path, &back);
        for (std::map<std::string, std::vector<std::string>>::const_iterator it = expectedLayers.begin(); it != expectedLayers.end(); ++it) {
            std::map<std::string, std::vector<std::string>>::const_iterator found = back.channels.find(it->first);
            ASSERT_TRUE(found != back.channels.end()) << "layer " << it->first << " did not read back";
            EXPECT_EQ(it->second, found->second) << it->first;
        }
        for (std::set<std::string>::const_iterator it = expected.begin(); it != expected.end(); ++it) {
            std::map<std::string, float>::const_iterator value = back.values.find(*it);
            ASSERT_TRUE(value != back.values.end()) << *it;
            EXPECT_NEAR(sourceValue(*it), value->second, 1e-6f) << *it;
        }
    }

    NodePtr _writer;
    NodePtr _reader;
    KnobChannelSetPtr _channels;
    std::string _source;
    QTemporaryDir _dir;
};

TEST_F(NativeWriteLayersTest, AllLayersAreWrittenAsLayerDotChannel)
{
    _channels->setAll();
    const std::string path = pathOf("all.exr");

    renderTo(path);
    std::map<std::string, std::vector<std::string>> layers;
    layers[kNatronColorLayerID] = { "R", "G", "B", "A" };
    layers["diffuse"] = { "R", "G", "B" };
    layers["specular"] = { "R", "G", "B" };
    layers["mask"] = { "A" };
    expectFile(path, names({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "specular.R", "specular.G", "specular.B", "mask.A" }), layers);
}

TEST_F(NativeWriteLayersTest, ColorAndOneLayer)
{
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);
    _channels->addLayer("diffuse", NULL);
    const std::string path = pathOf("color_diffuse.exr");

    renderTo(path);
    std::map<std::string, std::vector<std::string>> layers;
    layers[kNatronColorLayerID] = { "R", "G", "B", "A" };
    layers["diffuse"] = { "R", "G", "B" };
    expectFile(path, names({ "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B" }), layers);

    ReadBackLayers back;
    readLayers(path, &back);
    EXPECT_EQ(0u, back.channels.count("specular"));
    EXPECT_EQ(0u, back.channels.count("mask"));
}

TEST_F(NativeWriteLayersTest, ColorRgbSubsetWritesRgbOnly)
{
    const std::vector<std::string> rgb = { "R", "G", "B" };

    _channels->setLayer(0, kNatronColorViewRGBA, &rgb);
    const std::string path = pathOf("rgb.exr");

    renderTo(path);
    std::map<std::string, std::vector<std::string>> layers;
    layers[kNatronColorLayerID] = { "R", "G", "B" };
    expectFile(path, names({ "R", "G", "B" }), layers);
}

TEST_F(NativeWriteLayersTest, ColorAlphaAloneWritesAlphaOnly)
{
    const std::vector<std::string> alpha = { "A" };

    _channels->setLayer(0, kNatronColorViewRGBA, &alpha);
    const std::string path = pathOf("alpha.exr");

    renderTo(path);
    EXPECT_EQ(names({ "A" }), fileChannelNames(path));
    ReadBackLayers back;
    readLayers(path, &back);
    ASSERT_EQ(1u, back.values.count("A"));
    EXPECT_NEAR(sourceValue("A"), back.values["A"], 1e-6f);
}

TEST_F(NativeWriteLayersTest, SingleChannelsOfLayersKeepTheirOwnNames)
{
    const std::vector<std::string> green = { "G" };
    const std::vector<std::string> redBlue = { "R", "B" };

    _channels->setLayer(0, kNatronColorViewRGBA, NULL);
    _channels->addLayer("diffuse", &green);
    _channels->addLayer("specular", &redBlue);
    const std::string path = pathOf("single_channels.exr");

    renderTo(path);
    std::map<std::string, std::vector<std::string>> layers;
    layers[kNatronColorLayerID] = { "R", "G", "B", "A" };
    layers["diffuse"] = { "G" };
    layers["specular"] = { "R", "B" };
    expectFile(path, names({ "R", "G", "B", "A", "diffuse.G", "specular.R", "specular.B" }), layers);
}

TEST_F(NativeWriteLayersTest, LayerAloneWritesNoColor)
{
    _channels->setLayer(0, "specular", NULL);
    const std::string path = pathOf("specular_alone.exr");

    renderTo(path);
    std::map<std::string, std::vector<std::string>> layers;
    layers["specular"] = { "R", "G", "B" };
    expectFile(path, names({ "specular.R", "specular.G", "specular.B" }), layers);
}

TEST_F(NativeWriteLayersTest, OneChannelUserLayerReadsBackAsThatLayer)
{
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);
    _channels->addLayer("mask", NULL);
    const std::string path = pathOf("color_mask.exr");

    renderTo(path);
    std::map<std::string, std::vector<std::string>> layers;
    layers[kNatronColorLayerID] = { "R", "G", "B", "A" };
    layers["mask"] = { "A" };
    expectFile(path, names({ "R", "G", "B", "A", "mask.A" }), layers);

    _channels->removeRow(1);
    _channels->setLayer(0, "mask", NULL);
    const std::string alone = pathOf("mask_alone.exr");
    renderTo(alone);
    std::map<std::string, std::vector<std::string>> maskOnly;
    maskOnly["mask"] = { "A" };
    expectFile(alone, names({ "mask.A" }), maskOnly);
}

TEST_F(NativeWriteLayersTest, FormatWithoutExtraChannelsWritesColorAndWarns)
{
    ASSERT_FALSE(OiioWriteSupport::capabilities("png").nChannels);
    _channels->setLayer(0, kNatronColorViewRGBA, NULL);
    _channels->addLayer("diffuse", NULL);
    const std::string path = pathOf("limited.png");

    renderTo(path);
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(input)) << OIIO::geterror();
    EXPECT_EQ(4, input->spec().nchannels);
    input->close();

    ASSERT_TRUE(_writer->hasPersistentMessage());
    QString message;
    int type = 0;
    _writer->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("diffuse"))) << message.toStdString();

    std::vector<ChannelSetRow> rows = _channels->getRows();
    rows.resize(1);
    _channels->setRows(rows);
    renderTo(pathOf("color_only.png"));
    EXPECT_FALSE(_writer->hasPersistentMessage());
}
