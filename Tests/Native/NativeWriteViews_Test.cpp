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
#include <cstddef>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <SequenceParsing.h>

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
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kSize = 8;
const double kTime = 1.;

// The layers of the source in file order; a channel's value depends on the view and on its place here.
const char* const kBaseChannels[] = { "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B" };
const int kColourChannels = 4;
const int kChannelCount = 7;

float
valueOf(int view,
        int channel)
{
    return channel == 3 ? 1.f : (float)(channel + 1) / 32.f + 0.5f * (float)view;
}

// The view goes in the component before the channel, after any layer.
std::string
inView(const std::string& channel,
       const std::string& view)
{
    const std::size_t dot = channel.rfind('.');

    return dot == std::string::npos ? view + "." + channel : channel.substr(0, dot + 1) + view + "." + channel.substr(dot + 1);
}

struct Channel {
    std::string name;
    float value;
};

// The channels of `view` in the colour layer, the diffuse layer or both, named as a part holding
// several views names them when `prefixView` is set.
std::vector<Channel>
channelsOf(int view,
           const std::string& viewName,
           bool colour,
           bool diffuse,
           bool prefixView)
{
    std::vector<Channel> channels;

    for (int c = 0; c < kChannelCount; ++c) {
        if ((c < kColourChannels && !colour) || (c >= kColourChannels && !diffuse)) {
            continue;
        }
        Channel channel;
        channel.name = (prefixView && view > 0) ? inView(kBaseChannels[c], viewName) : std::string(kBaseChannels[c]);
        channel.value = valueOf(view, c);
        channels.push_back(channel);
    }

    return channels;
}

struct FilePart {
    OIIO::ImageSpec spec;
    std::vector<float> pixels;
};

bool
readParts(const std::string& path,
          std::vector<FilePart>* parts,
          std::string* error)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);

    if (!input) {
        *error = OIIO::geterror();

        return false;
    }
    parts->clear();
    for (int p = 0; input->seek_subimage(p, 0); ++p) {
        FilePart part;
        part.spec = input->spec();
        part.pixels.assign((std::size_t)part.spec.width * (std::size_t)part.spec.height * (std::size_t)part.spec.nchannels, 0.f);
        if (!input->read_image(p, 0, 0, part.spec.nchannels, OIIO::TypeDesc::FLOAT, part.pixels.data())) {
            *error = input->geterror();

            return false;
        }
        parts->push_back(part);
    }
    input->close();

    return true;
}

std::vector<std::string>
stringsOf(const OIIO::ImageSpec& spec,
          const char* name)
{
    std::vector<std::string> result;
    const OIIO::ParamValue* value = spec.find_attribute(name);

    if (!value || !value->data() || value->type().basetype != OIIO::TypeDesc::STRING) {
        return result;
    }
    const std::size_t count = value->type().numelements() * (std::size_t)std::max(1, value->nvalues());
    const char* const* text = (const char* const*)value->data();
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(text[i] ? text[i] : "");
    }

    return result;
}

void
expectPart(const FilePart& part,
           const std::vector<Channel>& expected)
{
    ASSERT_EQ(expected.size(), part.spec.channelnames.size());
    ASSERT_EQ((int)expected.size(), part.spec.nchannels);
    // OpenEXR stores channels sorted by name, so the file's order is not the order they were written in.
    for (std::size_t e = 0; e < expected.size(); ++e) {
        const std::vector<std::string>& names = part.spec.channelnames;
        ASSERT_EQ(1, std::count(names.begin(), names.end(), expected[e].name)) << expected[e].name;
        const std::size_t c = (std::size_t)(std::find(names.begin(), names.end(), expected[e].name) - names.begin());
        for (int p = 0; p < part.spec.width * part.spec.height; ++p) {
            ASSERT_EQ(expected[e].value, part.pixels[(std::size_t)p * expected.size() + c]) << expected[e].name << " pixel " << p;
        }
    }
}

std::size_t
countFiles(const std::string& directory)
{
    std::size_t count = 0;
    std::error_code ec;

    for (std::filesystem::directory_iterator it(directory, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++count;
    }

    return count;
}
} // namespace

class NativeWriteViewsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "nativeWriteViews", 1.));
        project->createProjectViews(std::vector<std::string>(1, "Right"));
        _views = project->getProjectViewNames();
        ASSERT_EQ(2u, _views.size());
        ASSERT_TRUE(_dir.isValid());

        _source = pathOf("source.exr");
        std::string error;
        ASSERT_TRUE(writeSource(_source, &error)) << error;

        _reader = createRead(_source);
        ASSERT_TRUE(bool(_reader));
        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(bool(_writer));
        // The values are compared with the constants that went in, so the file is written without conversion.
        KnobStringBase* outputSpace = dynamic_cast<KnobStringBase*>(_writer->getKnobByName("ocioOutputSpace").get());
        KnobBool* outputSpaceSet = dynamic_cast<KnobBool*>(_writer->getKnobByName("ocioOutputSpaceSet").get());
        ASSERT_TRUE(outputSpace != NULL);
        ASSERT_TRUE(outputSpaceSet != NULL);
        outputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        outputSpaceSet->setValue(true);
        connectNodes(_reader, _writer, 0, true);
        // Every view is checked on its colour and diffuse layers together, so the Write takes
        // every layer rather than only the colour its channel set selects by default.
        KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(_writer->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(channels));
        channels->setAll();
        setChoice(_writer, kNatronWriteParamBitDepth, "32f");
        setChoice(_writer, kNatronWriteParamCompression, "none");
    }

    virtual void TearDown() OVERRIDE
    {
        _writer.reset();
        _reader.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    // One part holding the colour and diffuse layers of both views, the second view's channels named after it.
    bool writeSource(const std::string& path,
                     std::string* error)
    {
        std::vector<Channel> channels = channelsOf(0, _views[0], true, true, true);
        const std::vector<Channel> second = channelsOf(1, _views[1], true, true, true);

        channels.insert(channels.end(), second.begin(), second.end());
        OIIO::ImageSpec spec(kSize, kSize, (int)channels.size(), OIIO::TypeDesc::FLOAT);
        spec.channelnames.clear();
        for (std::size_t c = 0; c < channels.size(); ++c) {
            spec.channelnames.push_back(channels[c].name);
        }
        spec.alpha_channel = 3;
        spec.attribute("compression", "none");
        const char* const names[] = { _views[0].c_str(), _views[1].c_str() };
        spec.attribute("multiView", OIIO::TypeDesc(OIIO::TypeDesc::STRING, 2), names);
        std::vector<float> pixels((std::size_t)kSize * kSize * channels.size());
        for (std::size_t p = 0; p < (std::size_t)kSize * kSize; ++p) {
            for (std::size_t c = 0; c < channels.size(); ++c) {
                pixels[p * channels.size() + c] = channels[c].value;
            }
        }
        std::unique_ptr<OIIO::ImageOutput> output = OIIO::ImageOutput::create(path);
        if (!output || !output->open(path, spec) || !output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !output->close()) {
            *error = output ? output->geterror() : OIIO::geterror();

            return false;
        }

        return true;
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
        node->getEffectInstance()->refreshMetadata_public(false);

        return node;
    }

    bool setChoice(const NodePtr& node,
                   const char* name,
                   const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(node->getKnobByName(name).get());

        EXPECT_TRUE(choice != NULL) << name;
        if (!choice) {
            return false;
        }
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return true;
            }
        }
        ADD_FAILURE() << name << " has no option " << id;

        return false;
    }

    void setFile(const std::string& path)
    {
        KnobOutputFile* file = dynamic_cast<KnobOutputFile*>(_writer->getKnobByName(kOfxImageEffectFileParamName).get());

        ASSERT_TRUE(file != NULL);
        file->setValue(path);
    }

    bool isShown(const char* name)
    {
        KnobIPtr k = _writer->getKnobByName(name);

        EXPECT_TRUE(bool(k)) << name;

        return k && !k->getIsSecret();
    }

    void renderFrame()
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);
    }

    // Reads `path` through the native Read and checks that each view's colour and diffuse planes hold that view's pixels.
    void expectViewsReadBack(const std::string& path)
    {
        NodePtr read = createRead(path);

        ASSERT_TRUE(bool(read));
        for (int v = 0; v < 2; ++v) {
            SCOPED_TRACE(_views[(std::size_t)v]);
            std::list<ImageLayerDesc> present;
            read->getEffectInstance()->getPresentLayers(kTime, ViewIdx(v), -1, &present);
            for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
                const bool colour = it->isColorLayer();
                if (!colour && it->getLayerID() != "diffuse") {
                    continue;
                }
                std::vector<RenderedPlane> planes;
                std::string error;
                ASSERT_TRUE(renderNodePlanesDirect(read, kTime, ViewIdx(v), 0, RectI(0, 0, kSize, kSize), std::list<ImageLayerDesc>(1, *it), &planes, &error)) << it->getLayerID() << ": " << error;
                ASSERT_EQ(1u, planes.size());
                const std::size_t nComps = planes[0].channels.size();
                ASSERT_EQ(colour ? 4u : 3u, nComps);
                ASSERT_EQ((std::size_t)kSize * kSize * nComps, planes[0].pixels.size());
                for (std::size_t p = 0; p < (std::size_t)kSize * kSize; ++p) {
                    for (std::size_t c = 0; c < nComps; ++c) {
                        ASSERT_EQ(valueOf(v, (int)c + (colour ? 0 : kColourChannels)), planes[0].pixels[p * nComps + c]) << it->getLayerID() << " channel " << c << " pixel " << p;
                    }
                }
            }
        }
    }

    std::string _source;
    std::vector<std::string> _views;
    NodePtr _reader;
    NodePtr _writer;
    QTemporaryDir _dir;
};

