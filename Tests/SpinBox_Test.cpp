// ***** BEGIN LICENSE BLOCK *****
// This file is part of Natron <https://natrongithub.github.io/>,
// (C) 2018-2023 The Natron developers
// (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
//
// Natron is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// Natron is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
// ***** END LICENSE BLOCK *****

#include <Python.h>

#include "Global/Macros.h"

#include <vector>

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QString>

#include <gtest/gtest.h>

#include "Gui/SpinBox.h"

NATRON_NAMESPACE_USING

namespace {

void
press(SpinBox* box,
      int key,
      const QString& text = QString())
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, text);

    QApplication::sendEvent(box, &event);
}

void
leave(SpinBox* box)
{
    QFocusEvent event(QEvent::FocusOut);

    QApplication::sendEvent(box, &event);
}

// Records what the box commits.
struct Committed {
    std::vector<double> values;

    explicit Committed(SpinBox* box)
    {
        QObject::connect(box, &SpinBox::valueChanged, box, [this](double d) { values.push_back(d); });
    }
};

} // namespace

TEST(SpinBox, AHugeValueIsShownRoundedAndReadInFull)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);

    box.setValue(1234567.);
    EXPECT_EQ(QString::fromUtf8("1.2e6"), box.text());
    EXPECT_DOUBLE_EQ(1234567., box.value());
    EXPECT_DOUBLE_EQ(1234567., box.getLastValidValueBeforeValidation());
}

TEST(SpinBox, AnIntBoxReadsAHugeValueInFull)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeInt);

    box.setValue(1234567);
    EXPECT_EQ(QString::fromUtf8("1.2e6"), box.text());
    EXPECT_DOUBLE_EQ(1234567., box.value());
}

TEST(SpinBox, TheExponentKeepsItsZeroes)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);

    box.setValue(1.2e100);
    EXPECT_EQ(QString::fromUtf8("1.2e100"), box.text());
    EXPECT_DOUBLE_EQ(1.2e100, box.value());

    box.setValue(1.5e20);
    EXPECT_EQ(QString::fromUtf8("1.5e20"), box.text());
}

TEST(SpinBox, AnArrowStepsFromTheFullValue)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);
    Committed committed(&box);

    box.setValue(1234567.);
    press(&box, Qt::Key_Up);
    ASSERT_EQ(1u, committed.values.size());
    EXPECT_DOUBLE_EQ(1234568., committed.values[0]);
    EXPECT_DOUBLE_EQ(1234568., box.value());
}

TEST(SpinBox, LeavingOrMovingInTheFieldCommitsNothingOfAHugeValue)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);
    Committed committed(&box);

    box.setValue(1234567.);
    press(&box, Qt::Key_Left);
    press(&box, Qt::Key_Home);
    leave(&box);
    EXPECT_TRUE(committed.values.empty());
    EXPECT_DOUBLE_EQ(1234567., box.value());
}

TEST(SpinBox, UndoingAnEditOfAHugeValueCommitsNothing)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);
    Committed committed(&box);

    box.setValue(1234567.);
    box.setCursorPosition(box.text().size());
    press(&box, Qt::Key_Backspace);
    EXPECT_EQ(QString::fromUtf8("1.2e"), box.text());
    press(&box, Qt::Key_6, QString::fromUtf8("6"));
    EXPECT_EQ(QString::fromUtf8("1.2e6"), box.text());
    leave(&box);
    EXPECT_TRUE(committed.values.empty());
    EXPECT_DOUBLE_EQ(1234567., box.value());
}

TEST(SpinBox, ReturnOnAnUneditedHugeValueCommitsItInFull)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);
    Committed committed(&box);

    box.setValue(1234567.);
    press(&box, Qt::Key_Return);
    for (double value : committed.values) {
        EXPECT_DOUBLE_EQ(1234567., value);
    }
    EXPECT_DOUBLE_EQ(1234567., box.value());
}

TEST(SpinBox, TypingOverAHugeValueCommitsWhatWasTyped)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);
    Committed committed(&box);

    box.setValue(1234567.);
    box.selectAll();
    press(&box, Qt::Key_4, QString::fromUtf8("4"));
    EXPECT_DOUBLE_EQ(4., box.value());
    leave(&box);
    ASSERT_EQ(1u, committed.values.size());
    EXPECT_DOUBLE_EQ(4., committed.values[0]);
}

TEST(SpinBox, ASmallValueIsShownInFull)
{
    SpinBox box(nullptr, SpinBox::eSpinBoxTypeDouble);

    box.setValue(999999.);
    EXPECT_DOUBLE_EQ(999999., box.text().toDouble());
    EXPECT_DOUBLE_EQ(999999., box.value());
}
