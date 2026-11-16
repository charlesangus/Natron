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

#ifndef Gui_GeoSpreadsheet_h
#define Gui_GeoSpreadsheet_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <memory>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include "Gui/GeoSpreadsheetModel.h"

class QLabel;
class QStackedWidget;
class QTableView;
class QToolButton;

NATRON_NAMESPACE_ENTER

class ScenePrimInspectorTab;

/**
 * @brief A class bar (Points, Vertices, Primitives, Detail, each with its row count) over a table of one
 * GeoSpreadsheetSource snapshot, or a message in place of the table. A new snapshot keeps the class shown
 * when it has rows in it, else shows the first class that has. Double-clicking a cell opens its whole value
 * in the inspector's full value window. Main thread only; nothing it does on its own scales with the rows.
 *
 * Scripts find its children by object name, the widget's own followed by Points, Vertices, Primitives,
 * Detail, Table and Message, and read natronGeoSpreadsheetRows (the rows of the class shown),
 * natronGeoSpreadsheetClass (its label) and natronGeoSpreadsheetCellReads (cells the table has read since the
 * snapshot or the class last changed) as properties.
 **/
class GeoSpreadsheetWidget
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON
    Q_PROPERTY(qlonglong natronGeoSpreadsheetRows READ getShownRows)
    Q_PROPERTY(QString natronGeoSpreadsheetClass READ getShownClassName)
    Q_PROPERTY(qulonglong natronGeoSpreadsheetCellReads READ getCellReads)

public:
    explicit GeoSpreadsheetWidget(const QString& objectName,
                                  QWidget* parent = nullptr);
    ~GeoSpreadsheetWidget() override;

    // A null source shows an empty table.
    void setSnapshot(const GeoSpreadsheetModel::SourcePtr& source);

    // Drops the snapshot and shows text in place of the table.
    void showMessage(const QString& text);

    // Shows the table greyed, as no longer current.
    void setStale(bool stale);

    void clearSelection();

    qlonglong getShownRows() const;

    QString getShownClassName() const;

    qulonglong getCellReads() const;

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void showClass(GeoAttribClassEnum cls);
    void updateClassBar();
    void updateMetrics();
    QStringList columnNames() const;
    void openFullValue(const QModelIndex& index);

    GeoSpreadsheetModel* _model;
    std::array<QToolButton*, kGeoAttribClassCount> _classButtons;
    QStackedWidget* _stack;
    QTableView* _table;
    QLabel* _message;
    bool _metricsOwed = false;
};

/**
 * @brief The inspector's Spreadsheet tab: the selected prim's points, vertices, primitives and detail as
 * USD holds them at the inspector's frame, read without loading a payload. For a node that puts out
 * geometry, its whole detail instead: the one the viewer shows when the viewer shows that node, else
 * the node evaluated on the inspector's worker at the inspector's frame.
 **/
std::unique_ptr<ScenePrimInspectorTab> createScenePrimSpreadsheetTab();

NATRON_NAMESPACE_EXIT

#endif // Gui_GeoSpreadsheet_h
