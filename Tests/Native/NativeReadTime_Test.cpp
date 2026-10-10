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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdio>
#include <list>
#include <memory>
#include <optional>
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
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kSize = 4;
const float kNotRendered = -1.f;

// Frame 3 is absent. Every value is exact in float, so it can be compared with EXPECT_EQ.
const int kFrames[] = { 1, 2, 4 };
const float kValues[] = { 0.125f, 0.25f, 0.5f };

bool
writeConstantFrame(const std::string& path,
                   float value,
                   int width = kSize,
                   int height = kSize)
{
    OIIO::ImageSpec spec(width, height, 4, OIIO::TypeDesc::FLOAT);
    std::vector<float> pixels((std::size_t)width * (std::size_t)height * 4, value);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

std::string
framePath(const QTemporaryDir& dir,
          const char* stem,
          int frame)
{
    char number[16];

    snprintf(number, sizeof(number), "%04d", frame);

    return (dir.path() + QString::fromUtf8("/%1.%2.exr").arg(QString::fromUtf8(stem), QString::fromUtf8(number))).toStdString();
}

std::string
patternPath(const QTemporaryDir& dir,
            const char* stem)
{
    return (dir.path() + QString::fromUtf8("/%1.####.exr").arg(QString::fromUtf8(stem))).toStdString();
}
} // namespace

class NativeReadTimeTest
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

    void writeSequence(const QTemporaryDir& dir,
                       const char* stem)
    {
        ASSERT_TRUE(dir.isValid());
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(writeConstantFrame(framePath(dir, stem, kFrames[i]), kValues[i]));
        }
    }

    NodePtr createRead(const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));
        if (node) {
            setFile(node, pattern);
        }

        return node;
    }

    void setFile(const NodePtr& node,
                 const std::string& pattern)
    {
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(pattern);
        // These tests compare with the raw pixels, so the file is read without conversion.
        KnobStringBase* inputSpace = dynamic_cast<KnobStringBase*>(node->getKnobByName("ocioInputSpace").get());
        ASSERT_TRUE(inputSpace != NULL);
        inputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        node->getEffectInstance()->refreshMetadata_public(false);
    }

    template <typename K>
    K* knob(const NodePtr& node,
            const char* name)
    {
        K* result = dynamic_cast<K*>(node->getKnobByName(name).get());
        EXPECT_TRUE(result != NULL) << name;

        return result;
    }

    void setChoice(const NodePtr& node,
                   const char* name,
                   int index)
    {
        knob<KnobChoice>(node, name)->setValue(index);
    }

    // What the GUI does when the user types into an int knob.
    void userSetInt(const NodePtr& node,
                    const char* name,
                    int value)
    {
        knob<KnobInt>(node, name)->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    void userSetChoice(const NodePtr& node,
                       const char* name,
                       int index)
    {
        knob<KnobChoice>(node, name)->setValue(index, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    // The same calls, in the same order, as the Gui's "Reset to default" on one knob.
    void restoreDefaultAsTheGuiDoes(const NodePtr& node,
                                    const char* name)
    {
        KnobIPtr target = node->getKnobByName(name);
        ASSERT_TRUE(bool(target)) << name;
        KnobHolder* holder = target->getHolder();
        ASSERT_TRUE(holder != NULL);

        holder->beginChanges();
        holder->beginChanges();
        for (int d = 0; d < target->getDimension(); ++d) {
            target->resetToDefaultValue(d);
        }
        holder->endChanges(true);
        holder->onKnobValueChanged_public(target.get(), eValueChangedReasonRestoreDefault, 0., ViewIdx(0), true);
        holder->endChanges();
        holder->incrHashAndEvaluate(true, true);
    }

    bool userEdited(const NodePtr& node)
    {
        return knob<KnobBool>(node, "timeDomainUserEdited")->getValue();
    }

    int intValue(const NodePtr& node,
                 const char* name,
                 int dimension = 0)
    {
        return knob<KnobInt>(node, name)->getValue(dimension);
    }

    // The value of the first pixel of the frame rendered at `time`, or kNotRendered on failure.
    float render(const NodePtr& node,
                 double time)
    {
        std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;

        if (!renderNodePlanesDirect(node, time, ViewIdx(0), 0, RectI(0, 0, kSize, kSize), layers, &planes, &error) || planes.size() != 1 || planes[0].pixels.empty()) {
            return kNotRendered;
        }

        return planes[0].pixels[0];
    }

    std::string persistentMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }

    void frameRangeOf(const NodePtr& node,
                      double* first,
                      double* last)
    {
        node->getEffectInstance()->getFrameRange_public(node->getHashValue(), first, last, true);
    }

    ImageMetadata metadataOf(const NodePtr& node,
                             double time)
    {
        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
        EXPECT_TRUE(effect != NULL);

        return effect ? effect->getOutputMetadata(time, ViewIdx(0)) : ImageMetadata();
    }
};

TEST_F(NativeReadTimeTest, SettingTheFilenameFillsTheOriginalRangeAndTheTimeDomain)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    EXPECT_EQ(1, intValue(node, "originalFrameRange", 0));
    EXPECT_EQ(4, intValue(node, "originalFrameRange", 1));
    EXPECT_EQ(1, intValue(node, "firstFrame"));
    EXPECT_EQ(4, intValue(node, "lastFrame"));
    EXPECT_EQ(1, intValue(node, "startingTime"));
    EXPECT_EQ(0, intValue(node, "timeOffset"));

    double first = 0.;
    double last = 0.;
    frameRangeOf(node, &first, &last);
    EXPECT_EQ(1., first);
    EXPECT_EQ(4., last);
    EXPECT_TRUE(node->getEffectInstance()->isFrameVarying());
}

