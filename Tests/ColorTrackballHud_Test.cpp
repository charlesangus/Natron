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
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorTrackballHud.h"

NATRON_NAMESPACE_USING

namespace {
ColorTrackballHudState
stateFor(ColorGlobalModeEnum mode)
{
    ColorTrackballHudState state;

    state.knobLabel = QString::fromUtf8("Gain");
    state.mode = mode;
    state.gear = 1.0;

    return state;
}
} // namespace

TEST(ColorTrackballHud, ModeKeyNames)
{
    EXPECT_EQ(QString::fromUtf8("C"), colorGlobalModeKeyName(eColorGlobalModeC));
    EXPECT_EQ(QString::fromUtf8("S"), colorGlobalModeKeyName(eColorGlobalModeS));
    EXPECT_EQ(QString::fromUtf8("T"), colorGlobalModeKeyName(eColorGlobalModeT));
    EXPECT_EQ(QString::fromUtf8("V"), colorGlobalModeKeyName(eColorGlobalModeV));
}

TEST(ColorTrackballHud, BlockShowsTheKnobNameKeysAndHintsInOrder)
{
    ColorTrackballHudBlock block;

    block.setState(stateFor(eColorGlobalModeS));

    const QStringList lines = block.lines();

    ASSERT_EQ(4, lines.size());
    EXPECT_EQ(QString::fromUtf8("Gain"), lines[0]);
    EXPECT_EQ(QString::fromUtf8("C S T V"), lines[1]);
    EXPECT_EQ(QString::fromUtf8("Ctrl 10x slower   Shift 4x faster   Ctrl+Shift 100x slower"), lines[2]);
    EXPECT_EQ(QString::fromUtf8("Esc/Enter: accept   Ctrl+up/down: switch knob"), lines[3]);
    EXPECT_EQ(eColorGlobalModeS, block.litKey());
    EXPECT_EQ(-1, block.litGear());
    EXPECT_GT(block.width(), 0);
    EXPECT_GT(block.height(), 0);
}

TEST(ColorTrackballHud, BlockAddsTheChromaHintInCAndTheEndOfListMessage)
{
    ColorTrackballHudBlock block;
    ColorTrackballHudState state = stateFor(eColorGlobalModeC);

    block.setState(state);

    const int plainHeight = block.height();

    ASSERT_EQ(5, block.lines().size());
    EXPECT_EQ(QString::fromUtf8("Drag toward a hue to push the colour that way"), block.lines()[4]);

    state.endOfListMessage = QString::fromUtf8("Last colour knob.");
    block.setState(state);
    ASSERT_EQ(6, block.lines().size());
    EXPECT_EQ(state.endOfListMessage, block.lines()[5]);
    EXPECT_GT(block.height(), plainHeight);

    state.mode = eColorGlobalModeV;
    state.endOfListMessage.clear();
    block.setState(state);
    EXPECT_EQ(4, block.lines().size());
    EXPECT_EQ(eColorGlobalModeV, block.litKey());
}

TEST(ColorTrackballHud, BlockLightsTheHeldGear)
{
    EXPECT_EQ(-1, colorTrackballHudLitGear(gear(false, false)));
    EXPECT_EQ(0, colorTrackballHudLitGear(gear(true, false)));
    EXPECT_EQ(1, colorTrackballHudLitGear(gear(false, true)));
    EXPECT_EQ(2, colorTrackballHudLitGear(gear(true, true)));

    ColorTrackballHudBlock block;
    ColorTrackballHudState state = stateFor(eColorGlobalModeT);

    state.gear = gear(true, false);
    block.setState(state);
    EXPECT_EQ(0, block.litGear());
}

TEST(ColorTrackballHud, PillIsSizedToItsText)
{
    ColorTrackballPill shortPill;
    ColorTrackballPill longPill;

    shortPill.setText(QString::fromUtf8("+Hue"));
    longPill.setText(QString::fromUtf8("Less green"));
    EXPECT_EQ(QString::fromUtf8("+Hue"), shortPill.text());
    EXPECT_GT(shortPill.width(), 0);
    EXPECT_GT(longPill.width(), shortPill.width());
    EXPECT_EQ(shortPill.height(), longPill.height());
    EXPECT_TRUE(shortPill.testAttribute(Qt::WA_TransparentForMouseEvents));
}

