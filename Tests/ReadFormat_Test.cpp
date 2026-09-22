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

#include <list>
#include <string>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ReadNode.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {
const char kSmallFixture[] = NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr";
const int kSmallSize = 8;
const char kLargeFixture[] = NATRON_TESTS_FIXTURES_DIR "/tracker-patch.0001.exr";
const int kLargeSize = 128;

void
expectSquareFormat(const NodePtr& read,
                   int size)
{
    EffectInstancePtr effect = read->getEffectInstance();
    const RectI format = effect->getOutputFormat();
    EXPECT_EQ(RectI(0, 0, size, size), format) << read->getScriptName() << " output format is " << format.x1 << ' ' << format.y1 << ' ' << format.x2 << ' ' << format.y2;

    RectD rod;
    bool isProjectFormat = false;
    ASSERT_EQ(eStatusOK, effect->getRegionOfDefinition_public(read->getHashValue(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    EXPECT_EQ(RectD(0., 0., size, size), rod) << read->getScriptName() << " RoD is " << rod.x1 << ' ' << rod.y1 << ' ' << rod.x2 << ' ' << rod.y2;
    EXPECT_FALSE(isProjectFormat);
}

void
expectProjectFormat(const ProjectPtr& project,
                    int size)
{
    Format format;
    project->getProjectDefaultFormat(&format);
    EXPECT_EQ(size, format.width());
    EXPECT_EQ(size, format.height());
}
} // namespace

// The project format is set once, by the first reader rendered in a fresh project; every
// reader after that must still carry its own file's format, not the project's.
class ReadFormatTest
    : public BaseTest {
protected:
    NodePtr createRead(const std::string& file)
    {
        CreateNodeArgs args(_readOIIOPluginID.toStdString(), getApp()->getProject());
        args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, file);
        NodePtr read = getApp()->createNode(args);
        if (read && !dynamic_cast<ReadNode*>(read->getEffectInstance().get())) {
            ADD_FAILURE() << "the reader is not backed by a Read container";
            return NodePtr();
        }

        return read;
    }

    // Renders one frame of the reader through a Write: the project format autoset lives in
    // the render path, on a render thread, exactly as when a viewer first shows the reader.
    void renderOnce(const NodePtr& read)
    {
        NodePtr writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(bool(writer));
        connectNodes(read, writer, 0, true);

        ASSERT_TRUE(_tmp.isValid());
        const std::string path = (_tmp.path() + QLatin1String("/") + QString::fromStdString(read->getScriptName()) + QLatin1String(".exr")).toStdString();
        writer->setOutputFilesForWriter(path);

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        ASSERT_TRUE(writerEffect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);
        ASSERT_TRUE(QFile::exists(QString::fromStdString(path))) << "frame was not rendered: " << path;
    }

    void autosetProjectFormatFromSmallRead()
    {
        ProjectPtr project = getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);

        _small = createRead(kSmallFixture);
        ASSERT_TRUE(bool(_small));
        expectSquareFormat(_small, kSmallSize);

        renderOnce(_small);
        if (HasFatalFailure()) {
            return;
        }
        EXPECT_FALSE(project->isAutoSetProjectFormatEnabled());
        expectProjectFormat(project, kSmallSize);
    }

    QTemporaryDir _tmp;
    NodePtr _small;
};

TEST_F(ReadFormatTest, SecondReadKeepsItsFileFormatAfterProjectFormatAutoset)
{
    autosetProjectFormatFromSmallRead();
    if (HasFatalFailure()) {
        return;
    }

    NodePtr large = createRead(kLargeFixture);
    ASSERT_TRUE(bool(large));
    expectSquareFormat(large, kLargeSize);
    expectSquareFormat(_small, kSmallSize);
    expectProjectFormat(getApp()->getProject(), kSmallSize);

    renderOnce(large);
    if (HasFatalFailure()) {
        return;
    }
    expectSquareFormat(large, kLargeSize);
    expectProjectFormat(getApp()->getProject(), kSmallSize);
}

TEST_F(ReadFormatTest, ReadFollowsItsNewFileFormatAfterProjectFormatAutoset)
{
    autosetProjectFormatFromSmallRead();
    if (HasFatalFailure()) {
        return;
    }

    KnobFile* filename = dynamic_cast<KnobFile*>(_small->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(filename != NULL);
    filename->setValue(kLargeFixture);

    expectSquareFormat(_small, kLargeSize);
    expectProjectFormat(getApp()->getProject(), kSmallSize);
}
