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

#include <bitset>
#include <cstddef>
#include <list>
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
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"
#include "Global/StrUtils.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 8;
const int kHeight = 6;

// Left is view 0 of the files, right view 1. Every value is exact in float.
enum FileView {
    eLeft = 0,
    eRight = 1,
};

enum FileLayerKind {
    eColour = 0,
    eDiffuse = 1,
};

const char* const kViewNames[] = { "left", "right" };

int
seedOf(FileView view,
       FileLayerKind layer,
       int channel)
{
    return (int)view * 50 + (int)layer * 10 + channel;
}

float
valueAt(int seed,
        int x,
        int fileRow)
{
    return (float)((x * 37 + fileRow * 11 + seed * 13) % 97) / 64.f;
}

// One channel as the file names it, and the seed of the pixels written to it.
struct FileChannel {
    std::string name;
    int seed;
};

struct FilePart {
    std::string name;
    std::string view;
    std::vector<FileChannel> channels;
};

std::vector<FileChannel>
channelsOf(FileView view,
           FileLayerKind layer,
           const std::string& prefix,
           const std::string& infix)
{
    std::vector<FileChannel> channels;
    const char* const names[] = { "R", "G", "B", "A" };
    const int count = layer == eColour ? 4 : 3;

    for (int c = 0; c < count; ++c) {
        FileChannel channel;
        channel.name = prefix + infix + names[c];
        channel.seed = seedOf(view, layer, c);
        channels.push_back(channel);
    }

    return channels;
}

std::string
pathIn(const QTemporaryDir& dir,
       const char* name)
{
    return (dir.path() + QString::fromUtf8("/") + QString::fromUtf8(name)).toStdString();
}

OIIO::ImageSpec
specOf(const FilePart& part)
{
    OIIO::ImageSpec spec(kWidth, kHeight, (int)part.channels.size(), OIIO::TypeDesc::FLOAT);

    spec.channelnames.clear();
    for (std::size_t c = 0; c < part.channels.size(); ++c) {
        spec.channelnames.push_back(part.channels[c].name);
    }
    if (!part.name.empty()) {
        spec.attribute("oiio:subimagename", part.name);
    }
    if (!part.view.empty()) {
        spec.attribute("view", part.view);
    }

    return spec;
}

std::vector<float>
pixelsOf(const FilePart& part)
{
    const std::size_t nChannels = part.channels.size();
    std::vector<float> values((std::size_t)kWidth * kHeight * nChannels);

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (std::size_t c = 0; c < nChannels; ++c) {
                values[((std::size_t)y * kWidth + x) * nChannels + c] = valueAt(part.channels[c].seed, x, y);
            }
        }
    }

    return values;
}

bool
writeParts(const std::string& path,
           const std::vector<FilePart>& parts,
           const std::vector<std::string>& multiView)
{
    std::vector<OIIO::ImageSpec> specs;

    for (std::size_t p = 0; p < parts.size(); ++p) {
        specs.push_back(specOf(parts[p]));
    }
    if (!multiView.empty()) {
        std::vector<const char*> names;
        for (std::size_t i = 0; i < multiView.size(); ++i) {
            names.push_back(multiView[i].c_str());
        }
        specs[0].attribute("multiView", OIIO::TypeDesc(OIIO::TypeDesc::STRING, (int)names.size()), names.data());
    }

    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    if (!out) {
        ADD_FAILURE() << "No writer for " << path;

        return false;
    }
    if (!out->open(path, (int)specs.size(), specs.data())) {
        ADD_FAILURE() << out->geterror();

        return false;
    }
    for (std::size_t p = 0; p < specs.size(); ++p) {
        if (p > 0 && !out->open(path, specs[p], OIIO::ImageOutput::AppendSubimage)) {
            ADD_FAILURE() << out->geterror();

            return false;
        }
        const std::vector<float> pixels = pixelsOf(parts[p]);
        if (!out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) {
            ADD_FAILURE() << out->geterror();

            return false;
        }
    }
    EXPECT_TRUE(out->close()) << out->geterror();

    return true;
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
} // namespace

