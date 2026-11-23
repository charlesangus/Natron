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
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <QChar>
#include <QPushButton>
#include <QString>

#include <gtest/gtest.h>

#include "Gui/QtEnumConvert.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::vector<std::pair<StandardButtonEnum, QAbstractButton*>> ButtonList;

// Returns the mnemonic letter of text, or a null QChar unless it holds exactly one ampersand before a letter.
QChar
mnemonicOf(const QString& text)
{
    if (text.count(QLatin1Char('&')) != 1) {
        return QChar();
    }
    const int at = text.indexOf(QLatin1Char('&'));
    if (at + 1 >= text.size() || !text[at + 1].isLetter()) {
        return QChar();
    }

    return text[at + 1].toLower();
}

} // namespace

TEST(QtEnumConvertMnemonics, CollisionHeavySetGivesEveryButtonItsOwnLetter)
{
    const StandardButtonEnum which[] = {
        eStandardButtonYes, eStandardButtonOk, eStandardButtonYesToAll, eStandardButtonSaveAll,
        eStandardButtonOpen, eStandardButtonApply
    };
    std::vector<std::shared_ptr<QPushButton>> owned;
    ButtonList buttons;
    for (StandardButtonEnum b : which) {
        owned.push_back(std::make_shared<QPushButton>());
        buttons.push_back(std::make_pair(b, static_cast<QAbstractButton*>(owned.back().get())));
    }

    QtEnumConvert::setStandardButtonMnemonics(buttons);

    std::set<QChar> seen;
    for (const std::shared_ptr<QPushButton>& button : owned) {
        const QChar letter = mnemonicOf(button->text());
        EXPECT_FALSE(letter.isNull()) << button->text().toStdString();
        EXPECT_TRUE(seen.insert(letter).second) << button->text().toStdString();
    }
    EXPECT_EQ(QChar::fromLatin1('y'), mnemonicOf(owned[0]->text()));
}

TEST(QtEnumConvertMnemonics, PreferredLettersSurviveTheSavePrompt)
{
    QPushButton save, discard, cancel;
    ButtonList buttons;
    buttons.push_back(std::make_pair(eStandardButtonCancel, static_cast<QAbstractButton*>(&cancel)));
    buttons.push_back(std::make_pair(eStandardButtonSave, static_cast<QAbstractButton*>(&save)));
    buttons.push_back(std::make_pair(eStandardButtonDiscard, static_cast<QAbstractButton*>(&discard)));

    QtEnumConvert::setStandardButtonMnemonics(buttons);

    EXPECT_EQ(QChar::fromLatin1('s'), mnemonicOf(save.text()));
    EXPECT_EQ(QChar::fromLatin1('d'), mnemonicOf(discard.text()));
    EXPECT_EQ(QChar::fromLatin1('c'), mnemonicOf(cancel.text()));
}