TEST_F(NativeWriteViewsTest, TheViewsMenuListsAllThenEveryProjectView)
{
    KnobChoice* views = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kNatronWriteParamViewsSelector).get());

    ASSERT_TRUE(views != NULL);
    const std::vector<ChoiceOption> entries = views->getEntries_mt_safe();
    ASSERT_EQ(3u, entries.size());
    EXPECT_EQ(std::string(kNatronWriteViewsAll), entries[0].id);
    EXPECT_EQ(_views[0], entries[1].id);
    EXPECT_EQ(_views[1], entries[2].id);
    EXPECT_EQ(0, views->getValue());

    KnobChoice* parts = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kNatronWriteParamPartSplitting).get());
    ASSERT_TRUE(parts != NULL);
    const std::vector<ChoiceOption> splits = parts->getEntries_mt_safe();
    ASSERT_EQ(3u, splits.size());
    EXPECT_EQ(std::string(kNatronWritePartSplittingSingle), splits[0].id);
    EXPECT_EQ(std::string(kNatronWritePartSplittingViews), splits[1].id);
    EXPECT_EQ(std::string(kNatronWritePartSplittingViewsLayers), splits[2].id);
}

namespace {
void
setProjectViews(const ProjectPtr& project, const std::list<std::string>& names)
{
    KnobPath* list = dynamic_cast<KnobPath*>(project->getKnobByName("viewsList").get());

    ASSERT_TRUE(list != NULL);
    list->setValue(list->encodeToKnobTableFormatSingleCol(names));
}

std::vector<std::string>
menuOf(const NodePtr& node)
{
    KnobChoice* views = dynamic_cast<KnobChoice*>(node->getKnobByName(kNatronWriteParamViewsSelector).get());
    std::vector<std::string> ids;

    if (views) {
        for (const ChoiceOption& option : views->getEntries_mt_safe()) {
            ids.push_back(option.id);
        }
    }

    return ids;
}

std::string
chosenView(const NodePtr& node)
{
    KnobChoice* views = dynamic_cast<KnobChoice*>(node->getKnobByName(kNatronWriteParamViewsSelector).get());

    return views ? views->getActiveEntry().id : std::string();
}
} // namespace

TEST_F(NativeWriteViewsTest, AddedViewsAppearInTheMenu)
{
    setProjectViews(getApp()->getProject(), { _views[0], _views[1], "Extra" });

    EXPECT_EQ((std::vector<std::string> { kNatronWriteViewsAll, _views[0], _views[1], "Extra" }), menuOf(_writer));
    EXPECT_EQ(std::string(kNatronWriteViewsAll), chosenView(_writer));
}