class NativeReadViewsTest
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

    // A temporary directory under the build tree, which ctest runs in.
    static QString temporaryTemplate()
    {
        return QDir::current().absoluteFilePath(QString::fromUtf8("NativeReadViews-XXXXXX"));
    }

    NodePtr createRead(const std::string& path)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));
        if (node) {
            KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
            EXPECT_TRUE(file != NULL);
            if (file) {
                file->setValue(path);
            }
            // These tests compare with the raw pixels, so the file is read without conversion.
            KnobStringBase* inputSpace = dynamic_cast<KnobStringBase*>(node->getKnobByName("ocioInputSpace").get());
            EXPECT_TRUE(inputSpace != NULL);
            if (inputSpace) {
                inputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
            }
            node->getEffectInstance()->refreshMetadata_public(false);
        }

        return node;
    }

    // The index of the project view named `name`, whatever its case, or -1.
    int projectViewIndex(const char* name)
    {
        const std::vector<std::string> views = getApp()->getProject()->getProjectViewNames();

        for (std::size_t i = 0; i < views.size(); ++i) {
            if (StrUtils::iequals(views[i], name)) {
                return (int)i;
            }
        }

        return -1;
    }

    std::list<ImageLayerDesc> producedLayers(const NodePtr& node,
                                             int view)
    {
        EffectInstance::ComponentsNeededMap comps;
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime = 0.;
        int passThroughView = 0;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInputNb = -1;

        node->getEffectInstance()->getComponentsNeededAndProduced_public(node->getHashValue(), 1., ViewIdx(view), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

        return comps[-1];
    }

    // Renders `layer` in project view `view` over the whole file and compares each channel with
    // the pixels written to the file view `fileView`.
    void expectLayer(const NodePtr& node,
                     int view,
                     const ImageLayerDesc& layer,
                     FileView fileView,
                     FileLayerKind kind,
                     std::vector<float>* firstChannel = NULL)
    {
        SCOPED_TRACE(layer.getLayerID());
        const RectI window(0, 0, kWidth, kHeight);
        std::vector<RenderedPlane> planes;
        std::string error;

        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(view), 0, window, std::list<ImageLayerDesc>(1, layer), &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        const std::size_t nComps = planes[0].channels.size();
        ASSERT_EQ(kind == eColour ? 4u : 3u, nComps);
        ASSERT_EQ((std::size_t)kWidth * kHeight * nComps, planes[0].pixels.size());

        std::size_t mismatches = 0;
        for (int row = 0; row < kHeight; ++row) {
            const int fileRow = kHeight - 1 - row;
            for (int col = 0; col < kWidth; ++col) {
                for (std::size_t c = 0; c < nComps; ++c) {
                    const float got = planes[0].pixels[((std::size_t)row * kWidth + col) * nComps + c];
                    const float want = valueAt(seedOf(fileView, kind, (int)c), col, fileRow);
                    if (got != want && ++mismatches <= 5) {
                        ADD_FAILURE() << "pixel " << col << "," << row << " channel " << c << ": got " << got << ", want " << want;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
        if (firstChannel) {
            firstChannel->clear();
            for (std::size_t i = 0; i < planes[0].pixels.size(); i += nComps) {
                firstChannel->push_back(planes[0].pixels[i]);
            }
        }
    }

    // Colour and the "diffuse" layer of project view `view` come from the file view `fileView`.
    void expectView(const NodePtr& node,
                    int view,
                    FileView fileView,
                    std::vector<float>* colourR = NULL)
    {
        SCOPED_TRACE(view);
        const std::list<ImageLayerDesc> produced = producedLayers(node, view);
        EXPECT_EQ(std::vector<std::string>(1, "diffuse"), layerIDs(produced));
        expectLayer(node, view, ImageLayerDesc::getRGBAComponents(), fileView, eColour, colourR);
        for (std::list<ImageLayerDesc>::const_iterator it = produced.begin(); it != produced.end(); ++it) {
            if (it->getLayerID() == "diffuse") {
                expectLayer(node, view, *it, fileView, eDiffuse);
            }
        }
    }

    void expectTwoViewsRead(const std::string& path)
    {
        NodePtr node = createRead(path);
        ASSERT_TRUE(bool(node));

        const int left = projectViewIndex("left");
        const int right = projectViewIndex("right");
        ASSERT_GE(left, 0) << "the project was not given the file's left view";
        ASSERT_GE(right, 0) << "the project was not given the file's right view";
        ASSERT_NE(left, right);

        std::vector<float> leftPixels;
        std::vector<float> rightPixels;
        expectView(node, left, eLeft, &leftPixels);
        expectView(node, right, eRight, &rightPixels);
        EXPECT_NE(leftPixels, rightPixels);
    }
};

TEST_F(NativeReadViewsTest, AMultiPartFileGivesTheProjectItsViewsAndEachViewItsOwnPart)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());

    std::vector<FilePart> parts;
    for (int v = 0; v < 2; ++v) {
        const FileView view = (FileView)v;
        const std::string viewName = kViewNames[v];
        FilePart colour;
        colour.name = "rgba_" + viewName;
        colour.view = viewName;
        colour.channels = channelsOf(view, eColour, "", "");
        parts.push_back(colour);
        FilePart diffuse;
        diffuse.name = "diffuse_" + viewName;
        diffuse.view = viewName;
        diffuse.channels = channelsOf(view, eDiffuse, "", "");
        parts.push_back(diffuse);
    }
    const std::string path = pathIn(dir, "multi-part-views.exr");
    ASSERT_TRUE(writeParts(path, parts, std::vector<std::string>()));

    expectTwoViewsRead(path);
}

TEST_F(NativeReadViewsTest, ASinglePartMultiViewFileReadsEachViewsChannels)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());

    // The default view is unprefixed and the others carry their name before the last component.
    FilePart part;
    part.channels = channelsOf(eLeft, eColour, "", "");
    const std::vector<FileChannel> leftDiffuse = channelsOf(eLeft, eDiffuse, "diffuse.", "");
    const std::vector<FileChannel> rightColour = channelsOf(eRight, eColour, "", "right.");
    const std::vector<FileChannel> rightDiffuse = channelsOf(eRight, eDiffuse, "diffuse.", "right.");
    part.channels.insert(part.channels.end(), leftDiffuse.begin(), leftDiffuse.end());
    part.channels.insert(part.channels.end(), rightColour.begin(), rightColour.end());
    part.channels.insert(part.channels.end(), rightDiffuse.begin(), rightDiffuse.end());
    const std::string path = pathIn(dir, "single-part-multiview.exr");
    std::vector<std::string> multiView;
    multiView.push_back("left");
    multiView.push_back("right");
    ASSERT_TRUE(writeParts(path, std::vector<FilePart>(1, part), multiView));

    expectTwoViewsRead(path);
}