TEST_F(NativeReadTimeTest, EachFrameRendersItsOwnFile)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(kValues[i], render(node, kFrames[i])) << kFrames[i];
    }
}

TEST_F(NativeReadTimeTest, EveryMissingFramePolicyAtFrameThree)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    EXPECT_EQ(kNotRendered, render(node, 3.));
    EXPECT_FALSE(persistentMessage(node).empty());

    setChoice(node, "onMissingFrame", 0);
    EXPECT_EQ(0.25f, render(node, 3.));
    EXPECT_EQ(std::string(), persistentMessage(node));

    setChoice(node, "onMissingFrame", 1);
    EXPECT_EQ(0.5f, render(node, 3.));

    setChoice(node, "onMissingFrame", 2);
    EXPECT_EQ(0.5f, render(node, 3.));

    setChoice(node, "onMissingFrame", 3);
    EXPECT_EQ(kNotRendered, render(node, 3.));

    setChoice(node, "onMissingFrame", 4);
    EXPECT_EQ(0.f, render(node, 3.));
    EXPECT_EQ(std::string(), persistentMessage(node));
}

TEST_F(NativeReadTimeTest, BeforeAndAfterModesOutsideTheRange)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    EXPECT_EQ(0.125f, render(node, 0.));
    EXPECT_EQ(0.5f, render(node, 5.));

    setChoice(node, "before", 1);
    setChoice(node, "after", 1);
    EXPECT_EQ(0.5f, render(node, 0.));
    EXPECT_EQ(0.125f, render(node, 5.));

    setChoice(node, "before", 2);
    setChoice(node, "after", 2);
    EXPECT_EQ(0.25f, render(node, 0.));
    EXPECT_EQ(0.25f, render(node, 6.));

    setChoice(node, "before", 3);
    setChoice(node, "after", 3);
    EXPECT_EQ(0.f, render(node, 0.));
    EXPECT_EQ(0.f, render(node, 5.));
    EXPECT_EQ(std::string(), persistentMessage(node));

    setChoice(node, "before", 4);
    setChoice(node, "after", 4);
    EXPECT_EQ(kNotRendered, render(node, 0.));
    EXPECT_FALSE(persistentMessage(node).empty());
    EXPECT_EQ(kNotRendered, render(node, 5.));
}

TEST_F(NativeReadTimeTest, StartingTimeMovesTheSequenceAndSyncsTheOffset)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetInt(node, "startingTime", 100);
    EXPECT_EQ(99, intValue(node, "timeOffset"));
    EXPECT_EQ(0.125f, render(node, 100.));
    EXPECT_EQ(0.25f, render(node, 101.));
    EXPECT_EQ(0.5f, render(node, 103.));
    EXPECT_EQ(0.125f, render(node, 99.));

    double first = 0.;
    double last = 0.;
    frameRangeOf(node, &first, &last);
    EXPECT_EQ(100., first);
    EXPECT_EQ(103., last);
}

TEST_F(NativeReadTimeTest, TimeOffsetMovesTheSequenceAndSyncsTheStartingTime)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetChoice(node, "frameMode", 1);
    userSetInt(node, "timeOffset", 10);
    EXPECT_EQ(11, intValue(node, "startingTime"));
    EXPECT_EQ(0.125f, render(node, 11.));
    EXPECT_EQ(0.25f, render(node, 12.));
    EXPECT_EQ(0.5f, render(node, 14.));

    double first = 0.;
    double last = 0.;
    frameRangeOf(node, &first, &last);
    EXPECT_EQ(11., first);
    EXPECT_EQ(14., last);
}

