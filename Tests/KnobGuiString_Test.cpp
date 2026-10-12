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

#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QFont>

#include <gtest/gtest.h>

#include "Engine/KnobTypes.h"
#include "Gui/KnobGuiString.h"

NATRON_NAMESPACE_USING

namespace {

QString
labelWithoutColour()
{
    return KnobGuiString::decorateTextWithFontTag(QString::fromUtf8("Arial"), 9, QColor(),
                                                  QString::fromUtf8("hello"));
}

int
countOccurrences(const QString& text,
                 const char* needle)
{
    return text.count(QString::fromUtf8(needle));
}

} // namespace

TEST(KnobGuiString, AFontTagWithoutAColourHasNoColourAttribute)
{
    const QString label = labelWithoutColour();
    int size = 0;
    QString colorName;
    QString family;

    ASSERT_TRUE(KnobGuiString::parseFontTagAttributes(label, &size, &colorName, &family));
    EXPECT_EQ(9, size);
    EXPECT_EQ(QString::fromUtf8("Arial"), family);
    EXPECT_TRUE(colorName.isEmpty());
}

TEST(KnobGuiString, PickingAColourKeepsTheFontTagParseable)
{
    QString label = labelWithoutColour();
    const QColor picked(255, 0, 0);

    KnobGuiString::findReplaceColorName(label, picked);

    EXPECT_TRUE(label.startsWith(QString::fromUtf8(kFontSizeTag))) << label.toStdString();
    int size = 0;
    QString colorName;
    QString family;
    ASSERT_TRUE(KnobGuiString::parseFontTagAttributes(label, &size, &colorName, &family));
    EXPECT_EQ(9, size);
    EXPECT_EQ(QString::fromUtf8("Arial"), family);
    EXPECT_EQ(picked, QColor(colorName));
    EXPECT_FALSE(KnobGuiString::isAutomaticLabelFontColorName(colorName));

    EXPECT_EQ(KnobGuiString::decorateTextWithFontTag(QString::fromUtf8("Arial"), 9, picked, QString::fromUtf8("hello")), label);

    QFont f;
    QColor parsedColor;
    KnobGuiString::parseFont(label, &f, &parsedColor);
    EXPECT_EQ(9, f.pointSize());
    EXPECT_EQ(QString::fromUtf8("Arial"), f.family());
    EXPECT_EQ(picked, parsedColor);
}

TEST(KnobGuiString, PickingAColourTwiceReplacesItWithoutWrappingAgain)
{
    QString label = labelWithoutColour();

    KnobGuiString::findReplaceColorName(label, QColor(255, 0, 0));
    KnobGuiString::findReplaceColorName(label, QColor(0, 0, 255));

    EXPECT_EQ(1, countOccurrences(label, kFontSizeTag));
    EXPECT_EQ(1, countOccurrences(label, kFontColorTag));
    EXPECT_EQ(1, countOccurrences(label, kFontEndTag));
    int size = 0;
    QString colorName;
    QString family;
    ASSERT_TRUE(KnobGuiString::parseFontTagAttributes(label, &size, &colorName, &family));
    EXPECT_EQ(QColor(0, 0, 255), QColor(colorName));
}

TEST(KnobGuiString, AnInnerUserFontColourIsNeitherReadNorRewritten)
{
    const QString inner = QString::fromUtf8("<font color=\"#00ff00\">green</font>");
    QString label = KnobGuiString::decorateTextWithFontTag(QString::fromUtf8("Arial"), 9, QColor(), inner);

    int size = 0;
    QString colorName;
    QString family;
    ASSERT_TRUE(KnobGuiString::parseFontTagAttributes(label, &size, &colorName, &family));
    EXPECT_TRUE(colorName.isEmpty());

    KnobGuiString::findReplaceColorName(label, QColor(255, 0, 0));

    EXPECT_TRUE(label.contains(inner)) << label.toStdString();
    ASSERT_TRUE(KnobGuiString::parseFontTagAttributes(label, &size, &colorName, &family));
    EXPECT_EQ(QColor(255, 0, 0), QColor(colorName));
    EXPECT_EQ(QString::fromUtf8("Arial"), family);
}
