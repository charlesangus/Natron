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
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QLabel>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorTrackballHud.h"
#include "Gui/KnobGuiColor.h"

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

TEST(ColorTrackballHud, OffsetTempTintReadsInAxisUnitsNotMired)
{
    ColorTrackballHud hud;
    ColorTrackballHudState state = stateForC();

    state.mode = eColorGlobalModeT;
    state.role = eColorKnobRoleOffset;
    state.temp = 0.0123;
    state.tint = -0.0045;
    hud.setState(state);

    const QString values = labelByName(hud, "ColorTrackballHudValues")->text();

    EXPECT_FALSE(values.contains(QString::fromUtf8("mired")));
    EXPECT_TRUE(values.contains(QString::fromUtf8("0.0123")));
    EXPECT_TRUE(values.contains(QString::fromUtf8("-0.0045")));

    state.role = eColorKnobRoleGain;
    state.temp = 12.3;
    hud.setState(state);
    EXPECT_TRUE(labelByName(hud, "ColorTrackballHudValues")->text().contains(QString::fromUtf8("mired")));
}

TEST(ColorTrackballHud, ModeKeyNames)
{
    EXPECT_EQ(QString::fromUtf8("C"), colorGlobalModeKeyName(eColorGlobalModeC));
    EXPECT_EQ(QString::fromUtf8("S"), colorGlobalModeKeyName(eColorGlobalModeS));
    EXPECT_EQ(QString::fromUtf8("T"), colorGlobalModeKeyName(eColorGlobalModeT));
    EXPECT_EQ(QString::fromUtf8("V"), colorGlobalModeKeyName(eColorGlobalModeV));
}

