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
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/Metadata/ModifyMetadata.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 16;
const int kHeight = 12;
const float kFirst = 0.25f;
const float kSecond = 0.75f;

// A flat float EXR: `value` in every channel of `names` but alpha, which is 1.
bool
writeFlatExr(const std::string& path,
             int width,
             int height,
             const std::vector<std::string>& names,
             float value)
{
    const int n = (int)names.size();
    OIIO::ImageSpec spec(width, height, n, OIIO::TypeDesc::FLOAT);

    spec.channelnames = names;
    spec.alpha_channel = 3;
    std::vector<float> pixels((std::size_t)width * height * n, value);
    for (std::size_t i = 3; i < pixels.size(); i += (std::size_t)n) {
        pixels[i] = 1.f;
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

const std::vector<std::string> kRgba = { "R", "G", "B", "A" };
} // namespace

class NativeReadReloadTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
        ASSERT_TRUE(_dir.isValid());
        _path = (_dir.path() + QString::fromUtf8("/reload.exr")).toStdString();
    }

    virtual void TearDown() OVERRIDE
    {
        _read.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    void pressReload()
    {
        KnobButton* reload = dynamic_cast<KnobButton*>(_read->getKnobByName(kNatronReadParamReload).get());

        ASSERT_TRUE(reload != NULL);
        reload->trigger();
    }

    std::set<std::string> producedLayerIDs()
    {
        EffectInstance::ComponentsNeededMap comps;
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime = 0.;
        int passThroughView = 0;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInputNb = -1;

        _read->getEffectInstance()->getComponentsNeededAndProduced_public(_read->getHashValue(), 1., ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);
        std::set<std::string> ids;
        const std::list<ImageLayerDesc>& produced = comps[-1];
        for (std::list<ImageLayerDesc>::const_iterator it = produced.begin(); it != produced.end(); ++it) {
            ids.insert(it->isColorLayer() ? std::string(kNatronColorLayerID) : it->getLayerID());
        }

        return ids;
    }

    RectD regionOfDefinition()
    {
        RectD rod;
        bool isProjectFormat = false;

        EXPECT_EQ(eStatusOK, _read->getEffectInstance()->getRegionOfDefinition_public(_read->getHashValue(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));

        return rod;
    }

    bool outputIsFlat(float value)
    {
        std::vector<RenderedPlane> planes;
        std::string error;

        if (!renderNodePlanesDirect(_read, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error) || (planes.size() != 1)) {
            ADD_FAILURE() << error;

            return false;
        }
        for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
            const float want = (i % 4 == 3) ? 1.f : value;
            if (planes[0].pixels[i] != want) {
                return false;
            }
        }

        return true;
    }

    QTemporaryDir _dir;
    std::string _path;
    NodePtr _read;
};

TEST_F(NativeReadReloadTest, TheReloadButtonSitsBesideTheFile)
{
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kFirst));
    _read = createWorkingSpaceRead(_path);
    ASSERT_TRUE(bool(_read));

    const KnobsVec& knobs = _read->getEffectInstance()->getKnobs();
    KnobIPtr file = _read->getKnobByName(kOfxImageEffectFileParamName);
    KnobIPtr reload = _read->getKnobByName(kNatronReadParamReload);
    ASSERT_TRUE(file && reload);
    EXPECT_TRUE(dynamic_cast<KnobButton*>(reload.get()) != NULL);
    EXPECT_EQ(std::string("Reload"), reload->getLabel());
    EXPECT_FALSE(file->isNewLineActivated());
    const KnobsVec::const_iterator fileAt = std::find(knobs.begin(), knobs.end(), file);
    ASSERT_TRUE(fileAt != knobs.end());
    ASSERT_TRUE(fileAt + 1 != knobs.end());
    EXPECT_EQ(reload, *(fileAt + 1));
}

