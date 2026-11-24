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

#include <QApplication>
#include <QKeyEvent>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTest>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include "Gui/ComboBox.h"

NATRON_NAMESPACE_USING

namespace {

// "a", a disabled "b" and "c", showing "a" and holding the keyboard focus the wheel and arrow
// keys need.
bool
setUp(ComboBox& box)
{
    box.addItem(QString::fromUtf8("a"));
    box.addItem(QString::fromUtf8("b"));
    box.addItem(QString::fromUtf8("c"));
    box.disableItem(1);
    box.setCurrentIndex(0);
    box.show();
    box.activateWindow();
    if (!QTest::qWaitForWindowActive(&box)) {
        return false;
    }
    box.setFocus();

    return box.hasFocus();
}

void
sendKey(ComboBox& box,
        Qt::Key key)
{
    QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);

    QApplication::sendEvent(&box, &e);
}

// One wheel click: towards the first entry for a positive delta, towards the last for a negative one.
void
sendWheel(ComboBox& box,
          int delta)
{
    const QPointF pos(box.width() * 0.5, box.height() * 0.5);
    QWheelEvent e(pos, box.mapToGlobal(pos), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);

    QApplication::sendEvent(&box, &e);
}

} // anonymous namespace

TEST(ComboBox, ADisabledEntryCannotBeMadeCurrent)
{
    ComboBox box;
    ASSERT_TRUE(setUp(box));

    EXPECT_TRUE(box.isItemEnabled(0));
    EXPECT_FALSE(box.isItemEnabled(1));
    EXPECT_TRUE(box.isItemEnabled(2));
    EXPECT_FALSE(box.isItemEnabled(3));

    box.setCurrentIndex(1);
    EXPECT_EQ(0, box.activeIndex());

    box.enableItem(1);
    EXPECT_TRUE(box.isItemEnabled(1));
    box.setCurrentIndex(1);
    EXPECT_EQ(1, box.activeIndex());
}

TEST(ComboBox, TheArrowKeysStepOverADisabledEntry)
{
    ComboBox box;
    ASSERT_TRUE(setUp(box));

    sendKey(box, Qt::Key_Down);
    EXPECT_EQ(2, box.activeIndex());
    sendKey(box, Qt::Key_Up);
    EXPECT_EQ(0, box.activeIndex());
    // Past either end the keys wrap round, still over the disabled entry.
    sendKey(box, Qt::Key_Up);
    EXPECT_EQ(2, box.activeIndex());
    sendKey(box, Qt::Key_Down);
    EXPECT_EQ(0, box.activeIndex());
}

TEST(ComboBox, TheWheelStepsOverADisabledEntry)
{
    ComboBox box;
    ASSERT_TRUE(setUp(box));

    sendWheel(box, -120);
    EXPECT_EQ(2, box.activeIndex());
    sendWheel(box, 120);
    EXPECT_EQ(0, box.activeIndex());
}