TEST_F(NativeReadViewsTest, AFileOfOneViewReadsTheSameInEveryProjectView)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());

    FilePart colour;
    colour.name = "rgba";
    colour.channels = channelsOf(eLeft, eColour, "", "");
    FilePart diffuse;
    diffuse.name = "diffuse";
    diffuse.channels = channelsOf(eLeft, eDiffuse, "", "");
    std::vector<FilePart> parts;
    parts.push_back(colour);
    parts.push_back(diffuse);
    const std::string path = pathIn(dir, "one-view.exr");
    ASSERT_TRUE(writeParts(path, parts, std::vector<std::string>()));

    getApp()->getProject()->createProjectViews(std::vector<std::string>(1, "Right"));
    ASSERT_EQ(2, getApp()->getProject()->getProjectViewsCount());
    NodePtr node = createRead(path);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(2, getApp()->getProject()->getProjectViewsCount());

    for (int view = 0; view < 2; ++view) {
        expectView(node, view, eLeft);
    }
}

TEST_F(NativeReadViewsTest, AFileThatNamesOneViewAddsNoProjectView)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());

    FilePart colour;
    colour.name = "rgba_left";
    colour.view = "left";
    colour.channels = channelsOf(eLeft, eColour, "", "");
    FilePart diffuse;
    diffuse.name = "diffuse_left";
    diffuse.view = "left";
    diffuse.channels = channelsOf(eLeft, eDiffuse, "", "");
    std::vector<FilePart> parts;
    parts.push_back(colour);
    parts.push_back(diffuse);
    const std::string path = pathIn(dir, "named-one-view.exr");
    ASSERT_TRUE(writeParts(path, parts, std::vector<std::string>()));

    const int before = getApp()->getProject()->getProjectViewsCount();
    NodePtr node = createRead(path);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(before, getApp()->getProject()->getProjectViewsCount());
    EXPECT_EQ(-1, projectViewIndex("left"));
    expectView(node, 0, eLeft);
}