TEST_F(NativeReadReloadTest, ThePanelHasOneReload)
{
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kFirst));
    _read = createWorkingSpaceRead(_path);
    ASSERT_TRUE(bool(_read));

    for (const char* name : { kOfxImageEffectFileParamName, kOfxImageEffectProxyParamName }) {
        KnobFile* file = dynamic_cast<KnobFile*>(_read->getKnobByName(name).get());
        ASSERT_TRUE(file != NULL) << name;
        EXPECT_FALSE(file->showsReloadButton()) << name << ": the Reload button reloads it";
    }

    int reloads = 0;
    const KnobsVec& knobs = _read->getEffectInstance()->getKnobs();
    for (const KnobIPtr& knob : knobs) {
        if (dynamic_cast<KnobButton*>(knob.get()) && ((knob->getName() == kNatronReadParamReload) || (knob->getLabel() == "Reload"))) {
            ++reloads;
        }
    }
    EXPECT_EQ(1, reloads);

    ASSERT_TRUE(outputIsFlat(kFirst));
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kSecond));
    pressReload();
    EXPECT_TRUE(outputIsFlat(kSecond));
}

TEST_F(NativeReadReloadTest, ReloadReadsTheLayersOfAFileChangedOnDisk)
{
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kFirst));
    _read = createWorkingSpaceRead(_path);
    ASSERT_TRUE(bool(_read));
    ASSERT_EQ(std::set<std::string>({ kNatronColorLayerID }), producedLayerIDs());

    std::vector<std::string> withDepth = kRgba;
    withDepth.push_back("depth.Z");
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, withDepth, kFirst));
    // The produced layers are cached under the node hash, which a file changed on disk leaves alone.
    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID }), producedLayerIDs());

    const U64 before = _read->getHashValue();
    pressReload();
    EXPECT_NE(before, _read->getHashValue());
    EXPECT_EQ(std::set<std::string>({ kNatronColorLayerID, "depth" }), producedLayerIDs());
}

TEST_F(NativeReadReloadTest, ReloadReadsTheSizeOfAFileChangedOnDisk)
{
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kFirst));
    _read = createWorkingSpaceRead(_path);
    ASSERT_TRUE(bool(_read));
    ASSERT_EQ(RectD(0., 0., kWidth, kHeight), regionOfDefinition());

    ASSERT_TRUE(writeFlatExr(_path, kWidth * 2, kHeight * 2, kRgba, kFirst));
    EXPECT_EQ(RectD(0., 0., kWidth, kHeight), regionOfDefinition());

    pressReload();
    EXPECT_EQ(RectD(0., 0., kWidth * 2, kHeight * 2), regionOfDefinition());
}

TEST_F(NativeReadReloadTest, ReloadOutputsThePixelsOfAFileChangedOnDisk)
{
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kFirst));
    _read = createWorkingSpaceRead(_path);
    ASSERT_TRUE(bool(_read));
    ASSERT_TRUE(outputIsFlat(kFirst));

    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kSecond));
    pressReload();

    EXPECT_TRUE(outputIsFlat(kSecond));
    EXPECT_FALSE(_read->hasPersistentMessage());
}

TEST_F(NativeReadReloadTest, FilenameEditsKeepTheReadersHashesRecoverable)
{
    const std::string other = (_dir.path() + QString::fromUtf8("/other.exr")).toStdString();
    ASSERT_TRUE(writeFlatExr(_path, kWidth, kHeight, kRgba, kFirst));
    ASSERT_TRUE(writeFlatExr(other, kWidth, kHeight, kRgba, kSecond));
    _read = createWorkingSpaceRead(_path);
    NodePtr modify = createNode(QString::fromUtf8(PLUGINID_NATRON_MODIFYMETADATA));
    ASSERT_TRUE(_read && modify);
    KnobFile* file = dynamic_cast<KnobFile*>(_read->getKnobByName(kOfxImageEffectFileParamName).get());
    KnobInt* value = dynamic_cast<KnobInt*>(modify->getKnobByName("intValue1").get());
    ASSERT_TRUE(file && value);
    ASSERT_NO_THROW(value->setExpression(0, "len(" + _read->getScriptName() + ".metadataKeys(1))", false, true));

    const U64 before = modify->getHashValue();
    file->setValue(other);
    EXPECT_NE(before, modify->getHashValue());
    file->setValue(_path);

    // A filename edit moves the Read's hash, so it needs no metadata revision, which would keep
    // the reader from finding the renders of the state it returned to.
    EXPECT_EQ(0u, _read->getMetadataRevision());
    EXPECT_EQ(before, modify->getHashValue());
}
