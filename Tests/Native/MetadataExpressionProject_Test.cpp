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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {
const int kSize = 4;

bool
writeExr(const std::string& path)
{
    OIIO::ImageSpec spec(kSize, kSize, 4, OIIO::TypeDesc::FLOAT);

    spec.attribute("myString", "shot-042");
    spec.attribute("myInt", 42);

    std::vector<float> pixels((std::size_t)kSize * kSize * 4, 0.25f);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}
} // namespace

class MetadataExpressionProjectTest
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
};

TEST_F(MetadataExpressionProjectTest, LabelExpressionReadsAFileAttributeAcrossSaveAndLoad)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const std::string exrPath = (tmp.path() + QString::fromUtf8("/source.exr")).toStdString();
    ASSERT_TRUE(writeExr(exrPath));

    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
    NodePtr holder = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(read && holder);

    KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(exrPath);
    read->getEffectInstance()->refreshMetadata_public(false);

    const std::string readName = read->getScriptName();
    const std::string holderName = holder->getScriptName();

    KnobString* label = dynamic_cast<KnobString*>(holder->getKnobByName(kUserLabelKnobName).get());
    ASSERT_TRUE(label != NULL);
    ASSERT_NO_THROW(label->setExpression(0, "str(" + readName + ".metadata('exr/myInt')) + ':' + " + readName + ".metadata('exr/myString')", false, true));

    EXPECT_EQ("42:shot-042", label->getValue());

    ProjectPtr project = getApp()->getProject();
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("metadata-expression.ntpz");
    QString savedFilePath;

    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    OiioReadSupport::clearHeaderCache();
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr holder2 = project->getNodeByName(holderName);
    ASSERT_TRUE(bool(holder2));

    KnobString* label2 = dynamic_cast<KnobString*>(holder2->getKnobByName(kUserLabelKnobName).get());
    ASSERT_TRUE(label2 != NULL);

    EXPECT_EQ("42:shot-042", label2->getValue());
}