TEST_F(NativeWriteViewsTest, RenamingTheChosenViewFallsBackToAll)
{
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamViewsSelector, _views[1]));
    setProjectViews(getApp()->getProject(), { _views[0], "Renamed" });

    EXPECT_EQ((std::vector<std::string> { kNatronWriteViewsAll, _views[0], "Renamed" }), menuOf(_writer));
    EXPECT_EQ(std::string(kNatronWriteViewsAll), chosenView(_writer));
}

TEST_F(NativeWriteViewsTest, RemovingAnotherViewKeepsTheChosenViewByName)
{
    setProjectViews(getApp()->getProject(), { _views[0], _views[1], "Extra" });
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamViewsSelector, _views[1]));
    setProjectViews(getApp()->getProject(), { _views[1], "Extra" });

    EXPECT_EQ((std::vector<std::string> { kNatronWriteViewsAll, _views[1], "Extra" }), menuOf(_writer));
    EXPECT_EQ(_views[1], chosenView(_writer));
    KnobChoice* views = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kNatronWriteParamViewsSelector).get());
    ASSERT_TRUE(views != NULL);
    EXPECT_EQ(1, views->getValue());
}

TEST_F(NativeWriteViewsTest, RemovingTheChosenViewFallsBackToAll)
{
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamViewsSelector, _views[1]));
    setProjectViews(getApp()->getProject(), { _views[0] });

    EXPECT_EQ((std::vector<std::string> { kNatronWriteViewsAll, _views[0] }), menuOf(_writer));
    EXPECT_EQ(std::string(kNatronWriteViewsAll), chosenView(_writer));
}

TEST_F(NativeWriteViewsTest, AChosenViewSurvivesSaveAndLoad)
{
    setProjectViews(getApp()->getProject(), { _views[0], _views[1], "Extra" });
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamViewsSelector, "Extra"));
    const std::string name = _writer->getScriptName();

    ProjectPtr project = getApp()->getProject();
    const QString dirPath = _dir.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("views.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ((std::vector<std::string> { kNatronWriteViewsAll, _views[0], _views[1], "Extra" }), menuOf(loaded));
    EXPECT_EQ(std::string("Extra"), chosenView(loaded));
}

TEST_F(NativeWriteViewsTest, TheViewKnobsFollowTheFormatAndTheViewPattern)
{
    setFile(pathOf("shown.exr"));
    EXPECT_TRUE(isShown(kNatronWriteParamPartSplitting));
    EXPECT_TRUE(isShown(kNatronWriteParamViewsSelector));

    setFile(pathOf("shown.png"));
    EXPECT_FALSE(isShown(kNatronWriteParamPartSplitting));
    EXPECT_FALSE(isShown(kNatronWriteParamViewsSelector));

    setFile(pathOf("shown_%V.exr"));
    EXPECT_TRUE(isShown(kNatronWriteParamPartSplitting));
    EXPECT_FALSE(isShown(kNatronWriteParamViewsSelector));

    setFile(pathOf("shown.exr"));
    EXPECT_TRUE(isShown(kNatronWriteParamViewsSelector));
}

TEST_F(NativeWriteViewsTest, SinglePartHoldsEveryViewAndReadsBackPerView)
{
    const std::string path = pathOf("single.exr");

    setFile(path);
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamPartSplitting, kNatronWritePartSplittingSingle));
    renderFrame();

    std::vector<FilePart> parts;
    std::string error;
    ASSERT_TRUE(readParts(path, &parts, &error)) << error;
    ASSERT_EQ(1u, parts.size());
    EXPECT_EQ(_views, stringsOf(parts[0].spec, "multiView"));
    std::vector<Channel> expected = channelsOf(0, _views[0], true, true, true);
    const std::vector<Channel> second = channelsOf(1, _views[1], true, true, true);
    expected.insert(expected.end(), second.begin(), second.end());
    expectPart(parts[0], expected);
    const std::vector<std::string>& names = parts[0].spec.channelnames;
    EXPECT_EQ((int)(std::find(names.begin(), names.end(), "A") - names.begin()), parts[0].spec.alpha_channel);

    expectViewsReadBack(path);
    EXPECT_EQ(2u, countFiles(_dir.path().toStdString())) << "the source and one file";
}