TEST_F(NativeReadViewsTest, AViewTheFileLacksReadsItsDefaultView)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());

    std::vector<FilePart> parts;
    for (int v = 0; v < 2; ++v) {
        FilePart colour;
        colour.name = std::string("rgba_") + kViewNames[v];
        colour.view = kViewNames[v];
        colour.channels = channelsOf((FileView)v, eColour, "", "");
        parts.push_back(colour);
    }
    const std::string path = pathIn(dir, "two-views-colour.exr");
    ASSERT_TRUE(writeParts(path, parts, std::vector<std::string>()));

    getApp()->getProject()->createProjectViews(std::vector<std::string>(1, "Centre"));
    NodePtr node = createRead(path);
    ASSERT_TRUE(bool(node));
    const int centre = projectViewIndex("centre");
    const int left = projectViewIndex("left");
    ASSERT_GE(centre, 0);
    ASSERT_GE(left, 0);

    std::vector<float> centrePixels;
    std::vector<float> leftPixels;
    expectLayer(node, centre, ImageLayerDesc::getRGBAComponents(), eLeft, eColour, &centrePixels);
    expectLayer(node, left, ImageLayerDesc::getRGBAComponents(), eLeft, eColour, &leftPixels);
    EXPECT_EQ(leftPixels, centrePixels);
}

TEST_F(NativeReadViewsTest, AReadCreatedWithItsFileGetsTheViewsAndTheColourDefault)
{
    QTemporaryDir dir(temporaryTemplate());
    ASSERT_TRUE(dir.isValid());

    std::vector<FilePart> parts;
    for (int v = 0; v < 2; ++v) {
        FilePart colour;
        colour.name = std::string("rgba_") + kViewNames[v];
        colour.view = kViewNames[v];
        colour.channels = channelsOf((FileView)v, eColour, "", "");
        parts.push_back(colour);
    }
    const std::string path = pathIn(dir, "created-with-file.exr");
    ASSERT_TRUE(writeParts(path, parts, std::vector<std::string>()));

    CreateNodeArgs args(PLUGINID_NATRON_READ, getApp()->getProject());
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_READ, 0);
    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, path);
    NodePtr node = getApp()->createNode(args);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(dynamic_cast<NativeRead*>(node->getEffectInstance().get()));

    KnobStringBasePtr inputSpace = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName("ocioInputSpace"));
    KnobBoolPtr inputSpaceSet = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName("ocioInputSpaceSet"));
    ASSERT_TRUE(inputSpace && inputSpaceSet);
    EXPECT_EQ(std::string("ACES2065-1"), inputSpace->getValue());
    EXPECT_FALSE(inputSpaceSet->getValue());

    // The per-view checks below compare with the raw pixels, so the file is read without conversion.
    inputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
    node->getEffectInstance()->refreshMetadata_public(false);

    const int left = projectViewIndex("left");
    const int right = projectViewIndex("right");
    ASSERT_GE(left, 0) << "the project was not given the file's left view";
    ASSERT_GE(right, 0) << "the project was not given the file's right view";
    expectLayer(node, left, ImageLayerDesc::getRGBAComponents(), eLeft, eColour);
    expectLayer(node, right, ImageLayerDesc::getRGBAComponents(), eRight, eColour);
}
