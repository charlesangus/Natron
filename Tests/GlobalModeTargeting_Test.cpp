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
#include <memory>
#include <vector>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/EffectInstance.h"
#include "Engine/GlobalModeKind.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Transform/Transform.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorTrackballGlobalMode.h"
#include "Gui/ColorTrackballTarget.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/TransformGroupGlobalModeTarget.h"
#include "Gui/ValueKnobGlobalModeTarget.h"

#include "GlobalModeTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

class GlobalModeTargetingTest
    : public GlobalModeNodeTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        GlobalModeNodeTest::SetUp();
        _page.reset(new QWidget());
        new QVBoxLayout(_page.get());
        _panel.reset(new GlobalModeTestContainer(_page.get()));

        _transform = create(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM);
        _grade = create(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
        ASSERT_TRUE(bool(_transform));
        ASSERT_TRUE(bool(_grade));
    }

    virtual void TearDown() OVERRIDE
    {
        _guis.clear();
        _panel.reset();
        _page.reset();
        _transform.reset();
        _grade.reset();
        GlobalModeNodeTest::TearDown();
    }

    // Builds knob's widgets on the page, the way a settings panel lays out one row.
    KnobGuiPtr buildRow(const KnobIPtr& knob)
    {
        KnobGuiPtr gui;

        if (std::dynamic_pointer_cast<KnobColor>(knob)) {
            gui = std::make_shared<KnobGuiColor>(knob, _panel.get());
        } else if (std::dynamic_pointer_cast<KnobDouble>(knob)) {
            gui = std::make_shared<KnobGuiDouble>(knob, _panel.get());
        } else if (std::dynamic_pointer_cast<KnobInt>(knob)) {
            gui = std::make_shared<KnobGuiInt>(knob, _panel.get());
        }
        if (!gui) {
            return gui;
        }
        gui->initialize();

        QWidget* label = new QWidget(_page.get());
        QWidget* field = new QWidget(_page.get());
        QHBoxLayout* fieldLayout = new QHBoxLayout(field);

        fieldLayout->setContentsMargins(0, 0, 0, 0);
        gui->createGUI(field, label, 0, 0, fieldLayout, true, 0, std::vector<KnobIPtr>());
        _page->layout()->addWidget(field);
        _panel->guis[knob.get()] = gui;
        _guis.push_back(gui);

        return gui;
    }

    std::unique_ptr<QWidget> _page;
    std::unique_ptr<GlobalModeTestContainer> _panel;
    std::vector<KnobGuiPtr> _guis;
    NodePtr _transform;
    NodePtr _grade;
};

TEST_F(GlobalModeTargetingTest, HoverFindsTheKnobUnderThePointer)
{
    KnobGuiPtr translate = buildRow(_transform->getKnobByName(kTransformNodeParamTranslate));
    KnobGuiPtr multiply = buildRow(_grade->getKnobByName(kGradeParamMultiply));

    ASSERT_TRUE(bool(translate));
    ASSERT_TRUE(bool(multiply));

    KnobGuiValue* translateValue = dynamic_cast<KnobGuiValue*>(translate.get());
    KnobGuiColor* multiplyColor = dynamic_cast<KnobGuiColor*>(multiply.get());

    ASSERT_TRUE(translateValue && translateValue->getGlobalModeRowWidget());
    ASSERT_TRUE(multiplyColor && multiplyColor->getAdjustPanel());

    QWidget* translateRow = translateValue->getGlobalModeRowWidget();
    QWidget* insideTranslateRow = new QWidget(translateRow);

    EXPECT_EQ(translate.get(), globalModeKnobAt(translateRow));
    EXPECT_EQ(translate.get(), globalModeKnobAt(insideTranslateRow));
    EXPECT_EQ(multiply.get(), globalModeKnobAt(multiplyColor->getAdjustPanel()));
    EXPECT_EQ((KnobGui*)0, globalModeKnobAt(_page.get()));
    EXPECT_EQ((KnobGui*)0, globalModeKnobAt(0));

    EXPECT_TRUE(isGlobalModeKnob(translate.get()));
    EXPECT_TRUE(isGlobalModeKnob(multiply.get()));
}

