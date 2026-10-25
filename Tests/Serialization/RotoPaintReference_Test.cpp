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

#include <cstdlib>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// The project.yaml a project with a RotoPaint node saves is compared with a committed reference,
// so a change to how the roto contexts are built, saved or loaded shows up as a text diff. To
// record the reference, run this test with NATRON_GOLDEN_RECORD_DIR set to
// Tests/fixtures/roto-roundtrip and commit rotopaint.yaml. The build that saved the project and
// the temporary directory it was saved in differ from run to run, so normalizedYaml() drops the
// savedBy line and replaces the project path with a placeholder before anything is compared or
// recorded.

namespace {

const char* const kReferenceName = "rotopaint.yaml";

std::string
referencePath()
{
    return std::string(NATRON_TESTS_FIXTURES_DIR) + "/roto-roundtrip/" + kReferenceName;
}

bool
readFile(const std::string& path,
         std::string* text)
{
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    *text = QString::fromUtf8(file.readAll()).toStdString();

    return true;
}

bool
writeFile(const std::string& path,
          const std::string& text)
{
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray bytes = QString::fromStdString(text).toUtf8();

    return file.write(bytes) == bytes.size();
}

const char* const kProjectPathPlaceholder = "@TMP@";

std::string
normalizedYaml(const std::string& yaml)
{
    const std::string projectPathStart = "<Name>Project</Name><Value>";
    const std::string projectPathEnd = "</Value>";
    std::istringstream in(yaml);
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 8, "savedBy:") == 0) {
            continue;
        }
        if (line.find("projectPaths:") != std::string::npos) {
            const std::size_t start = line.find(projectPathStart);
            if (start != std::string::npos) {
                const std::size_t valueStart = start + projectPathStart.size();
                const std::size_t valueEnd = line.find(projectPathEnd, valueStart);
                if (valueEnd != std::string::npos) {
                    line.replace(valueStart, valueEnd - valueStart, kProjectPathPlaceholder);
                }
            }
        }
        out += line;
        if (!in.eof()) {
            out += '\n';
        }
    }

    return out;
}

} // anonymous namespace

class RotoPaintReferenceTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A RotoPaint node holding a square with a feather and keyed opacity and control points, an
    // ellipse in a second layer, and a one-point open Bezier.
    bool buildProject()
    {
        NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
        if (!roto || !roto->getRotoContext()) {
            return false;
        }
        roto->setPosition(120., -80.);
        RotoContextPtr context = roto->getRotoContext();

        BezierPtr square = context->makeSquare(10., 90., 40., 1.);
        if (!square) {
            return false;
        }
        square->getFeatherKnob()->setValue(6.);
        square->setKeyframe(10.);
        square->setPointAtIndex(false, 0, 10., 14., 96., 0., 0., 0., 0.);
        square->setPointAtIndex(false, 2, 10., 52., 48., 0., 0., 0., 0.);
        KnobDoublePtr opacity = square->getOpacityKnob();
        opacity->setValueAtTime(1., 1., ViewSpec::all(), 0);
        opacity->setValueAtTime(10., 0.25, ViewSpec::all(), 0);

        RotoLayerPtr layer = context->addLayer();
        if (!layer) {
            return false;
        }
        BezierPtr ellipse = context->makeEllipse(100., 60., 30., true, 1.);
        if (!ellipse) {
            return false;
        }
        ellipse->getFeatherKnob()->setValue(3.);

        return true;
    }

    std::string saveYaml(const QTemporaryDir& tmp,
                         const char* fileName)
    {
        std::string yaml;
        EXPECT_TRUE(saveProjectBundle(getApp()->getProject(), tmp, QString::fromUtf8(fileName), &yaml));

        return normalizedYaml(yaml);
    }

    bool reload(const QTemporaryDir& tmp,
                const char* fileName)
    {
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);

        return project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8(fileName));
    }
};

TEST_F(RotoPaintReferenceTest, SavedProjectMatchesTheReference)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(buildProject());

    const std::string yaml = saveYaml(tmp, "roto.ntpz");
    ASSERT_FALSE(yaml.empty());

    const char* const record = std::getenv("NATRON_GOLDEN_RECORD_DIR");
    if (record && *record) {
        QDir().mkpath(QString::fromUtf8(record));
        ASSERT_TRUE(writeFile(std::string(record) + "/" + kReferenceName, yaml)) << "cannot write the reference under " << record;

        return;
    }

    std::string reference;
    ASSERT_TRUE(readFile(referencePath(), &reference)) << "no reference " << referencePath()
                                                       << ". Record it with NATRON_GOLDEN_RECORD_DIR=" << NATRON_TESTS_FIXTURES_DIR << "/roto-roundtrip and commit the file.";
    EXPECT_EQ(normalizedYaml(reference), yaml);
}

TEST_F(RotoPaintReferenceTest, LoadThenSaveIsStable)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(buildProject());

    const std::string first = saveYaml(tmp, "first.ntpz");
    ASSERT_FALSE(first.empty());
    ASSERT_TRUE(reload(tmp, "first.ntpz"));

    const std::string second = saveYaml(tmp, "second.ntpz");
    EXPECT_EQ(first, second);

    ASSERT_TRUE(reload(tmp, "second.ntpz"));
    EXPECT_EQ(first, saveYaml(tmp, "third.ntpz"));
}
