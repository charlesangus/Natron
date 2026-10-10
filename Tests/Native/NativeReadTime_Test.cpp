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
                   float value)
{
    OIIO::ImageSpec spec(kSize, kSize, 4, OIIO::TypeDesc::FLOAT);
    std::vector<float> pixels((std::size_t)kSize * kSize * 4, value);
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

TEST_F(NativeReadTimeTest, ACustomFrameRateReachesTheOutputMetadata)
{
    QTemporaryDir dir;
    writeSequence(dir, "seq");
    NodePtr node = createRead(patternPath(dir, "seq"));

    knob<KnobBool>(node, "customFps")->setValue(true);
    knob<KnobDouble>(node, "frameRate")->setValue(30.);
    node->getEffectInstance()->refreshMetadata_public(false);
    EXPECT_DOUBLE_EQ(30., node->getEffectInstance()->getFrameRate());
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
