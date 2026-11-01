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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/ProjectPathVariableCommands.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kSize = 4;

bool
writeFrame(const QTemporaryDir& dir,
           int frame)
{
    const std::string path = (dir.path() + QString::fromUtf8("/seq.%1.exr").arg(frame, 4, 10, QLatin1Char('0'))).toStdString();
    OIIO::ImageSpec spec(kSize, kSize, 4, OIIO::TypeDesc::FLOAT);
    std::vector<float> pixels((std::size_t)kSize * kSize * 4, 0.5f);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}
} // namespace

class NativeReadPathVariableTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown() OVERRIDE
    {
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    NodePtr createRead(const std::string& pattern)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));
        if (node) {
            fileKnob(node)->setValue(pattern);
        }

        return node;
    }

    static KnobFile* fileKnob(const NodePtr& node)
    {
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        EXPECT_TRUE(file != NULL);

        return file;
    }

    static int originalFrame(const NodePtr& node,
                             int dimension)
    {
        KnobInt* range = dynamic_cast<KnobInt*>(node->getKnobByName("originalFrameRange").get());
        EXPECT_TRUE(range != NULL);

        return range ? range->getValue(dimension) : -1;
    }

    KnobString* pathVariable(const std::string& name) const
    {
        return dynamic_cast<KnobString*>(project()->getKnobByName(name).get());
    }

    // What the project panel does when the user types a new value.
    void userSetPathVariable(const std::string& name,
                             const std::string& value)
    {
        KnobString* variable = pathVariable(name);
        ASSERT_TRUE(variable != NULL) << name;
        variable->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }
};

