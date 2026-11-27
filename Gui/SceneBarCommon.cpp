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

#include "SceneBarCommon.h"

#include <QAction>
#include <QCoreApplication>
#include <QFrame>
#include <QSignalBlocker>
#include <QToolButton>

#include "Engine/AppManager.h"
#include "Gui/ThemedIcon.h"

NATRON_NAMESPACE_ENTER

namespace SceneBar {

QToolButton*
makeIconButton(QWidget* parent,
               const char* name,
               const char* iconPath,
               bool checkable)
{
    QToolButton* button = new QToolButton(parent);
    button->setObjectName(QString::fromUtf8(name));
    button->setFocusPolicy(Qt::NoFocus);
    button->setAutoRaise(true);
    button->setCheckable(checkable);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const int iconSize = TO_DPIX(18);
    button->setIconSize(QSize(iconSize, iconSize));
    setIcon(button, iconPath);
    button->setFixedSize(TO_DPIX(kBarSize - 4), TO_DPIY(kBarSize - 4));

    return button;
}

void
setIcon(QToolButton* button,
        const char* iconPath)
{
    const QString path = QString::fromUtf8(iconPath);
    // Bars call this on every state change; reloading the same icon would repaint for nothing.
    if (button->property("natronIcon").toString() == path) {
        return;
    }
    button->setProperty("natronIcon", path);
    button->setIcon(ThemedIcon::icon(QString::fromUtf8(NATRON_ICONS_PATH) + path, 18));
}

QFrame*
makeSeparator(QWidget* parent,
              bool inVerticalBar)
{
    QFrame* line = new QFrame(parent);
    line->setFrameShape(inVerticalBar ? QFrame::HLine : QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);

    return line;
}

void
showActionChecked(QAbstractButton* button,
                  const QAction* action)
{
    const bool checked = action && action->isChecked();
    if (button->isChecked() != checked) {
        const QSignalBlocker blocker(button);
        button->setChecked(checked);
    }
}

std::string
tipWithShortcut(const QString& tip)
{
    return "<p>" + tip.toStdString() + "</p><p><b>" + QCoreApplication::translate("SceneBar", "Keyboard shortcut: %1").toStdString() + "</b></p>";
}

} // namespace SceneBar

NATRON_NAMESPACE_EXIT
