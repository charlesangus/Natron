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

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "Engine/Settings.h"
#include "Gui/ActionShortcuts.h"

NATRON_NAMESPACE_USING

namespace {

MouseBinding
binding(Qt::KeyboardModifiers modifiers,
        Qt::MouseButton button)
{
    MouseBinding b;
    b.modifiers = modifiers;
    b.button = button;

    return b;
}

void
expectBindings(const std::vector<MouseBinding>& expected,
               const std::vector<MouseBinding>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(int(expected[i].modifiers), int(actual[i].modifiers)) << "binding " << i;
        EXPECT_EQ(int(expected[i].button), int(actual[i].button)) << "binding " << i;
    }
}

// The 3D viewer's mouse actions and their gestures, as GuiApplicationManager registers them.
struct Viewer3DAction {
    const char* id;
    MouseGestureEnum gesture;
};

const Viewer3DAction kViewer3DActions[] = {
    { kShortcutIDMouseViewer3DTumble, eMouseGestureDrag },
    { kShortcutIDMouseViewer3DPan, eMouseGestureDrag },
    { kShortcutIDMouseViewer3DDolly, eMouseGestureDrag },
    { kShortcutIDMouseViewer3DDollyWheel, eMouseGestureWheel },
    { kShortcutIDMouseViewer3DSelect, eMouseGestureClick },
    { kShortcutIDMouseViewer3DAddToSelection, eMouseGestureClick },
    { kShortcutIDMouseViewer3DToggleSelection, eMouseGestureClick },
    { kShortcutIDMouseViewer3DMarquee, eMouseGestureDrag },
};

class ShortcutSettingsTest
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(_dir.isValid());
        _path = _dir.filePath(QString::fromUtf8("shortcuts.ini"));
    }

    void write(const BoundAction& action)
    {
        QSettings settings(_path, QSettings::IniFormat);
        settings.beginGroup(QString::fromUtf8(kShortcutGroupViewer3D));
        writeShortcutSettings(settings, action.actionID, action);
        settings.endGroup();
        settings.sync();
        ASSERT_EQ(QSettings::NoError, settings.status());
    }

    void read(BoundAction* action)
    {
        QSettings settings(_path, QSettings::IniFormat);
        settings.beginGroup(QString::fromUtf8(kShortcutGroupViewer3D));
        readShortcutSettings(settings, action->actionID, action);
        settings.endGroup();
    }

    // A tumble action bound to the Nuke preset, as a fresh start makes it.
    static void makeTumble(MouseAction* action)
    {
        action->grouping = QString::fromUtf8(kShortcutGroupViewer3D);
        action->actionID = QString::fromUtf8(kShortcutIDMouseViewer3DTumble);
        action->gesture = eMouseGestureDrag;
        action->editable = true;
        std::vector<MouseBinding> bindings;
        ASSERT_TRUE(getViewer3DNavigationPresetBindings(Settings::eViewer3DNavigationNuke, action->actionID, &bindings));
        action->setBindings(bindings);
    }

    QTemporaryDir _dir;
    QString _path;
};
} // anonymous namespace

TEST_F(ShortcutSettingsTest, ReboundMouseActionRoundTrips)
{
    MouseAction rebound;
    makeTumble(&rebound);
    const std::vector<MouseBinding> bindings = {
        binding(Qt::AltModifier, Qt::LeftButton),
        binding(kShortcutModifierSpace | Qt::ShiftModifier, Qt::MiddleButton),
    };
    rebound.setBindings(bindings);
    write(rebound);

    MouseAction restored;
    makeTumble(&restored);
    expectBindings({ binding(Qt::ControlModifier, Qt::LeftButton) }, restored.getBindings());
    read(&restored);
    expectBindings(bindings, restored.getBindings());

    EXPECT_TRUE(restored.matches(Qt::AltModifier, Qt::LeftButton));
    EXPECT_FALSE(restored.matches(Qt::ControlModifier, Qt::LeftButton));
    EXPECT_FALSE(restored.matches(Qt::AltModifier, Qt::MiddleButton));
    // Space is a modifier of its own; Shift alone is not the binding.
    EXPECT_TRUE(restored.matches(mouseBindingModifiers(Qt::ShiftModifier, true), Qt::MiddleButton));
    EXPECT_FALSE(restored.matches(mouseBindingModifiers(Qt::ShiftModifier, false), Qt::MiddleButton));
    // Modifiers outside Control, Alt, Shift, Meta and Space do not count.
    EXPECT_TRUE(restored.matches(Qt::AltModifier | Qt::KeypadModifier, Qt::LeftButton));
}

TEST_F(ShortcutSettingsTest, ClearedMouseActionStaysCleared)
{
    MouseAction cleared;
    makeTumble(&cleared);
    cleared.setBindings(std::vector<MouseBinding>());
    write(cleared);

    MouseAction restored;
    makeTumble(&restored);
    read(&restored);
    EXPECT_TRUE(restored.getBindings().empty());
    EXPECT_FALSE(restored.matches(Qt::ControlModifier, Qt::LeftButton));
}

