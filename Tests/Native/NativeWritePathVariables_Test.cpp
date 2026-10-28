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

#include <cmath>
#include <cstddef>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kOutputSpaceMenuKnob = "ocioOutputSpaceIndex";
const char* const kOutVariable = "Out";
const char* const kOutSubdir = "out";

const int kWidth = 16;
const int kHeight = 8;
const float kGray = 0.18f;
} // namespace

class NativeWritePathVariablesTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
        ASSERT_TRUE(_dir.isValid());
        ASSERT_TRUE(QDir(_dir.path()).mkpath(QString::fromUtf8(kOutSubdir)));
        ASSERT_TRUE(QDir(_dir.path()).mkpath(QString::fromUtf8("rel")));
        // The variables must resolve before any file name is set: a [Project] change rewrites the
        // file names spelled through it.
        setPathVariable(NATRON_PROJECT_ENV_VAR_NAME, _dir.path().toStdString());
        setPathVariable(kOutVariable, pathOf(kOutSubdir));

        project()->setAutoSetProjectFormatEnabled(true);
        project()->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWritePathVariables", 1.));

        _checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        ASSERT_TRUE(bool(_checker));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamBoxSize, { 4096., 2112. }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamLineWidth, { 0. }));
        ASSERT_TRUE(setKnobValues(_checker, kCheckerBoardParamCenterLineWidth, { 0. }));
        for (const char* name : { kCheckerBoardParamColor0, kCheckerBoardParamColor1, kCheckerBoardParamColor2, kCheckerBoardParamColor3 }) {
            ASSERT_TRUE(setKnobValues(_checker, name, { (double)kGray, (double)kGray, (double)kGray, 1. }));
        }
        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(bool(_writer));
        connectNodes(_checker, _writer, 0, true);
        setBitDepth("32f");
        pickOutputSpace(project()->getWorkingColorSpace());
    }

    virtual void TearDown() OVERRIDE
    {
        _writer.reset();
        _checker.reset();
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    static bool exists(const std::string& path)
    {
        return QFileInfo::exists(QString::fromStdString(path));
    }

    // Sets the row `name` of the project's path variables to `value`, adding it if missing.
    void setPathVariable(const std::string& name,
                         const std::string& value)
    {
        KnobPathPtr variables = project()->getEnvVarKnob();

        ASSERT_TRUE(bool(variables));
        std::list<std::vector<std::string>> table;
        variables->getTable(&table);
        bool found = false;
        for (std::list<std::vector<std::string>>::iterator it = table.begin(); it != table.end(); ++it) {
            if ((it->size() == 2) && ((*it)[0] == name)) {
                (*it)[1] = value;
                found = true;
                break;
            }
        }
        if (!found) {
            table.push_back(std::vector<std::string> { name, value });
        }
        variables->setTable(table);
    }

    KnobOutputFile* fileKnob() const
    {
        return dynamic_cast<KnobOutputFile*>(_writer->getKnobByName(kOfxImageEffectFileParamName).get());
    }

    void setFile(const std::string& path)
    {
        KnobOutputFile* file = fileKnob();

        ASSERT_TRUE(file != NULL);
        file->setValue(path);
    }

    void setBitDepth(const std::string& id)
    {
        KnobChoice* choice = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kNatronWriteParamBitDepth).get());

        ASSERT_TRUE(choice != NULL);
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return;
            }
        }
        ADD_FAILURE() << "no bit depth " << id;
    }

    void pickOutputSpace(const std::string& space)
    {
        KnobChoice* menu = dynamic_cast<KnobChoice*>(_writer->getKnobByName(kOutputSpaceMenuKnob).get());

        ASSERT_TRUE(menu != NULL);
        const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == space) {
                menu->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0, true);

                return;
            }
        }
        ADD_FAILURE() << "no colourspace " << space;
    }

    void writeFrameOne()
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
        ASSERT_FALSE(effect->isDoingSequentialRender());
    }

    std::string message() const
    {
        QString text;
        int type = 0;

        _writer->getPersistentMessage(&text, &type, false);

        return text.toStdString();
    }

    // Frame 1 of a Read of `pattern`, as RGBA floats.
    bool readFrameOne(const std::string& pattern,
                      std::vector<float>* pixels)
    {
        NodePtr read = createWorkingSpaceRead(pattern);

        if (!read) {
            return false;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            EXPECT_EQ(pattern, file->getValue());
        }
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool ok = renderNodePlanesDirect(read, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error) && (planes.size() == 1);
        EXPECT_TRUE(ok) << pattern << ": " << error;
        if (ok) {
            *pixels = planes[0].pixels;
        }
        read->destroyNode(true, false);

        return ok;
    }

    static bool isGray(const std::vector<float>& pixels)
    {
        if (pixels.size() != (std::size_t)kWidth * kHeight * 4) {
            return false;
        }
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const float want = (i % 4 == 3) ? 1.f : kGray;
            if (std::fabs(pixels[i] - want) > 1e-5f) {
                return false;
            }
        }

        return true;
    }

    NodePtr _checker;
    NodePtr _writer;
    QTemporaryDir _dir;
};

