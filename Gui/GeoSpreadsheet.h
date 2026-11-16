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
#include <atomic>
#include <memory>

#include <QtCore/QMetaObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include "Engine/EngineFwd.h"
#include "Gui/GeoSpreadsheetModel.h"

class QComboBox;
class QLabel;
class QThread;
class QStackedWidget;
class QTableView;
class QToolButton;

NATRON_NAMESPACE_ENTER

class ScenePrimInspectorTab;
class SceneViewport;
struct GeoComponentSelection;
struct GeoSpreadsheetBuildResult;
struct GeoSpreadsheetPickResult;
struct GeoSpreadsheetShowResult;

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
 *
 * A Group box filters the rows to a group of the class shown (for geometry, any group pattern; for a USD
 * prim, a subset name), and a click on a column header sorts by it, a second click reversing. The
 * filtered and sorted order is built on the widget's own worker thread, one build at a time: a newer
 * sort, filter, snapshot or class cancels the build under way. The table keeps its previous order, and
 * stays usable, until the new one arrives. A new snapshot keeps the sort column and the filter. A
 * malformed pattern shows its error under the Group box and leaves the previous filter applied.
 * natronGeoSpreadsheetSortBusy is true while a build runs; natronGeoSpreadsheetSortsStarted and
 * natronGeoSpreadsheetSortsCancelled count builds started and builds cancelled before they finished.
 * Children Group, Busy and Error carry the widget's object name as prefix like the others.
 *
 * With a viewer set through setSelectionViewer(), whose view shows the geometry the table lists, selecting
 * rows of Points, Vertices or Primitives replaces that view's live component selection: the points,
 * the points of the vertices, or the primitives numbered by the rows. A selection of many rows is
 * gathered on the worker and handed to the view when done, a newer one cancelling it. Detail rows select
 * nothing.
 *
 * The other way round, a change of that view's selection that the table did not make selects the rows of
 * the elements picked, in the class they belong to: points and primitives as they are, edges as their
 * endpoints in Points. Objects mode or nothing picked clears the rows. The first selected row is scrolled
 * into view. Rows are selected as runs of contiguous view rows, through the inverse of the sort and filter
 * order built beside that order. Picked elements the group filter leaves out are counted in the
 * SelectionNote label, which natronGeoSpreadsheetSelectionNote reads, e.g. "12 selected, 3 hidden by filter".
 **/
class GeoSpreadsheetWidget
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON
    Q_PROPERTY(qlonglong natronGeoSpreadsheetRows READ getShownRows)
    Q_PROPERTY(QString natronGeoSpreadsheetClass READ getShownClassName)
    Q_PROPERTY(qulonglong natronGeoSpreadsheetCellReads READ getCellReads)
    Q_PROPERTY(QString natronGeoSpreadsheetSelectionNote READ getSelectionNote)
    Q_PROPERTY(bool natronGeoSpreadsheetSortBusy READ getSortBusy)
    Q_PROPERTY(int natronGeoSpreadsheetSortsStarted READ getSortsStarted)
    Q_PROPERTY(int natronGeoSpreadsheetSortsCancelled READ getSortsCancelled)

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

    // The viewer whose view takes the rows selected; empty for none.
    void setSelectionViewer(const ViewerInstanceWPtr& viewer);

    qlonglong getShownRows() const;

    QString getShownClassName() const;

    qulonglong getCellReads() const;

    QString getSelectionNote() const;

    bool getSortBusy() const
    {
        return _busy;
    }

    int getSortsStarted() const
    {
        return _sortsStarted;
    }

    int getSortsCancelled() const
    {
        return _sortsCancelled;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void showClass(GeoAttribClassEnum cls);
    void updateClassBar();
    void updateMetrics();
    QStringList columnNames() const;
    void openFullValue(const QModelIndex& index);
    void populateGroups();
    void headerClicked(int column);
    void groupSubmitted();
    void updateSortIndicator();
    void setError(const QString& text);
    void cancelBuild();
    void requestBuild(const QString& pattern,
                      bool keepError = false);
    void launchBuild(const QString& pattern,
                     bool keepError);
    void finishBuild(quint64 generation,
                     const GeoSpreadsheetBuildResult& result,
                     const QString& pattern,
                     bool keepError);
    void applyOrder(const GeoSpreadsheetModel::RowOrderPtr& order,
                    const GeoSpreadsheetModel::RowInversePtr& inverse = GeoSpreadsheetModel::RowInversePtr());
    bool shownMatchesWanted() const;
    void rowsSelected();
    void viewerSelectionChanged();
    void syncFromViewer(bool switchClass,
                        bool scroll);
    void applyShown(const GeoSpreadsheetShowResult& result,
                    bool scroll);
    void finishShow(quint64 generation,
                    const GeoSpreadsheetShowResult& result,
                    bool scroll);
    void clearRows();
    void setSelectionNote(const QString& text);
    void cancelPick();
    void writeSelection(SceneViewport* viewport,
                        const GeoComponentSelection& selection);
    void finishPick(quint64 generation,
                    const GeoSpreadsheetPickResult& result);
    QString wantedPattern() const;

    GeoSpreadsheetModel* _model;
    std::array<QToolButton*, kGeoAttribClassCount> _classButtons;
    QStackedWidget* _stack;
    QTableView* _table;
    QLabel* _message;
    bool _metricsOwed = false;

    QComboBox* _group;
    QLabel* _busyLabel;
    QLabel* _error;
    QLabel* _selectionNote;
    QThread* _thread;
    QObject* _worker;

    // The filter applied, the sort wanted and, by name so it survives a new snapshot, its column.
    QString _pattern;
    QString _typed;
    QString _pendingPattern;
    QString _sortName;
    int _sortColumn = -1;
    bool _descending = false;

    // What the model's order was built for.
    bool _shownValid = false;
    QString _shownPattern;
    int _shownColumn = -1;
    bool _shownDescending = false;

    ViewerInstanceWPtr _selectionViewer;
    std::shared_ptr<std::atomic<bool>> _pickCancel;
    quint64 _pickGeneration = 0;
    bool _writingSelection = false;
    bool _showingSelection = false;
    std::array<QMetaObject::Connection, 2> _viewerConnections;

    std::shared_ptr<std::atomic<bool>> _cancel;
    quint64 _generation = 0;
    bool _busy = false;
    int _sortsStarted = 0;
    int _sortsCancelled = 0;
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
