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

#ifndef Gui_ScenePurposeDelegate_h
#define Gui_ScenePurposeDelegate_h

#include "Global/Macros.h"

#include <QtGui/QPalette>
#include <QtWidgets/QStyledItemDelegate>

#include "Gui/SceneGraphModel.h"

NATRON_NAMESPACE_ENTER

// Draws a purpose the row inherits dimmed, as the draw state column does, so that the rows that
// author one stand out.
class ScenePurposeDelegate
    : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option,
                         const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.data(SceneGraphModel::ePurposeInheritedRole).toBool()) {
            option->palette.setColor(QPalette::Text, option->palette.color(QPalette::Disabled, QPalette::Text));
            option->palette.setColor(QPalette::HighlightedText, option->palette.color(QPalette::Disabled, QPalette::HighlightedText));
        }
    }
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ScenePurposeDelegate_h