TEST(ColorTrackballHud, LightsOnlySForModeS)
{
    ColorTrackballHud hud;
    ColorTrackballHudState state = stateForC();

    state.mode = eColorGlobalModeS;
    hud.setState(state);

    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendC"));
    EXPECT_TRUE(isLegendLit(hud, "ColorTrackballHudLegendS"));
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendT"));
    EXPECT_FALSE(isLegendLit(hud, "ColorTrackballHudLegendV"));

    const QString values = labelByName(hud, "ColorTrackballHudValues")->text();

    EXPECT_TRUE(values.contains(QString::fromUtf8("124.3")));
    EXPECT_TRUE(values.contains(QString::fromUtf8("0.0820")));
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

    // A hidden widget's resize event stays pending until it is shown, and a top-level window's
    // resize comes back asynchronously from the window system, so the window is shown and the
    // event loop runs until the frame has caught up.
    window.show();
    window.resize(321, 233);

    QElapsedTimer timer;
    timer.start();
    while ((frame->size() != window.size()) && (timer.elapsed() < 2000)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    EXPECT_EQ(QSize(321, 233), window.size());
    EXPECT_EQ(window.size(), frame->size());
}

TEST(ColorTrackballHud, AttachPutsTheHudRightAfterTheChannelLinesAndDetachRemovesIt)
{
    // A stand-in for a knob's below-row column: the row, the channel lines, then the adjust
    // panel, exactly the order KnobGuiValue/KnobGuiColor build it in, but with none of them
    // backed by a real knob.
    QWidget column;
    QVBoxLayout* columnLayout = new QVBoxLayout(&column);
    QWidget row;
    QWidget channelLines;
    QWidget panel;
    ColorTrackballHud hud;

    columnLayout->addWidget(&row);
    columnLayout->addWidget(&channelLines);
    columnLayout->addWidget(&panel);

    attachColorGlobalModeHud(columnLayout, &channelLines, &hud);

    EXPECT_EQ(2, columnLayout->indexOf(&hud));
    EXPECT_EQ(3, columnLayout->indexOf(&panel));
    EXPECT_EQ(&column, hud.parentWidget());

    detachColorGlobalModeHud(&hud);

    EXPECT_EQ(-1, columnLayout->indexOf(&hud));
    EXPECT_TRUE(hud.parentWidget() == 0);
}

TEST(ColorTrackballHud, AttachWithNoChannelLinesGoesRightAfterTheRow)
{
    QWidget column;
    QVBoxLayout* columnLayout = new QVBoxLayout(&column);
    QWidget row;
    QWidget panel;
    ColorTrackballHud hud;

    columnLayout->addWidget(&row);
    columnLayout->addWidget(&panel);

    attachColorGlobalModeHud(columnLayout, 0, &hud);

    EXPECT_EQ(1, columnLayout->indexOf(&hud));
    EXPECT_EQ(2, columnLayout->indexOf(&panel));
}

TEST(ColorTrackballHud, AttachTwiceMovesTheHudInsteadOfDuplicatingIt)
{
    QWidget columnA;
    QVBoxLayout* columnALayout = new QVBoxLayout(&columnA);
    QWidget rowA;
    QWidget channelLinesA;

    columnALayout->addWidget(&rowA);
    columnALayout->addWidget(&channelLinesA);

    QWidget columnB;
    QVBoxLayout* columnBLayout = new QVBoxLayout(&columnB);
    QWidget rowB;
    QWidget channelLinesB;

    columnBLayout->addWidget(&rowB);
    columnBLayout->addWidget(&channelLinesB);

    ColorTrackballHud hud;

    attachColorGlobalModeHud(columnALayout, &channelLinesA, &hud);
    EXPECT_EQ(2, columnALayout->indexOf(&hud));

    attachColorGlobalModeHud(columnBLayout, &channelLinesB, &hud);

    EXPECT_EQ(-1, columnALayout->indexOf(&hud));
    EXPECT_EQ(2, columnBLayout->indexOf(&hud));
    EXPECT_EQ(&columnB, hud.parentWidget());
}

TEST(ColorTrackballHud, FollowPositionIsBelowRightAndFlipsAtTheScreenEdges)
{
    const QRect screen(0, 0, 800, 600);
    const QSize size(200, 100);
    const QPoint offset(16, 16);

    EXPECT_EQ(QPoint(116, 116), colorTrackballHudFollowPosition(QPoint(100, 100), size, screen, offset));
    // Within 10 px of the right and bottom edges: left of and above the cursor.
    EXPECT_EQ(QPoint(790 - 16 - 200, 590 - 16 - 100), colorTrackballHudFollowPosition(QPoint(790, 590), size, screen, offset));
    // Only the axis that runs out flips.
    EXPECT_EQ(QPoint(790 - 16 - 200, 116), colorTrackballHudFollowPosition(QPoint(790, 100), size, screen, offset));
    EXPECT_EQ(QPoint(116, 590 - 16 - 100), colorTrackballHudFollowPosition(QPoint(100, 590), size, screen, offset));
    // Exactly fitting against the edge does not flip.
    EXPECT_EQ(QPoint(600, 116), colorTrackballHudFollowPosition(QPoint(584, 100), size, screen, offset));
    // A screen that does not start at the origin.
    EXPECT_EQ(QPoint(1936, 21), colorTrackballHudFollowPosition(QPoint(1920, 5), size, QRect(1920, 0, 1920, 1080), offset));
    // Too big for either side: kept on screen.
    EXPECT_EQ(QPoint(0, 0), colorTrackballHudFollowPosition(QPoint(400, 300), QSize(900, 700), screen, offset));
    EXPECT_EQ(QPoint(116, 116), colorTrackballHudFollowPosition(QPoint(100, 100), size, QRect(), offset));
}

TEST(ColorTrackballHud, FollowingMakesItAClickThroughToolWindowOutOfTheColumn)
{
    QWidget column;
    QVBoxLayout* columnLayout = new QVBoxLayout(&column);
    QWidget row;
    QWidget channelLines;
    ColorTrackballHud hud;

    columnLayout->addWidget(&row);
    columnLayout->addWidget(&channelLines);
    attachColorGlobalModeHud(columnLayout, &channelLines, &hud);
    ASSERT_EQ(2, columnLayout->indexOf(&hud));

    hud.setFollowCursor(true);

    const Qt::WindowFlags flags = hud.windowFlags();

    EXPECT_TRUE(hud.isFollowingCursor());
    EXPECT_EQ(-1, columnLayout->indexOf(&hud));
    EXPECT_TRUE(hud.parentWidget() == 0);
    EXPECT_TRUE(hud.isWindow());
    EXPECT_TRUE(hud.isHidden());
    EXPECT_EQ((int)Qt::Tool, (int)(flags & Qt::WindowType_Mask));
    EXPECT_TRUE(flags.testFlag(Qt::FramelessWindowHint));
    EXPECT_TRUE(flags.testFlag(Qt::WindowStaysOnTopHint));
    EXPECT_TRUE(flags.testFlag(Qt::WindowTransparentForInput));
    EXPECT_TRUE(flags.testFlag(Qt::WindowDoesNotAcceptFocus));
    EXPECT_TRUE(hud.testAttribute(Qt::WA_ShowWithoutActivating));
    EXPECT_TRUE(hud.testAttribute(Qt::WA_TransparentForMouseEvents));
}

TEST(ColorTrackballHud, FollowingHudSitsAtTheOffsetAndFlipsNearTheEdges)
{
    QScreen* screen = QGuiApplication::primaryScreen();

    ASSERT_TRUE(screen != 0);

    const QRect g = screen->geometry();
    ColorTrackballHud hud;

    hud.setState(stateForC());
    hud.setFollowCursor(true);
    ASSERT_LE(g.left() + 116 + hud.width(), g.left() + g.width());
    ASSERT_LE(g.top() + 116 + hud.height(), g.top() + g.height());

    const QPoint near = g.topLeft() + QPoint(100, 100);

    hud.followCursorTo(near);
    EXPECT_EQ(near + QPoint(16, 16), hud.pos());

    const QPoint corner(g.left() + g.width() - 10, g.top() + g.height() - 10);

    hud.followCursorTo(corner);
    EXPECT_LT(hud.geometry().right(), corner.x());
    EXPECT_LT(hud.geometry().bottom(), corner.y());
    EXPECT_GE(hud.geometry().left(), g.left());
    EXPECT_GE(hud.geometry().top(), g.top());

    // A new state that resizes it is placed by the same cursor again.
    ColorTrackballHudState longer = stateForC();

    longer.endOfListMessage = QString::fromUtf8("Last colour knob.");
    hud.setState(longer);
    EXPECT_LT(hud.geometry().right(), corner.x());
    EXPECT_LT(hud.geometry().bottom(), corner.y());
}

TEST(ColorTrackballHud, FollowCursorToIsIgnoredWhenNotFollowing)
{
    ColorTrackballHud hud;

    hud.move(5, 7);
    hud.followCursorTo(QPoint(100, 100));
    EXPECT_EQ(QPoint(5, 7), hud.pos());
}

TEST(ColorTrackballHud, AfterFollowingItAttachesUnderTheKnobAsBefore)
{
    QWidget column;
    QVBoxLayout* columnLayout = new QVBoxLayout(&column);
    QWidget row;
    QWidget channelLines;
    QWidget panel;
    ColorTrackballHud hud;

    columnLayout->addWidget(&row);
    columnLayout->addWidget(&channelLines);
    columnLayout->addWidget(&panel);

    const Qt::WindowFlags beforeFollowing = hud.windowFlags();
    const bool showedWithoutActivating = hud.testAttribute(Qt::WA_ShowWithoutActivating);

    hud.setFollowCursor(true);
    hud.setFollowCursor(false);

    EXPECT_FALSE(hud.isFollowingCursor());
    // A parentless widget is always a window to Qt; every other flag is the one it had before.
    EXPECT_EQ(beforeFollowing | Qt::Window, hud.windowFlags());
    EXPECT_FALSE(hud.windowFlags().testFlag(Qt::WindowTransparentForInput));
    EXPECT_FALSE(hud.testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_EQ(showedWithoutActivating, hud.testAttribute(Qt::WA_ShowWithoutActivating));

    attachColorGlobalModeHud(columnLayout, &channelLines, &hud);

    EXPECT_EQ(2, columnLayout->indexOf(&hud));
    EXPECT_EQ(&column, hud.parentWidget());
    EXPECT_FALSE(hud.isWindow());
    EXPECT_EQ(beforeFollowing & ~Qt::WindowType_Mask, hud.windowFlags() & ~Qt::WindowType_Mask);
}