TEST_F(NativeReadTimeTest, ChangingTheFirstFrameNarrowsTheSequenceAndMovesTheStartingTime)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetInt(node, "firstFrame", 2);
    EXPECT_EQ(2, intValue(node, "startingTime"));
    EXPECT_EQ(0.25f, render(node, 1.));
    EXPECT_EQ(0.25f, render(node, 2.));
}

TEST_F(NativeReadTimeTest, ResettingTheFirstFrameRestoresTheRangeAndTheStartingTime)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetInt(node, "firstFrame", 2);
    ASSERT_TRUE(userEdited(node));
    restoreDefaultAsTheGuiDoes(node, "firstFrame");

    EXPECT_EQ(1, intValue(node, "firstFrame"));
    EXPECT_EQ(1, intValue(node, "startingTime"));
    EXPECT_EQ(0, intValue(node, "timeOffset"));
    EXPECT_FALSE(userEdited(node));
    EXPECT_EQ(0.125f, render(node, 1.));
    EXPECT_EQ(0.25f, render(node, 2.));

    for (int frame = 1; frame <= 5; ++frame) {
        ASSERT_TRUE(writeConstantFrame(framePath(dir, "other", frame), 0.5f));
    }
    setFile(node, patternPath(dir, "other"));
    EXPECT_EQ(1, intValue(node, "firstFrame"));
    EXPECT_EQ(5, intValue(node, "lastFrame"));
}

TEST_F(NativeReadTimeTest, ResettingTheLastFrameFollowsTheFileAgain)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    ASSERT_TRUE(dir.isValid());
    for (int frame = 1; frame <= 5; ++frame) {
        ASSERT_TRUE(writeConstantFrame(framePath(dir, "other", frame), 0.5f));
    }
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetInt(node, "lastFrame", 3);
    ASSERT_TRUE(userEdited(node));
    restoreDefaultAsTheGuiDoes(node, "lastFrame");

    EXPECT_EQ(4, intValue(node, "lastFrame"));
    EXPECT_FALSE(userEdited(node));

    setFile(node, patternPath(dir, "other"));
    EXPECT_EQ(5, intValue(node, "lastFrame"));
}

TEST_F(NativeReadTimeTest, ResettingOneEndOfAnEditedRangeKeepsTheOtherEdit)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetInt(node, "firstFrame", 2);
    userSetInt(node, "lastFrame", 3);
    restoreDefaultAsTheGuiDoes(node, "firstFrame");

    EXPECT_EQ(1, intValue(node, "firstFrame"));
    EXPECT_EQ(3, intValue(node, "lastFrame"));
    EXPECT_TRUE(userEdited(node));
}

TEST_F(NativeReadTimeTest, ResettingTheStartingTimeRestoresTheOffset)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetInt(node, "startingTime", 100);
    ASSERT_EQ(99, intValue(node, "timeOffset"));
    restoreDefaultAsTheGuiDoes(node, "startingTime");

    EXPECT_EQ(1, intValue(node, "startingTime"));
    EXPECT_EQ(0, intValue(node, "timeOffset"));
    EXPECT_FALSE(userEdited(node));
    EXPECT_EQ(0.125f, render(node, 1.));
    EXPECT_EQ(0.25f, render(node, 2.));
}

TEST_F(NativeReadTimeTest, ResettingTheTimeOffsetRestoresTheStartingTime)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetChoice(node, "frameMode", 1);
    userSetInt(node, "timeOffset", 10);
    ASSERT_EQ(11, intValue(node, "startingTime"));
    restoreDefaultAsTheGuiDoes(node, "timeOffset");

    EXPECT_EQ(0, intValue(node, "timeOffset"));
    EXPECT_EQ(1, intValue(node, "startingTime"));
    EXPECT_FALSE(userEdited(node));
    EXPECT_EQ(0.125f, render(node, 1.));
}

TEST_F(NativeReadTimeTest, ResettingTheFrameModeShowsTheDefaultModesKnob)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetChoice(node, "frameMode", 1);
    ASSERT_TRUE(knob<KnobInt>(node, "startingTime")->getIsSecret());
    ASSERT_FALSE(knob<KnobInt>(node, "timeOffset")->getIsSecret());
    restoreDefaultAsTheGuiDoes(node, "frameMode");

    EXPECT_EQ(0, knob<KnobChoice>(node, "frameMode")->getValue());
    EXPECT_FALSE(knob<KnobInt>(node, "startingTime")->getIsSecret());
    EXPECT_TRUE(knob<KnobInt>(node, "timeOffset")->getIsSecret());
}

