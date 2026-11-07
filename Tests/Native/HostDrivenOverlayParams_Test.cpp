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

#include <gtest/gtest.h>

#include <QFile>
#include <QLatin1Char>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {

// Settings::setRenderOnEditingFinishedOnly() also writes the setting to disk, which would leak
// the value into later runs.
class RenderOnEditingFinishedScope {
public:
    RenderOnEditingFinishedScope()
        : _knob(appPTR->getCurrentSettings()->getKnobByNameAndType<KnobBool>("renderOnEditingFinished"))
        , _saved(_knob ? _knob->getValue() : false)
    {
    }

    ~RenderOnEditingFinishedScope()
    {
        if (_knob) {
            _knob->setValue(_saved);
        }
    }

    bool valid() const
    {
        return bool(_knob);
    }

    void set(bool on)
    {
        _knob->setValue(on);
    }

private:
    KnobBoolPtr _knob;
    bool _saved;
};

bool
hostHiDPI()
{
    return appPTR->getScreenDevicePixelRatio() >= 1.5;
}

} // namespace

class HostDrivenOverlayParamsTest
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

    NodePtr createFixture()
    {
        return createNode(QString::fromUtf8(kNatronTestOverlayParamsPluginID), kNatronTestOverlayParamsMajor);
    }

    void expectHostDriven(const NodePtr& node,
                          bool interactive)
    {
        KnobBoolPtr interactiveKnob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kNatronTestOverlayParamsInteractive));
        KnobBoolPtr hidpiKnob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kNatronTestOverlayParamsHidpi));
        ASSERT_TRUE(interactiveKnob && hidpiKnob);

        EXPECT_TRUE(interactiveKnob->getIsSecret());
        EXPECT_FALSE(interactiveKnob->getIsPersistent());
        EXPECT_EQ(interactive, interactiveKnob->getValue());

        EXPECT_TRUE(hidpiKnob->getIsSecret());
        EXPECT_FALSE(hidpiKnob->getIsPersistent());
        EXPECT_EQ(hostHiDPI(), hidpiKnob->getValue());
    }
};

TEST_F(HostDrivenOverlayParamsTest, HiddenNotPersistentAndFollowTheSetting)
{
    RenderOnEditingFinishedScope setting;
    ASSERT_TRUE(setting.valid());
    setting.set(false);

    NodePtr node = createFixture();
    ASSERT_TRUE(bool(node));
    expectHostDriven(node, true);

    setting.set(true);
    expectHostDriven(node, false);

    setting.set(false);
    expectHostDriven(node, true);
}

TEST_F(HostDrivenOverlayParamsTest, CreationReadsTheSetting)
{
    RenderOnEditingFinishedScope setting;
    ASSERT_TRUE(setting.valid());
    setting.set(true);

    NodePtr node = createFixture();
    ASSERT_TRUE(bool(node));
    expectHostDriven(node, false);
}

// Saved with the setting on and loaded with it off: the loaded node follows the setting of the
// session that opens the project, not the one that saved it.
TEST_F(HostDrivenOverlayParamsTest, ProjectLoadFollowsTheCurrentSetting)
{
    RenderOnEditingFinishedScope setting;
    ASSERT_TRUE(setting.valid());
    setting.set(true);

    NodePtr node = createFixture();
    ASSERT_TRUE(bool(node));
    expectHostDriven(node, false);
    const std::string scriptName = node->getScriptName();
    node.reset();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("overlayParams.ntpz");
    QString savedFilePath;
    ProjectPtr project = getApp()->getProject();
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));
    project->reset(false, true);

    setting.set(false);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(scriptName);
    ASSERT_TRUE(bool(loaded));
    expectHostDriven(loaded, true);
}
