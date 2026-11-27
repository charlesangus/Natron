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
#include <functional>
#include <memory>
#include <string>

#include <QtCore/QMetaObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Geometry/GeoViewerLayer.h"
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
struct GeoSpreadsheetBuildResult;
struct GeoSpreadsheetPickResult;
struct GeoSpreadsheetShowResult;

/**
 * @brief The rows of @p cls a Group @p pattern and, when given, @p selectedOnly keep, as a mask indexed by dense
 * row, ready for geoSpreadsheetBuildOrder(). A geometry source's selection is read in storage offsets and mapped
 * to rows here, so tombstoned elements never shift the rows kept. *filtered is false, and @p mask empty, when
 * nothing narrows the rows. Returns false with *error set for a malformed pattern. Any thread.
 **/
bool geoSpreadsheetFilterMask(const GeoSpreadsheetSource& source,
                              GeoAttribClassEnum cls,
                              const std::string& pattern,
                              const GeoComponentSelection* selectedOnly,
                              const std::atomic<bool>& cancelled,
                              GeoElementMask* mask,
                              bool* filtered,
                              std::string* error);

/**
 * @brief A class bar (Points, Vertices, Primitives, Detail, with row counts) over a table of one
 * GeoSpreadsheetSource snapshot, or a message in place of the table. Main thread only; nothing it does
 * on the main thread scales with the rows.
 *
 * The Group filter, the Selected Only narrowing and the column sort are built together on the widget's
 * own worker, one build at a time, a newer request cancelling the one under way; the table keeps its
 * previous order until the new one arrives. A malformed pattern leaves the previous filter applied.
 *
 * With a viewer set through setSelectionViewer(), selected rows replace that view's component selection
 * and the view's selection selects rows, through the inverse of the built order. Selected Only does not
 * write the view's selection, since that would shrink the rows to the ones selected. For a prim, rows
 * highlight in the scene view through setHighlightTarget() instead, and Selected Only narrows to the
 * highlight.
 *
 * Children are named by the widget's object name followed by Points, Vertices, Primitives, Detail,
 * Table, Message, Group, Busy, Error, SelectionNote and SelectedOnly. natronGeoSpreadsheetHoldSorts
 * keeps builds from starting until it is cleared or the build is cancelled, so tests can hold one.
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
    Q_PROPERTY(bool natronGeoSpreadsheetSelectedOnly READ getSelectedOnly WRITE setSelectedOnly)
    Q_PROPERTY(bool natronGeoSpreadsheetSortBusy READ getSortBusy)
    Q_PROPERTY(int natronGeoSpreadsheetSortsStarted READ getSortsStarted)
    Q_PROPERTY(int natronGeoSpreadsheetSortsCancelled READ getSortsCancelled)
    Q_PROPERTY(bool natronGeoSpreadsheetHoldSorts READ getHoldSorts WRITE setHoldSorts)

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

    /**
     * @brief For a prim's rows: the viewer whose scene view shows @p node, and the prim at @p primPath
     * the rows highlight there. A change of any of the three, or an empty viewer, clears the highlight.
     **/
    void setHighlightTarget(const ViewerInstanceWPtr& viewer,
                            const QString& primPath,
                            const NodeWPtr& node);

    bool getSelectedOnly() const
    {
        return _selectedOnly;
    }

    void setSelectedOnly(bool on);

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

    bool getHoldSorts() const
    {
        return _holdSorts.load();
    }

    void setHoldSorts(bool hold)
    {
        _holdSorts.store(hold);
    }

    /**
     * @brief Whether the create key makes a node from the rows selected: when they are rows of the
     * geometry a 3D viewer shows, whose selection they are.
     **/
    bool canCreateFromSelection() const;

    // Creates a node from the selection, as the create key in the viewer does.
    void createFromSelection();

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
    bool selectedOnlyActive() const;
    void updateSelectedOnlyEnabled();
    void rebuildForSelection();
    void clearHighlight();
    void applyHighlight(const GeoComponentSelection& selection);

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
    QToolButton* _selectedOnlyButton;
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
    bool _shownSelectedOnly = false;
    bool _selectedOnly = false;

    ViewerInstanceWPtr _selectionViewer;
    ViewerInstanceWPtr _highlightViewer;
    NodeWPtr _highlightNode;
    std::string _highlightPath;
    GeoComponentSelection _highlightSelection;
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
    std::atomic<bool> _holdSorts { false };
};

// Whether node puts out native geometry, which the Geometry Spreadsheet panel lists.
bool geoSpreadsheetListsNode(const NodePtr& node);

struct GeoSpreadsheetNodeRead {
    // Null when the read failed, or was cancelled.
    GeoSpreadsheetModel::SourcePtr source;
    // Set when the source is the viewer's copy, so rows and the 3D view's picks share element numbers.
    ViewerInstanceWPtr selectionViewer;
    bool fromViewer = false;
    bool cancelled = false;
    QString error;
};

/**
 * @brief The whole detail node puts out at frame: the copy viewer's 3D view serves when it shows that
 * node's current geometry at that frame, else the node evaluated on the calling thread, a worker.
 **/
GeoSpreadsheetNodeRead geoSpreadsheetReadNode(const NodePtr& node,
                                              const ViewerInstancePtr& viewer,
                                              double frame,
                                              const std::function<bool()>& isCancelled);

/**
 * @brief The inspector's Spreadsheet tab: the selected prim's points, vertices, primitives and detail as
 * USD holds them at the inspector's frame, read without loading a payload. A node that puts out native
 * geometry shows a note pointing to the Geometry Spreadsheet panel instead.
 **/
std::unique_ptr<ScenePrimInspectorTab> createScenePrimSpreadsheetTab();

NATRON_NAMESPACE_EXIT

#endif // Gui_GeoSpreadsheet_h