TEST_F(NativeReadTimeTest, AUserEditedTimeDomainSurvivesAFilenameChange)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    ASSERT_TRUE(dir.isValid());
    for (int frame = 1; frame <= 5; ++frame) {
        ASSERT_TRUE(writeConstantFrame(framePath(dir, "other", frame), 0.5f));
    }

    NodePtr followed = createRead(patternPath(dir, "seq"));
    setFile(followed, patternPath(dir, "other"));
    EXPECT_EQ(1, intValue(followed, "firstFrame"));
    EXPECT_EQ(5, intValue(followed, "lastFrame"));

    NodePtr edited = createRead(patternPath(dir, "seq"));
    userSetInt(edited, "firstFrame", 2);
    setFile(edited, patternPath(dir, "other"));
    EXPECT_EQ(5, intValue(edited, "originalFrameRange", 1));
    EXPECT_EQ(2, intValue(edited, "firstFrame"));
    EXPECT_EQ(4, intValue(edited, "lastFrame"));
}

TEST_F(NativeReadTimeTest, ResettingAnEditedRangeAfterAFileChangeAdoptsTheNewFile)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    ASSERT_TRUE(dir.isValid());
    for (int frame = 3; frame <= 6; ++frame) {
        ASSERT_TRUE(writeConstantFrame(framePath(dir, "other", frame), 0.5f));
    }
    NodePtr node = createRead(patternPath(dir, "seq"));

    userSetInt(node, "firstFrame", 2);
    setFile(node, patternPath(dir, "other"));
    ASSERT_EQ(2, intValue(node, "firstFrame"));
    ASSERT_EQ(4, intValue(node, "lastFrame"));
    ASSERT_TRUE(userEdited(node));

    restoreDefaultAsTheGuiDoes(node, "firstFrame");
    restoreDefaultAsTheGuiDoes(node, "lastFrame");

    EXPECT_EQ(3, intValue(node, "firstFrame"));
    EXPECT_EQ(6, intValue(node, "lastFrame"));
    EXPECT_EQ(3, intValue(node, "startingTime"));
    EXPECT_EQ(0, intValue(node, "timeOffset"));
    EXPECT_FALSE(userEdited(node));
    double first = 0.;
    double last = 0.;
    frameRangeOf(node, &first, &last);
    EXPECT_EQ(3., first);
    EXPECT_EQ(6., last);
}

TEST_F(NativeReadTimeTest, PurgingTheCachesFindsFramesAddedOnDisk)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));
    ASSERT_EQ(kNotRendered, render(node, 3.));
    ASSERT_FALSE(metadataOf(node, 3.).getString("ofx/filepath").has_value());

    ASSERT_TRUE(writeConstantFrame(framePath(dir, "seq", 3), 0.375f));
    ASSERT_TRUE(writeConstantFrame(framePath(dir, "seq", 5), 0.75f));
    node->getEffectInstance()->purgeCaches();

    EXPECT_EQ(5, intValue(node, "originalFrameRange", 1));
    EXPECT_EQ(1, intValue(node, "firstFrame"));
    EXPECT_EQ(5, intValue(node, "lastFrame"));
    EXPECT_FALSE(userEdited(node));
    double first = 0.;
    double last = 0.;
    frameRangeOf(node, &first, &last);
    EXPECT_EQ(1., first);
    EXPECT_EQ(5., last);
    EXPECT_EQ(0.375f, render(node, 3.));
    EXPECT_EQ(0.75f, render(node, 5.));
    EXPECT_EQ(std::optional<std::string>(framePath(dir, "seq", 3)), metadataOf(node, 3.).getString("ofx/filepath"));
}

TEST_F(NativeReadTimeTest, PurgingTheCachesKeepsAUserEditedRange)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));
    userSetInt(node, "lastFrame", 2);

    ASSERT_TRUE(writeConstantFrame(framePath(dir, "seq", 5), 0.75f));
    node->getEffectInstance()->purgeCaches();

    EXPECT_EQ(5, intValue(node, "originalFrameRange", 1));
    EXPECT_EQ(2, intValue(node, "lastFrame"));
    EXPECT_TRUE(userEdited(node));

    restoreDefaultAsTheGuiDoes(node, "lastFrame");
    EXPECT_EQ(5, intValue(node, "lastFrame"));
    EXPECT_FALSE(userEdited(node));
}