TEST_F(ShortcutSettingsTest, UnsavedMouseActionKeepsItsBindings)
{
    MouseAction restored;
    makeTumble(&restored);
    read(&restored);
    expectBindings({ binding(Qt::ControlModifier, Qt::LeftButton) }, restored.getBindings());
}

TEST_F(ShortcutSettingsTest, KeyActionRoundTrips)
{
    KeyBoundAction frame;
    frame.actionID = QString::fromUtf8(kShortcutIDActionViewer3DFrameSelection);
    frame.modifiers = { Qt::ShiftModifier };
    frame.currentShortcut = { Qt::Key_F };
    write(frame);

    KeyBoundAction restored;
    restored.actionID = frame.actionID;
    restored.modifiers = { Qt::NoModifier };
    restored.currentShortcut = { Qt::Key_A };
    read(&restored);
    ASSERT_EQ(std::size_t(1), restored.modifiers.size());
    EXPECT_EQ(int(Qt::ShiftModifier), int(restored.modifiers.front()));
    ASSERT_EQ(std::size_t(1), restored.currentShortcut.size());
    EXPECT_EQ(int(Qt::Key_F), int(restored.currentShortcut.front()));
}

TEST(Viewer3DNavigationPresetTest, WheelBindingIgnoresTheButton)
{
    MouseAction wheel;
    wheel.gesture = eMouseGestureWheel;
    wheel.setBindings({ binding(Qt::NoModifier, Qt::LeftButton) });
    // A wheel action keeps no button.
    EXPECT_EQ(int(Qt::NoButton), int(wheel.getBindings().front().button));
    EXPECT_TRUE(wheel.matches(Qt::NoModifier, Qt::NoButton));
    EXPECT_FALSE(wheel.matches(Qt::ControlModifier, Qt::NoButton));
}

TEST(Viewer3DNavigationPresetTest, PresetsAreUnambiguous)
{
    for (int preset = Settings::eViewer3DNavigationNuke; preset < Settings::eViewer3DNavigationCustom; ++preset) {
        std::vector<std::vector<MouseBinding>> bindings;
        for (const Viewer3DAction& action : kViewer3DActions) {
            std::vector<MouseBinding> b;
            EXPECT_TRUE(getViewer3DNavigationPresetBindings(preset, QString::fromUtf8(action.id), &b)) << "preset " << preset << ", " << action.id;
            EXPECT_FALSE(b.empty()) << "preset " << preset << ", " << action.id;
            bindings.push_back(b);
        }
        const std::size_t n = bindings.size();
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                if (!mouseGesturesOverlap(kViewer3DActions[i].gesture, kViewer3DActions[j].gesture)) {
                    continue;
                }
                const bool wheel = (kViewer3DActions[i].gesture == eMouseGestureWheel);
                for (const MouseBinding& a : bindings[i]) {
                    for (const MouseBinding& b : bindings[j]) {
                        EXPECT_FALSE((a.modifiers == b.modifiers) && (wheel || (a.button == b.button)))
                            << "preset " << preset << ": " << kViewer3DActions[i].id << " and " << kViewer3DActions[j].id
                            << " share " << mouseBindingToString(a, wheel).toStdString();
                    }
                }
            }
        }
    }
    // Custom is whatever the shortcut editor made of the bindings.
    std::vector<MouseBinding> custom;
    EXPECT_FALSE(getViewer3DNavigationPresetBindings(Settings::eViewer3DNavigationCustom, QString::fromUtf8(kShortcutIDMouseViewer3DTumble), &custom));
}

TEST(Viewer3DNavigationPresetTest, PresetsFollowTheirApplications)
{
    std::vector<MouseBinding> b;
    ASSERT_TRUE(getViewer3DNavigationPresetBindings(Settings::eViewer3DNavigationNuke, QString::fromUtf8(kShortcutIDMouseViewer3DTumble), &b));
    expectBindings({ binding(Qt::ControlModifier, Qt::LeftButton) }, b);
    ASSERT_TRUE(getViewer3DNavigationPresetBindings(Settings::eViewer3DNavigationMaya, QString::fromUtf8(kShortcutIDMouseViewer3DDolly), &b));
    expectBindings({ binding(Qt::AltModifier, Qt::RightButton) }, b);
    ASSERT_TRUE(getViewer3DNavigationPresetBindings(Settings::eViewer3DNavigationBlender, QString::fromUtf8(kShortcutIDMouseViewer3DTumble), &b));
    expectBindings({ binding(Qt::NoModifier, Qt::MiddleButton) }, b);
    ASSERT_TRUE(getViewer3DNavigationPresetBindings(Settings::eViewer3DNavigationHoudini, QString::fromUtf8(kShortcutIDMouseViewer3DTumble), &b));
    expectBindings({ binding(kShortcutModifierSpace, Qt::LeftButton), binding(Qt::AltModifier, Qt::LeftButton) }, b);
    EXPECT_EQ(QString::fromUtf8("Space+LeftButton"), mouseBindingToString(b.front(), false));
}