TEST_F(GlobalModeTargetingTest, HoveredKnobEngagesTheRightTarget)
{
    const char* names[] = {
        kTransformNodeParamTranslate,
        kTransformNodeParamRotate,
        kTransformNodeParamScale,
        kTransformNodeParamSkewX,
        kTransformNodeParamSkewY,
        kTransformNodeParamCenter,
    };

    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        ASSERT_TRUE(bool(buildRow(_transform->getKnobByName(names[i]))));
    }

    KnobGuiPtr skewY = _panel->getKnobGui(_transform->getKnobByName(kTransformNodeParamSkewY));
    std::shared_ptr<TransformGroupGlobalModeTarget> group = std::dynamic_pointer_cast<TransformGroupGlobalModeTarget>(makeGlobalModeTarget(globalModeKnobAt(dynamic_cast<KnobGuiValue*>(skewY.get())->getGlobalModeRowWidget())));

    ASSERT_TRUE(bool(group));
    EXPECT_EQ(eGlobalModeGroupMemberSkew, group->getMember());

    KnobGuiPtr multiply = buildRow(_grade->getKnobByName(kGradeParamMultiply));
    std::shared_ptr<GlobalModeTarget> colour = makeGlobalModeTarget(multiply.get());

    ASSERT_TRUE(bool(colour));
    EXPECT_EQ(multiply.get(), colour->getKnobGui());
    EXPECT_FALSE(bool(std::dynamic_pointer_cast<ValueGlobalModeTargetBase>(colour)));
}

TEST_F(GlobalModeTargetingTest, CtrlDownStepsFromTheLastColourKnobOntoANumericKnob)
{
    // Every Grade knob in declaration order, filtered as orderedGlobalModeKnobs filters a panel.
    const std::vector<KnobIPtr>& knobs = _grade->getEffectInstance()->getKnobs();
    std::vector<KnobGui*> ordered;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobGuiPtr gui = buildRow(knobs[i]);

        if (gui && isGlobalModeKnob(gui.get())) {
            ordered.push_back(gui.get());
        }
    }

    int lastColour = -1;

    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (dynamic_cast<KnobGuiColor*>(ordered[i])) {
            lastColour = (int)i;
        }
    }
    ASSERT_GE(lastColour, 0);
    ASSERT_LT(lastColour + 1, (int)ordered.size());

    std::vector<ColorKnobInfo> infos;

    for (std::size_t i = 0; i < ordered.size(); ++i) {
        ColorKnobInfo info;

        info.scriptName = ordered[i]->getKnob()->getName();
        info.enabled = ordered[i]->getKnob()->isEnabled(0);
        info.visible = !ordered[i]->isSecretRecursive();
        infos.push_back(info);
    }

    const int next = stepColorKnobIndex(infos, lastColour, 1);

    ASSERT_EQ(lastColour + 1, next);
    EXPECT_FALSE(dynamic_cast<KnobGuiColor*>(ordered[next]));

    std::shared_ptr<ValueKnobGlobalModeTarget> value = std::dynamic_pointer_cast<ValueKnobGlobalModeTarget>(makeGlobalModeTarget(ordered[next]));

    ASSERT_TRUE(bool(value));
    EXPECT_EQ(ordered[next], value->getKnobGui());
    EXPECT_EQ(lastColour, stepColorKnobIndex(infos, next, -1));
}

TEST_F(GlobalModeTargetingTest, SteppingStaysOnTheEngagedKnobsPage)
{
    const std::vector<KnobIPtr>& knobs = _grade->getEffectInstance()->getKnobs();
    std::vector<KnobGui*> ordered;
    KnobGui* multiply = 0;
    KnobGui* lifeTime = 0;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobGuiPtr gui = buildRow(knobs[i]);

        if (!gui || !isGlobalModeKnob(gui.get())) {
            continue;
        }
        ordered.push_back(gui.get());
        if (knobs[i]->getName() == kGradeParamMultiply) {
            multiply = gui.get();
        } else if (knobs[i]->getName() == "nodeLifeTime") {
            lifeTime = gui.get();
        }
    }
    ASSERT_TRUE(multiply != 0);
    ASSERT_TRUE(lifeTime != 0);

    KnobPagePtr controls = multiply->getKnob()->getTopLevelPage();

    ASSERT_TRUE(bool(controls));
    ASSERT_NE(controls, lifeTime->getKnob()->getTopLevelPage());

    const std::vector<KnobGui*> onControls = restrictToPageOf(ordered, multiply);

    EXPECT_NE(onControls.end(), std::find(onControls.begin(), onControls.end(), multiply));
    EXPECT_EQ(onControls.end(), std::find(onControls.begin(), onControls.end(), lifeTime));
    for (std::size_t i = 0; i < onControls.size(); ++i) {
        EXPECT_EQ(controls, onControls[i]->getKnob()->getTopLevelPage());
    }

    const std::vector<KnobGui*> onNodePage = restrictToPageOf(ordered, lifeTime);

    EXPECT_NE(onNodePage.end(), std::find(onNodePage.begin(), onNodePage.end(), lifeTime));
    EXPECT_EQ(onNodePage.end(), std::find(onNodePage.begin(), onNodePage.end(), multiply));
}

} // namespace