TEST_F(NativeReadTimeTest, PurgingOneReadKeepsTheHeadersOfAnother)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    ASSERT_TRUE(writeConstantFrame(framePath(dir, "other", 1), 0.5f));
    ASSERT_TRUE(writeConstantFrame(framePath(dir, "proxy", 1), 0.5f));
    NodePtr purged = createRead(patternPath(dir, "seq"));
    KnobFile* proxy = knob<KnobFile>(purged, kOfxImageEffectProxyParamName);
    ASSERT_TRUE(proxy != NULL);
    proxy->setValue(patternPath(dir, "proxy"));
    NodePtr kept = createRead(patternPath(dir, "other"));

    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> ownBefore = OiioReadSupport::readHeader(framePath(dir, "seq", 2), &error);
    const std::shared_ptr<const OiioReadSupport::Header> proxyBefore = OiioReadSupport::readHeader(framePath(dir, "proxy", 1), &error);
    const std::shared_ptr<const OiioReadSupport::Header> otherBefore = OiioReadSupport::readHeader(framePath(dir, "other", 1), &error);
    ASSERT_TRUE(ownBefore && proxyBefore && otherBefore) << error;
    ASSERT_EQ(otherBefore, OiioReadSupport::readHeader(framePath(dir, "other", 1), &error));

    purged->getEffectInstance()->purgeCaches();

    EXPECT_NE(ownBefore, OiioReadSupport::readHeader(framePath(dir, "seq", 2), &error));
    EXPECT_NE(proxyBefore, OiioReadSupport::readHeader(framePath(dir, "proxy", 1), &error));
    EXPECT_EQ(otherBefore, OiioReadSupport::readHeader(framePath(dir, "other", 1), &error));
}

TEST_F(NativeReadTimeTest, ClearingTheCachesRefreshesTheFormatOfARewrittenFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = framePath(dir, "still", 1);
    ASSERT_TRUE(writeConstantFrame(path, 0.5f));
    NodePtr node = createRead(path);
    ASSERT_TRUE(RectI(0, 0, kSize, kSize) == node->getEffectInstance()->getOutputFormat());

    ASSERT_TRUE(writeConstantFrame(path, 0.5f, 8, 6));
    getApp()->clearOpenFXPluginsCaches();

    EXPECT_TRUE(RectI(0, 0, 8, 6) == node->getEffectInstance()->getOutputFormat());
}

TEST_F(NativeReadTimeTest, APatternMatchingOneFileKeepsItsFrameNumber)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeConstantFrame(framePath(dir, "one", 100), 0.75f));
    NodePtr node = createRead(patternPath(dir, "one"));

    EXPECT_EQ(100, intValue(node, "originalFrameRange", 0));
    EXPECT_EQ(100, intValue(node, "originalFrameRange", 1));
    EXPECT_EQ(100, intValue(node, "firstFrame"));
    EXPECT_EQ(100, intValue(node, "lastFrame"));
    EXPECT_EQ(100, intValue(node, "startingTime"));
    double first = 0.;
    double last = 0.;
    frameRangeOf(node, &first, &last);
    EXPECT_EQ(100., first);
    EXPECT_EQ(100., last);
    EXPECT_EQ(0.75f, render(node, 100.));
    EXPECT_EQ(std::optional<int>(100), metadataOf(node, 100.).getInt("ofx/frame"));
}

TEST_F(NativeReadTimeTest, TheReadHasNoFrameRateKnobs)
{
    NodePtr node = createRead(std::string());

    EXPECT_FALSE(bool(node->getKnobByName("frameRate")));
    EXPECT_FALSE(bool(node->getKnobByName("customFps")));
}

TEST_F(NativeReadTimeTest, FirstAndLastFrameAreFieldsWithoutASlider)
{
    NodePtr node = createRead(std::string());

    EXPECT_TRUE(knob<KnobInt>(node, "firstFrame")->isSliderDisabled());
    EXPECT_TRUE(knob<KnobInt>(node, "lastFrame")->isSliderDisabled());
}

TEST_F(NativeReadTimeTest, ASingleImageAnswersEveryFrame)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = framePath(dir, "still", 7);
    ASSERT_TRUE(writeConstantFrame(path, 0.75f));
    NodePtr node = createRead(path);

    EXPECT_EQ(0.75f, render(node, 1.));
    EXPECT_EQ(0.75f, render(node, 50.));
}