TEST_F(NativeReadPathVariableTest, ChangingAProjectPathRefreshesTheReadsUsingIt)
{
    QTemporaryDir first;
    QTemporaryDir second;
    ASSERT_TRUE(first.isValid());
    ASSERT_TRUE(second.isValid());
    for (int frame = 1; frame <= 3; ++frame) {
        ASSERT_TRUE(writeFrame(first, frame));
    }
    for (int frame = 5; frame <= 6; ++frame) {
        ASSERT_TRUE(writeFrame(second, frame));
    }
    std::string error;
    ASSERT_TRUE(bool(project()->addPathVariable("plates", first.path().toStdString(), &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("shot", "[plates]", &error))) << error;

    NodePtr direct = createRead("[plates]/seq.####.exr");
    NodePtr nested = createRead("[shot]/seq.####.exr");
    ASSERT_TRUE(direct && nested);
    EXPECT_EQ(1, originalFrame(direct, 0));
    EXPECT_EQ(3, originalFrame(direct, 1));
    EXPECT_EQ(1, originalFrame(nested, 0));
    EXPECT_EQ(3, originalFrame(nested, 1));
    const U64 directHash = direct->getHashValue();
    const U64 nestedHash = nested->getHashValue();

    userSetPathVariable("plates", second.path().toStdString());

    EXPECT_EQ(std::string("[plates]/seq.####.exr"), fileKnob(direct)->getValue());
    EXPECT_EQ(5, originalFrame(direct, 0));
    EXPECT_EQ(6, originalFrame(direct, 1));
    EXPECT_EQ(5, originalFrame(nested, 0));
    EXPECT_EQ(6, originalFrame(nested, 1));
    EXPECT_NE(directHash, direct->getHashValue());
    EXPECT_NE(nestedHash, nested->getHashValue());
}

TEST_F(NativeReadPathVariableTest, OnlyTheFilePathsUsingAProjectPathDependOnIt)
{
    std::string error;
    ASSERT_TRUE(bool(project()->addPathVariable("plates", "/plates", &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("shot", "[plates]/s01", &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("renders", "/renders", &error))) << error;

    NodePtr nested = createRead("[shot]/a.exr");
    NodePtr other = createRead("[renders]/a.exr");
    ASSERT_TRUE(nested && other);

    const KnobsVec usingPlates = project()->getFilePathKnobsUsingPathVariable("plates");
    ASSERT_EQ(1u, usingPlates.size());
    EXPECT_EQ(nested->getKnobByName(kOfxImageEffectFileParamName), usingPlates[0]);
    const KnobsVec spellingPlates = project()->getKnobsSpellingPathVariable("plates");
    ASSERT_EQ(1u, spellingPlates.size());
    EXPECT_EQ(project()->getKnobByName("shot"), spellingPlates[0]);
}

TEST_F(NativeReadPathVariableTest, RenamingAProjectPathRewritesTheFilePathsAsOneUndoableStep)
{
    std::string error;
    ASSERT_TRUE(bool(project()->addPathVariable("plates", "/plates", &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("shot", "[plates]/s01", &error))) << error;
    ASSERT_TRUE(bool(project()->addPathVariable("platesB", "/platesB", &error))) << error;

    NodePtr direct = createRead("[plates]/a.####.exr");
    NodePtr nested = createRead("[shot]/b.exr");
    NodePtr similar = createRead("[platesB]/c.exr");
    ASSERT_TRUE(direct && nested && similar);

    RenamePathVariableCommand command(project(), "plates", "footage", "plates", "Footage");
    EXPECT_TRUE(command.hasValueEdits());
    command.redo();
    ASSERT_TRUE(command.getError().empty()) << command.getError();

    EXPECT_FALSE(bool(project()->getKnobByName("plates")));
    ASSERT_TRUE(pathVariable("footage") != NULL);
    EXPECT_EQ(std::string("Footage"), pathVariable("footage")->getLabel());
    EXPECT_EQ(std::string("/plates"), pathVariable("footage")->getValue());
    EXPECT_EQ(std::string("[footage]/s01"), pathVariable("shot")->getValue());
    EXPECT_EQ(std::string("[footage]/a.####.exr"), fileKnob(direct)->getValue());
    EXPECT_EQ(std::string("[shot]/b.exr"), fileKnob(nested)->getValue());
    EXPECT_EQ(std::string("[platesB]/c.exr"), fileKnob(similar)->getValue());

    command.undo();

    EXPECT_FALSE(bool(project()->getKnobByName("footage")));
    ASSERT_TRUE(pathVariable("plates") != NULL);
    EXPECT_EQ(std::string("plates"), pathVariable("plates")->getLabel());
    EXPECT_EQ(std::string("[plates]/s01"), pathVariable("shot")->getValue());
    EXPECT_EQ(std::string("[plates]/a.####.exr"), fileKnob(direct)->getValue());

    command.redo();

    EXPECT_EQ(std::string("[footage]/a.####.exr"), fileKnob(direct)->getValue());
    EXPECT_EQ(std::string("[footage]/s01"), pathVariable("shot")->getValue());
}

TEST_F(NativeReadPathVariableTest, CreatingAProjectPathForAFileIsOneUndoableStep)
{
    NodePtr read = createRead("/plates/s01/a.####.exr");
    ASSERT_TRUE(bool(read));

    AddPathVariableCommand command(project(), "s01", "/plates/s01");
    command.addValueEdit(read->getKnobByName(kOfxImageEffectFileParamName), "/plates/s01/a.####.exr", "[s01]/a.####.exr");
    command.redo();
    ASSERT_TRUE(command.getError().empty()) << command.getError();
    ASSERT_TRUE(pathVariable("s01") != NULL);
    EXPECT_EQ(project()->getRFVGroup(), pathVariable("s01")->getParentKnob());
    EXPECT_EQ(std::string("[s01]/a.####.exr"), fileKnob(read)->getValue());

    command.undo();
    EXPECT_FALSE(bool(project()->getKnobByName("s01")));
    EXPECT_EQ(std::string("/plates/s01/a.####.exr"), fileKnob(read)->getValue());

    command.redo();
    ASSERT_TRUE(pathVariable("s01") != NULL);
    EXPECT_EQ(std::string("[s01]/a.####.exr"), fileKnob(read)->getValue());
}

TEST_F(NativeReadPathVariableTest, CreatingATakenNameChangesNothing)
{
    std::string error;
    ASSERT_TRUE(bool(project()->addPathVariable("s01", "/elsewhere", &error))) << error;
    NodePtr read = createRead("/plates/s01/a.exr");
    ASSERT_TRUE(bool(read));

    AddPathVariableCommand command(project(), "s01", "/plates/s01");
    command.addValueEdit(read->getKnobByName(kOfxImageEffectFileParamName), "/plates/s01/a.exr", "[s01]/a.exr");
    command.redo();
    EXPECT_FALSE(command.getError().empty());
    EXPECT_EQ(std::string("/plates/s01/a.exr"), fileKnob(read)->getValue());

    command.undo();
    ASSERT_TRUE(pathVariable("s01") != NULL);
    EXPECT_EQ(std::string("/elsewhere"), pathVariable("s01")->getValue());
}
