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
#include <vector>

#include <gtest/gtest.h>

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
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

bool
isNativeRead(const NodePtr& node)
{
    return node && dynamic_cast<NativeRead*>(node->getEffectInstance().get());
}

} // namespace

class NativeReadSkeletonTest
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
};

TEST_F(NativeReadSkeletonTest, ExplicitMajorTwoIsTheNativeNode)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);

    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isNativeRead(node));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_READ, node->getMajorVersion());
    EXPECT_TRUE(node->getEffectInstance()->isReader());
    EXPECT_EQ(0, node->getEffectInstance()->getNInputs());
}

TEST_F(NativeReadSkeletonTest, OfxReaderIDBuildsNothingAtTheNativeMajor)
{
    CreateNodeArgs args(PLUGINID_OFX_READOIIO, getApp()->getProject());
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_READ, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);
    args.setProperty<bool>(kCreateNodeArgsPropSilent, true);

    EXPECT_FALSE(bool(getApp()->createNode(args)));
}

TEST_F(NativeReadSkeletonTest, RendersBlack)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
    ASSERT_TRUE(isNativeRead(node));

    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 8, 8), layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ(8u * 8u * 4u, planes[0].pixels.size());
    for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
        EXPECT_EQ(0.f, planes[0].pixels[i]) << i;
    }
}

TEST_F(NativeReadSkeletonTest, MajorAndFilenameSurviveSaveAndLoad)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
    ASSERT_TRUE(isNativeRead(node));

    KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(std::string("/tmp/native-read-skeleton.png"));
    const std::string name = node->getScriptName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("native-read.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(name).get() == NULL);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_TRUE(isNativeRead(loaded));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_READ, loaded->getMajorVersion());
    KnobFile* loadedFile = dynamic_cast<KnobFile*>(loaded->getKnobByName(kOfxImageEffectFileParamName).get());
    ASSERT_TRUE(loadedFile != NULL);
    EXPECT_EQ(std::string("/tmp/native-read-skeleton.png"), loadedFile->getValue());
}
