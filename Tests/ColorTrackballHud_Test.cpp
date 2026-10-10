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

#include <QApplication>
#include <QLabel>
#include <QString>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorTrackballHud.h"

NATRON_NAMESPACE_USING

namespace {

QLabel*
labelByName(const ColorTrackballHud& hud,
            const char* name)
{
    return hud.findChild<QLabel*>(QString::fromUtf8(name));
}

bool
isLegendLit(const ColorTrackballHud& hud,
            const char* name)
{
    QLabel* label = labelByName(hud, name);

    return label && label->property("trackballHudKeyLit").toBool();
}

ColorTrackballHudState
stateForC()
{
    ColorTrackballHudState state;

    state.nodeLabel = QString::fromUtf8("Grade1");
    state.knobLabel = QString::fromUtf8("white");
    state.knobIndex = 2;
    state.knobCount = 7;
    state.mode = eColorGlobalModeC;
    state.value.rgb[0] = state.value.rgb[1] = state.value.rgb[2] = 1.5;
    state.value.a = 1.0;
    state.value.hasAlpha = true;
    state.luma = 0.503;
    state.gear = 1.0;
    state.chromaValid = true;
    state.hueDegrees = 124.3;
    state.saturation = 0.082;

    return state;
}

} // namespace

TEST(ColorTrackballHud, ShowsTheTargetAndModeC)
{
    ColorTrackballHud hud;

    hud.setState(stateForC());

    const QString target = labelByName(hud, "ColorTrackballHudTarget")->text();

    EXPECT_TRUE(target.contains(QString::fromUtf8("Grade1")));
    EXPECT_TRUE(target.contains(QString::fromUtf8("white")));
    EXPECT_TRUE(target.contains(QString::fromUtf8("2")));
    EXPECT_TRUE(target.contains(QString::fromUtf8("7")));

    const QString values = labelByName(hud, "ColorTrackballHudValues")->text();

    EXPECT_TRUE(values.contains(QString::fromUtf8("124.3")));
    EXPECT_TRUE(values.contains(QString::fromUtf8("0.0820")));
    EXPECT_TRUE(values.contains(QString::fromUtf8("0.503")));
    EXPECT_TRUE(values.contains(QString::fromUtf8("1.000")));
    EXPECT_TRUE(values.contains(QString::fromUtf8("x1.00")));
}

TEST(ColorTrackballHud, LightsOnlyTheActiveModeKey)
{
    ColorTrackballHud hud;
    ColorTrackballHudState state = stateForC();

    state.mode = eColorGlobalModeC;
    hud.setState(state);
    EXPECT_TRUE(isLegendLit(hud, "ColorTrackballHudLegendC"));
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendT"));
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendV"));

    state.mode = eColorGlobalModeT;
    state.temp = 12.3;
    state.tint = -4.1;
    hud.setState(state);
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendC"));
    EXPECT_TRUE(isLegendLit(hud, "ColorTrackballHudLegendT"));
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendV"));

    const QString tValues = labelByName(hud, "ColorTrackballHudValues")->text();

    EXPECT_TRUE(tValues.contains(QString::fromUtf8("12.3")));
    EXPECT_TRUE(tValues.contains(QString::fromUtf8("-4.1")));

    state.mode = eColorGlobalModeV;
    hud.setState(state);
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendC"));
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendT"));
    EXPECT_TRUE(isLegendLit(hud, "ColorTrackballHudLegendV"));
}

TEST(ColorTrackballHud, ReportsAnInvalidChromaReadoutInsteadOfStaleNumbers)
{
    ColorTrackballHud hud;
    ColorTrackballHudState state = stateForC();

    state.chromaValid = false;
    hud.setState(state);

    const QString values = labelByName(hud, "ColorTrackballHudValues")->text();

    EXPECT_TRUE(values.contains(QString::fromUtf8("unavailable")));
    EXPECT_FALSE(values.contains(QString::fromUtf8("124.3")));
}

TEST(ColorTrackballHud, ShowsAndHidesTheEndOfListMessage)
{
    ColorTrackballHud hud;

    // isVisible() only means anything once the top-level widget itself has been shown: Qt
    // reports every descendant of an unshown top-level as not visible regardless of its own
    // explicit show/hide state.
    hud.show();
    QApplication::processEvents();

    ColorTrackballHudState state = stateForC();

    EXPECT_FALSE(labelByName(hud, "ColorTrackballHudEndOfList")->isVisible());

    state.endOfListMessage = QString::fromUtf8("Last colour knob.");
    hud.setState(state);

    QLabel* endOfList = labelByName(hud, "ColorTrackballHudEndOfList");

    EXPECT_TRUE(endOfList->isVisible());
    EXPECT_EQ(QString::fromUtf8("Last colour knob."), endOfList->text());

    state.endOfListMessage = QString();
    hud.setState(state);
    EXPECT_FALSE(endOfList->isVisible());
}

TEST(ColorTrackballHud, FrameOverADummyWidgetTracksAResize)
{
    QWidget window;

    window.resize(100, 80);

    std::unique_ptr<WindowHighlightFrame> frame(new WindowHighlightFrame(&window));

    EXPECT_EQ(window.size(), frame->size());
    EXPECT_EQ(0, frame->x());
    EXPECT_EQ(0, frame->y());

    window.resize(321, 233);
    EXPECT_EQ(window.size(), frame->size());
}
