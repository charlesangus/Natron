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

#ifndef Gui_SceneBreadcrumbBar_h
#define Gui_SceneBreadcrumbBar_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtWidgets/QWidget>

#include "Gui/GuiFwd.h"

class QToolButton;

NATRON_NAMESPACE_ENTER

class SceneViewport;

/**
 * @brief The strip above the scene view that shows the current selected prim's path as `/` and
 * one button per segment. A click selects the segment's prim, Shift+click adds it and `/` clears
 * the selection. A chevron after `/` and after each segment lists the prim's children in a
 * ScenePrimListPopup, and a right-click on a segment lists its siblings with it marked. Segments
 * the stage no longer has are greyed and cannot be clicked; when the bar is too narrow the middle segments fold into a "..." button whose menu lists them.
 *
 * The widget publishes natronBreadcrumbSegments (see Segment::toVariant()), natronBreadcrumbPath,
 * natronBreadcrumbMore and natronBreadcrumbElided for GUI tests. Its buttons are named
 * "breadcrumbRoot", "breadcrumbRootChevron", "breadcrumbSegment", "breadcrumbChevron",
 * "breadcrumbElision" and "breadcrumbMore".
 **/
class SceneBreadcrumbBar : public QWidget {
    Q_OBJECT

public:
    enum StateEnum {
        // The stage has the prim.
        eStatePresent,
        // The stage does not have the prim: shown, not clickable.
        eStateMissing,
        // No stage to ask, or a sync under way: the text is shown, nothing is clickable.
        eStateUnknown
    };

    // One step of the path. More kinds of segment (a "+N" badge) extend this.
    struct Segment {
        QString text;
        // The prim the segment stands for, the segment's own path from the root.
        QString path;
        StateEnum state = eStateUnknown;
        // Whether the segment's button is folded into the "..." menu.
        bool elided = false;

        QVariant toVariant() const;
    };

    explicit SceneBreadcrumbBar(SceneViewport* viewport, QWidget* parent = nullptr);

    ~SceneBreadcrumbBar() override;

    // The segments of the path shown, `/` not included, empty with no selection.
    QList<Segment> getSegments() const;

    // The path shown: the selection's last prim, empty with no selection.
    QString getPath() const;

    // The selected prims past the one shown.
    int getMoreCount() const;

    QSize minimumSizeHint() const override;

public Q_SLOTS:

    // Reads the viewport's selection and the stage again.
    void refresh();

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void rebuild();
    void layoutButtons();
    void clickSegment(const Segment& segment);
    void readStates(QList<Segment>& segments);
    QToolButton* makeButton(const QString& name, const QString& text);
    QToolButton* makeChevron(const QString& name, const QString& path);
    void openPopup(const QString& listedPath, const QString& highlightName, QWidget* anchor);

    struct Private;
    Private* const _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneBreadcrumbBar_h