TEST_F(NativeWritePathVariablesTest, AWriteToTheProjectVariableLandsInTheProjectDirectory)
{
    setFile("[Project]/w.####.exr");
    writeFrameOne();

    EXPECT_FALSE(_writer->hasPersistentMessage()) << message();
    EXPECT_TRUE(exists(pathOf("w.0001.exr")));
    EXPECT_EQ(std::string("[Project]/w.####.exr"), fileKnob()->getValue());
}

TEST_F(NativeWritePathVariablesTest, AWriteToAUserVariableLandsInItsDirectory)
{
    setFile("[Out]/w.####.exr");
    writeFrameOne();

    EXPECT_FALSE(_writer->hasPersistentMessage()) << message();
    EXPECT_TRUE(exists(pathOf("out/w.0001.exr")));
    EXPECT_EQ(std::string("[Out]/w.####.exr"), fileKnob()->getValue());
}

TEST_F(NativeWritePathVariablesTest, ARelativeWritePathIsRelativeToTheProject)
{
    setFile("rel/w.####.exr");
    writeFrameOne();

    EXPECT_FALSE(_writer->hasPersistentMessage()) << message();
    EXPECT_TRUE(exists(pathOf("rel/w.0001.exr")));
    EXPECT_EQ(std::string("rel/w.####.exr"), fileKnob()->getValue());
}

TEST_F(NativeWritePathVariablesTest, TheGeneratedFileNameIsExpandedAndTheStoredValueIsNot)
{
    setFile("[Out]/w.####.exr");

    EXPECT_EQ(QString::fromStdString(pathOf("out/w.0007.exr")), fileKnob()->generateFileNameAtTime(7, ViewIdx(0)));
    EXPECT_EQ(std::string("[Out]/w.####.exr"), fileKnob()->getValue());
}

TEST_F(NativeWritePathVariablesTest, AReadOfEitherVariableReadsTheWrittenFrameBack)
{
    setFile("[Project]/w.####.exr");
    writeFrameOne();
    ASSERT_TRUE(exists(pathOf("w.0001.exr"))) << message();
    setFile("[Out]/w.####.exr");
    writeFrameOne();
    ASSERT_TRUE(exists(pathOf("out/w.0001.exr"))) << message();

    std::vector<float> fromProject;
    std::vector<float> fromOut;
    ASSERT_TRUE(readFrameOne("[Project]/w.####.exr", &fromProject));
    ASSERT_TRUE(readFrameOne("[Out]/w.####.exr", &fromOut));
    EXPECT_TRUE(isGray(fromProject));
    EXPECT_TRUE(isGray(fromOut));
    EXPECT_EQ(fromProject, fromOut);
}

TEST_F(NativeWritePathVariablesTest, AnUnknownVariableFailsAndWritesNothing)
{
    setFile("[Nope]/w.exr");
    writeFrameOne();

    EXPECT_TRUE(_writer->hasPersistentMessage());
    EXPECT_NE(std::string::npos, message().find("[Nope]")) << message();
    EXPECT_FALSE(exists(pathOf("[Nope]")));
    EXPECT_FALSE(exists("[Nope]/w.exr"));
    EXPECT_EQ(std::string("[Nope]/w.exr"), fileKnob()->getValue());
}