TEST(ColorTrackballHud, BlockWindowIsAClickThroughToolWindow)
{
    ColorTrackballHudBlock block(nullptr, colorTrackballFollowCursorWindowFlags());
    const Qt::WindowFlags flags = block.windowFlags();

    EXPECT_TRUE(block.isWindow());
    EXPECT_TRUE(block.isHidden());
    EXPECT_EQ((int)Qt::Tool, (int)(flags & Qt::WindowType_Mask));
    EXPECT_TRUE(flags.testFlag(Qt::FramelessWindowHint));
    EXPECT_TRUE(flags.testFlag(Qt::WindowStaysOnTopHint));
    EXPECT_TRUE(flags.testFlag(Qt::WindowTransparentForInput));
    EXPECT_TRUE(flags.testFlag(Qt::WindowDoesNotAcceptFocus));
    EXPECT_TRUE(block.testAttribute(Qt::WA_ShowWithoutActivating));
    EXPECT_TRUE(block.testAttribute(Qt::WA_TransparentForMouseEvents));
    // Masked to its rounded panel, so its corners never paint black without a compositor.
    EXPECT_FALSE(block.mask().isEmpty());
    EXPECT_FALSE(block.mask().contains(QPoint(0, 0)));
    EXPECT_TRUE(block.mask().contains(QPoint(block.width() / 2, block.height() / 2)));
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

namespace {
class PaintCounter
    : public QObject {
public:
    int count = 0;

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() == QEvent::Paint) {
            ++count;
        }

        return false;
    }
};
}

TEST(ColorTrackballHud, ANewStateAndAMoveLeaveTheBlockWindowRepainted)
{
    ColorTrackballHudBlock block(nullptr, colorTrackballFollowCursorWindowFlags());
    PaintCounter counter;

    block.setState(stateFor(eColorGlobalModeS));
    block.show();
    QElapsedTimer timer;
    timer.start();
    while (!block.isVisible() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    block.installEventFilter(&counter);

    ColorTrackballHudState longer = stateFor(eColorGlobalModeS);

    longer.endOfListMessage = QString::fromUtf8("Last colour knob.");
    block.setState(longer);
    block.followCursorTo(QPoint(120, 130));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_GE(counter.count, 1);
    block.hide();
}

TEST(ColorTrackballHud, FollowingBlockSitsAtTheOffsetAndFlipsNearTheEdges)
{
    QScreen* screen = QGuiApplication::primaryScreen();

    ASSERT_TRUE(screen != 0);

    const QRect g = screen->geometry();
    ColorTrackballHudBlock block(nullptr, colorTrackballFollowCursorWindowFlags());

    block.setState(stateFor(eColorGlobalModeS));
    ASSERT_LE(g.left() + 116 + block.width(), g.left() + g.width());
    ASSERT_LE(g.top() + 116 + block.height(), g.top() + g.height());

    const QPoint near = g.topLeft() + QPoint(100, 100);

    block.followCursorTo(near);
    EXPECT_EQ(near + QPoint(16, 16), block.pos());

    const QPoint corner(g.left() + g.width() - 10, g.top() + g.height() - 10);

    block.followCursorTo(corner);
    EXPECT_LT(block.geometry().right(), corner.x());
    EXPECT_LT(block.geometry().bottom(), corner.y());
    EXPECT_GE(block.geometry().left(), g.left());
    EXPECT_GE(block.geometry().top(), g.top());

    // A new state that resizes it is placed by the same cursor again.
    ColorTrackballHudState longer = stateFor(eColorGlobalModeC);

    longer.endOfListMessage = QString::fromUtf8("Last colour knob.");
    block.setState(longer);
    EXPECT_LT(block.geometry().right(), corner.x());
    EXPECT_LT(block.geometry().bottom(), corner.y());
}