TEST_F(NativeWriteViewsTest, SplitViewsHoldsOnePartPerView)
{
    const std::string path = pathOf("views.exr");

    setFile(path);
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamPartSplitting, kNatronWritePartSplittingViews));
    renderFrame();

    std::vector<FilePart> parts;
    std::string error;
    ASSERT_TRUE(readParts(path, &parts, &error)) << error;
    ASSERT_EQ(2u, parts.size());
    for (int v = 0; v < 2; ++v) {
        SCOPED_TRACE(_views[(std::size_t)v]);
        EXPECT_EQ(std::vector<std::string>(1, _views[(std::size_t)v]), stringsOf(parts[(std::size_t)v].spec, "view"));
        EXPECT_TRUE(stringsOf(parts[(std::size_t)v].spec, "multiView").empty());
        expectPart(parts[(std::size_t)v], channelsOf(v, _views[(std::size_t)v], true, true, false));
    }
    expectViewsReadBack(path);
}

TEST_F(NativeWriteViewsTest, SplitViewsAndLayersHoldsOnePartPerLayerPerView)
{
    const std::string path = pathOf("layers.exr");

    setFile(path);
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamPartSplitting, kNatronWritePartSplittingViewsLayers));
    renderFrame();

    std::vector<FilePart> parts;
    std::string error;
    ASSERT_TRUE(readParts(path, &parts, &error)) << error;
    ASSERT_EQ(4u, parts.size());
    std::size_t index = 0;
    for (int v = 0; v < 2; ++v) {
        const std::string& view = _views[(std::size_t)v];
        SCOPED_TRACE(view);
        EXPECT_EQ(std::vector<std::string>(1, view), stringsOf(parts[index].spec, "view"));
        EXPECT_EQ(std::string("Color.") + view, parts[index].spec.get_string_attribute("oiio:subimagename"));
        expectPart(parts[index], channelsOf(v, view, true, false, false));
        ++index;
        EXPECT_EQ(std::vector<std::string>(1, view), stringsOf(parts[index].spec, "view"));
        EXPECT_EQ(std::string("diffuse.") + view, parts[index].spec.get_string_attribute("oiio:subimagename"));
        expectPart(parts[index], channelsOf(v, view, false, true, false));
        ++index;
    }
    expectViewsReadBack(path);
}

TEST_F(NativeWriteViewsTest, AViewPatternWritesOneFilePerView)
{
    const std::string pattern = pathOf("pattern_%V.exr");

    setFile(pattern);
    renderFrame();

    EXPECT_EQ(3u, countFiles(_dir.path().toStdString())) << "the source and one file per view";
    for (int v = 0; v < 2; ++v) {
        SCOPED_TRACE(_views[(std::size_t)v]);
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, _views, 1, v);
        std::vector<FilePart> parts;
        std::string error;
        ASSERT_TRUE(readParts(path, &parts, &error)) << error;
        ASSERT_EQ(1u, parts.size());
        EXPECT_TRUE(stringsOf(parts[0].spec, "multiView").empty());
        expectPart(parts[0], channelsOf(v, _views[(std::size_t)v], true, true, false));
    }
}

TEST_F(NativeWriteViewsTest, ChoosingOneViewWritesThatViewAlone)
{
    const std::string path = pathOf("right.exr");

    setFile(path);
    ASSERT_TRUE(setChoice(_writer, kNatronWriteParamViewsSelector, _views[1]));
    renderFrame();

    std::vector<FilePart> parts;
    std::string error;
    ASSERT_TRUE(readParts(path, &parts, &error)) << error;
    ASSERT_EQ(1u, parts.size());
    EXPECT_TRUE(stringsOf(parts[0].spec, "multiView").empty());
    expectPart(parts[0], channelsOf(1, _views[1], true, true, false));
}

TEST_F(NativeWriteViewsTest, TaskGraphRenderOfAllViewsHasNoUnplannedPulls)
{
    setFile(pathOf("planned.exr"));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    std::vector<int> unplannedPulls;
    const RenderMismatch m = renderBothWaysDirect(_writer, kTime, ViewIdx(0), 0, RectI(0, 0, kSize, kSize), poolSizes, std::function<void()>(), 0.f, &unplannedPulls);

    EXPECT_FALSE(m.any) << describe(m);
    for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
        EXPECT_EQ(0, unplannedPulls[p]) << "pool " << poolSizes[p];
    }
}
