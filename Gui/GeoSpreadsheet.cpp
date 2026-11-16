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

#include "Gui/GeoSpreadsheet.h"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QtCore/QEvent>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableView>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePrimElements.h"
#include "Engine/TLSHolder.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Gui/ScenePrimInspector.h"

NATRON_NAMESPACE_ENTER

struct GeoSpreadsheetBuildResult {
    GeoSpreadsheetModel::RowOrderPtr order;
    QString error;
};

namespace {

const char* const kClassLabels[kGeoAttribClassCount] = {
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Points"),
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Vertices"),
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Primitives"),
    QT_TRANSLATE_NOOP("GeoSpreadsheetWidget", "Detail"),
};

const char* const kClassObjectSuffixes[kGeoAttribClassCount] = { "Points", "Vertices", "Primitives", "Detail" };

QString
classLabel(GeoAttribClassEnum cls)
{
    return GeoSpreadsheetWidget::tr(kClassLabels[cls]);
}

// Exact counts of millions of rows would widen the class bar past the inspector's default width;
// the exact count is in the button's tooltip.
QString
compactCount(std::int64_t count)
{
    if (count < 100000) {
        return QString::number(count);
    }
    if (count < 1000000) {
        return QString::number(count / 1000) + QLatin1Char('k');
    }
    if (count < 1000000000) {
        return QString::number((double)count / 1e6, 'g', 3) + QLatin1Char('M');
    }

    return QString::number((double)count / 1e9, 'g', 3) + QLatin1Char('B');
}

QString
groupHelp()
{
    return GeoSpreadsheetWidget::tr("Show only the elements of a group. Geometry also takes element numbers (0-9 14), "
                                    "ranges with a step (0-99:2), attribute tests (@Cd > 0.5), ! to invert and ^ to remove.");
}

// Runs on the worker thread. A null order with no error is a cancelled build.
GeoSpreadsheetBuildResult
runBuild(const GeoSpreadsheetModel::SourcePtr& source,
         GeoAttribClassEnum cls,
         const std::string& pattern,
         int sortColumn,
         bool descending,
         const std::atomic<bool>& cancelled)
{
    GeoSpreadsheetBuildResult result;

    if (cancelled.load()) {
        return result;
    }
    GeoElementMask mask;
    bool filtered = false;
    if ((cls != eGeoAttribClassDetail) && (pattern.find_first_not_of(" \t\r\n") != std::string::npos)) {
        if (const GeoDetailSpreadsheetSource* geometry = dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get())) {
            std::string error;
            if (!geoSelectElements(*geometry->detail(), cls, pattern, &mask, &error)) {
                result.error = QString::fromStdString(error);

                return result;
            }
            filtered = true;
        } else if (const ScenePrimElementsSource* prim = dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
            const std::string name = QString::fromStdString(pattern).trimmed().toStdString();
            const ScenePrimElementGroup* found = nullptr;
            for (const ScenePrimElementGroup& group : prim->groups()) {
                if ((group.cls == cls) && (group.name == name)) {
                    found = &group;
                    break;
                }
            }
            if (!found) {
                result.error = GeoSpreadsheetWidget::tr("No group named \"%1\"").arg(QString::fromStdString(name));

                return result;
            }
            const std::int64_t rows = source->rowCount(cls);
            mask.assign((std::size_t)rows, 0);
            for (const int index : found->indices) {
                if ((index >= 0) && (index < rows)) {
                    mask[(std::size_t)index] = 1;
                }
            }
            filtered = true;
        }
    }
    result.order = geoSpreadsheetBuildOrder(*source, cls, filtered ? &mask : nullptr, sortColumn, descending, cancelled);

    return result;
}

class ScenePrimSpreadsheetTab
    : public ScenePrimInspectorTab {
public:
    ScenePrimSpreadsheetTab()
        : _widget(new GeoSpreadsheetWidget(QString::fromUtf8("sceneGraphSpreadsheet")))
    {
    }

    QString getTitle() const override
    {
        return ScenePrimInspector::tr("Spreadsheet");
    }

    QWidget* getWidget() const override
    {
        return _widget;
    }

    bool readsWithoutPrim(const NodePtr& node) const override
    {
        return putsOutGeometry(node);
    }

    std::function<void()> read(const ScenePrimInspectorRead& read) override
    {
        if (putsOutGeometry(read.getNode())) {
            return readGeometry(read);
        }
        const ScenePrimElements elements = SceneOps::readPrimElements(
            [&read](const std::function<void(const SceneStage* stage)>& readOne) { return read.readStage(readOne); },
            read.getPath().toStdString(),
            read.getFrame(),
            read.getFramesPerSecond());

        if ((elements.status == eScenePrimElementsStatusCancelled) || read.isCancelled()) {
            return std::function<void()>();
        }
        switch (elements.status) {
        case eScenePrimElementsStatusOk: {
            const GeoSpreadsheetModel::SourcePtr source = elements.source;

            return [this, source]() { _widget->setSnapshot(source); };
        }
        case eScenePrimElementsStatusNotLoaded:
            return [this]() { _widget->showMessage(GeoSpreadsheetWidget::tr("Not loaded — load the payload from the scene graph to inspect it")); };
        case eScenePrimElementsStatusMissing:
        case eScenePrimElementsStatusCancelled:
            break;
        }

        return [this]() { _widget->setSnapshot(GeoSpreadsheetModel::SourcePtr()); };
    }

    void setStale(bool stale) override
    {
        _widget->setStale(stale);
    }

    void clear() override
    {
        _widget->setSnapshot(GeoSpreadsheetModel::SourcePtr());
    }

    void clearSelection() override
    {
        _widget->clearSelection();
    }

    bool readsAtFrame() const override
    {
        return true;
    }

private:
    static bool putsOutGeometry(const NodePtr& node)
    {
        const EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

        return effect && node->isActivated() && effect->producesGeometryData();
    }

    // The node's whole detail: a prim selected in its scene, one of the layer the viewer made of
    // it, narrows nothing.
    std::function<void()> readGeometry(const ScenePrimInspectorRead& read)
    {
        GeoDetailConstPtr detail;
        const ViewerInstancePtr& viewer = read.getViewer();

        // The viewer's own copy, when it has one, numbers the elements as its 3D view picks them.
        if (!viewer || !viewer->getShownGeometry(0, &detail, nullptr) || !detail) {
            detail.reset();
            const EffectInstancePtr effect = read.getNode()->getEffectInstance();
            std::string error;
            EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
            try {
                code = effect->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(read.getFrame(), ViewIdx(0)), &detail, &error);
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
            }
            appPTR->getAppTLS()->cleanupTLSForThread();
            if (read.isCancelled()) {
                return std::function<void()>();
            }
            if ((code != EffectInstance::eRenderRoIRetCodeOk) || !detail) {
                const QString text = error.empty() ? GeoSpreadsheetWidget::tr("The geometry could not be evaluated")
                                                   : GeoSpreadsheetWidget::tr("The geometry could not be evaluated: %1").arg(QString::fromStdString(error));

                return [this, text]() { _widget->showMessage(text); };
            }
        }
        if (read.isCancelled()) {
            return std::function<void()>();
        }
        const GeoSpreadsheetModel::SourcePtr source = std::make_shared<GeoDetailSpreadsheetSource>(detail);

        return [this, source]() { _widget->setSnapshot(source); };
    }

    GeoSpreadsheetWidget* _widget;
};

} // anon namespace

GeoSpreadsheetWidget::GeoSpreadsheetWidget(const QString& objectName,
                                           QWidget* parent)
    : QWidget(parent)
    , _model(nullptr)
    , _classButtons()
    , _stack(new QStackedWidget(this))
    , _table(new QTableView(_stack))
    , _message(new QLabel(_stack))
    , _group(nullptr)
    , _busyLabel(nullptr)
    , _error(nullptr)
    , _thread(new QThread(this))
    , _worker(new QObject)
{
    setObjectName(objectName);

    QWidget* classBar = new QWidget(this);
    QHBoxLayout* barLayout = new QHBoxLayout(classBar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->setSpacing(2);
    QButtonGroup* group = new QButtonGroup(this);
    group->setExclusive(true);
    for (int c = 0; c < kGeoAttribClassCount; ++c) {
        QToolButton* button = new QToolButton(classBar);
        button->setObjectName(objectName + QString::fromUtf8(kClassObjectSuffixes[c]));
        button->setCheckable(true);
        button->setAutoRaise(true);
        group->addButton(button, c);
        barLayout->addWidget(button);
        _classButtons[(std::size_t)c] = button;
    }
    barLayout->addStretch(1);
    QObject::connect(group, &QButtonGroup::idClicked, this, [this](int id) { showClass((GeoAttribClassEnum)id); });

    _model = new GeoSpreadsheetModel(_table);
    _table->setObjectName(objectName + QString::fromUtf8("Table"));
    _table->setModel(_model);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setWordWrap(false);
    _table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    _table->horizontalHeader()->setStretchLastSection(false);
    QHeaderView* header = _table->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    // A header click sorts; it must not also select the column's millions of cells.
    QObject::disconnect(header, SIGNAL(sectionPressed(int)), _table, nullptr);
    QObject::connect(header, &QHeaderView::sectionClicked, this, [this](int column) { headerClicked(column); });
    _table->installEventFilter(this);
    QObject::connect(_table, &QTableView::doubleClicked, this, [this](const QModelIndex& index) { openFullValue(index); });

    _message->setObjectName(objectName + QString::fromUtf8("Message"));
    _message->setAlignment(Qt::AlignCenter);
    _message->setWordWrap(true);
    // A long message must not set the inspector's minimum width.
    _message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _stack->addWidget(_table);
    _stack->addWidget(_message);

    QWidget* filterBar = new QWidget(this);
    QHBoxLayout* filterLayout = new QHBoxLayout(filterBar);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(4);
    _group = new QComboBox(filterBar);
    _group->setObjectName(objectName + QString::fromUtf8("Group"));
    _group->setEditable(true);
    _group->setInsertPolicy(QComboBox::NoInsert);
    _group->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _group->setMinimumContentsLength(8);
    _group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _group->lineEdit()->setPlaceholderText(tr("Group"));
    _group->lineEdit()->setClearButtonEnabled(true);
    _group->setToolTip(groupHelp());
    QObject::connect(_group->lineEdit(), &QLineEdit::editingFinished, this, [this]() { groupSubmitted(); });
    QObject::connect(_group, QOverload<int>::of(&QComboBox::activated), this, [this](int) { groupSubmitted(); });
    _busyLabel = new QLabel(tr("Sorting…"), filterBar);
    _busyLabel->setObjectName(objectName + QString::fromUtf8("Busy"));
    _busyLabel->hide();
    filterLayout->addWidget(_group, 1);
    filterLayout->addWidget(_busyLabel);

    _error = new QLabel(this);
    _error->setObjectName(objectName + QString::fromUtf8("Error"));
    _error->setWordWrap(true);
    _error->setStyleSheet(QString::fromUtf8("QLabel { color: #e05a5a; }"));
    _error->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _error->hide();

    _worker->moveToThread(_thread);
    _thread->start();

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(classBar);
    layout->addWidget(filterBar);
    layout->addWidget(_error);
    layout->addWidget(_stack, 1);

    updateClassBar();
}

GeoSpreadsheetWidget::~GeoSpreadsheetWidget()
{
    if (_cancel) {
        _cancel->store(true);
    }
    _thread->quit();
    _thread->wait();
    delete _worker;
}

QStringList
GeoSpreadsheetWidget::columnNames() const
{
    QStringList names;

    for (int c = 0; c < _model->columnCount(); ++c) {
        names << _model->headerData(c, Qt::Horizontal).toString();
    }

    return names;
}

void
GeoSpreadsheetWidget::setSnapshot(const GeoSpreadsheetModel::SourcePtr& source)
{
    const GeoAttribClassEnum shownClass = _model->attribClass();
    GeoAttribClassEnum cls = shownClass;
    QString wanted = wantedPattern();

    cancelBuild();

    if (source && (source->rowCount(cls) == 0)) {
        for (int c = 0; c < kGeoAttribClassCount; ++c) {
            if (source->rowCount((GeoAttribClassEnum)c) > 0) {
                cls = (GeoAttribClassEnum)c;
                break;
            }
        }
    }
    // A new frame of the same geometry keeps the widths, measured or set by the user: the reset
    // would put every column back to its default width.
    const bool mayKeepWidths = _model->source() && source && (cls == shownClass) && !_metricsOwed;
    const QStringList names = mayKeepWidths ? columnNames() : QStringList();
    std::vector<int> widths;
    for (int c = 0; c < names.size(); ++c) {
        widths.push_back(_table->columnWidth(c));
    }
    _model->setSnapshot(source, cls);
    _shownValid = false;
    if (!source || (cls != shownClass)) {
        _sortColumn = -1;
        _sortName.clear();
        wanted.clear();
        _pattern.clear();
        _typed.clear();
        setError(QString());
    } else {
        _sortColumn = _sortName.isEmpty() ? -1 : columnNames().indexOf(_sortName);
    }
    _message->clear();
    _stack->setCurrentWidget(_table);
    updateClassBar();
    if (mayKeepWidths && (columnNames() == names)) {
        for (int c = 0; c < names.size(); ++c) {
            _table->setColumnWidth(c, widths[(std::size_t)c]);
        }
    } else {
        updateMetrics();
    }
    populateGroups();
    updateSortIndicator();
    if (source) {
        requestBuild(wanted);
    }
}

void
GeoSpreadsheetWidget::showMessage(const QString& text)
{
    cancelBuild();
    _shownValid = false;
    _model->setSnapshot(GeoSpreadsheetModel::SourcePtr(), _model->attribClass());
    populateGroups();
    _message->setText(text);
    _stack->setCurrentWidget(_message);
    updateClassBar();
}

void
GeoSpreadsheetWidget::setStale(bool stale)
{
    QPalette palette = _table->palette();
    QColor color = QApplication::palette().color(QPalette::Text);

    if (stale) {
        color.setAlphaF(0.4);
    }
    palette.setColor(QPalette::Text, color);
    _table->setPalette(palette);
    _message->setEnabled(!stale);
}

void
GeoSpreadsheetWidget::clearSelection()
{
    _table->selectionModel()->clear();
}

qlonglong
GeoSpreadsheetWidget::getShownRows() const
{
    return (qlonglong)_model->rowCount();
}

QString
GeoSpreadsheetWidget::getShownClassName() const
{
    return classLabel(_model->attribClass());
}

qulonglong
GeoSpreadsheetWidget::getCellReads() const
{
    return (qulonglong)_model->cellsRead();
}

bool
GeoSpreadsheetWidget::eventFilter(QObject* watched,
                                  QEvent* e)
{
    if ((watched == _table) && _metricsOwed && ((e->type() == QEvent::Show) || (e->type() == QEvent::Resize))) {
        updateMetrics();
    }

    return QWidget::eventFilter(watched, e);
}

void
GeoSpreadsheetWidget::showClass(GeoAttribClassEnum cls)
{
    if (cls != _model->attribClass()) {
        cancelBuild();
        _model->setClass(cls);
        _shownValid = false;
        _sortColumn = -1;
        _sortName.clear();
        _pattern.clear();
        _typed.clear();
        setError(QString());
        updateMetrics();
        populateGroups();
        updateSortIndicator();
        requestBuild(QString());
    }
    updateClassBar();
}

void
GeoSpreadsheetWidget::updateClassBar()
{
    const GeoSpreadsheetModel::SourcePtr& source = _model->source();
    const QLocale locale;

    for (int c = 0; c < kGeoAttribClassCount; ++c) {
        QToolButton* button = _classButtons[(std::size_t)c];
        const GeoAttribClassEnum cls = (GeoAttribClassEnum)c;
        const std::int64_t rows = source ? source->rowCount(cls) : 0;
        button->setText(QString::fromUtf8("%1 %2").arg(classLabel(cls), compactCount(rows)));
        button->setToolTip(tr("%1: %2 rows").arg(classLabel(cls), locale.toString((qlonglong)rows)));
        button->setChecked(cls == _model->attribClass());
        button->setEnabled((bool)source);
    }
}

// Column widths come from the rows in view, so they are measured again once the table has its size.
void
GeoSpreadsheetWidget::updateMetrics()
{
    _metricsOwed = !_table->isVisible() || (_table->viewport()->height() <= 0);
    applyGeoSpreadsheetMetrics(_table);
}

void
GeoSpreadsheetWidget::openFullValue(const QModelIndex& index)
{
    const GeoSpreadsheetModel::SourcePtr source = _model->source();

    if (!source || !index.isValid()) {
        return;
    }
    const GeoAttribClassEnum cls = _model->attribClass();
    const GeoIndex element = _model->elementAt(index.row());
    if ((element < 0) || (element >= source->rowCount(cls))) {
        return;
    }
    const int column = index.column();
    openScenePrimFullValue(_table, [source, cls, column, element](const std::atomic<bool>* /*cancelled*/) {
        return QString::fromStdString(source->fullValue(cls, column, element));
    });
}

QString
GeoSpreadsheetWidget::wantedPattern() const
{
    return _busy ? _pendingPattern : _pattern;
}

void
GeoSpreadsheetWidget::populateGroups()
{
    const GeoSpreadsheetModel::SourcePtr& source = _model->source();
    const GeoAttribClassEnum cls = _model->attribClass();
    QStringList names;

    if (source && (cls != eGeoAttribClassDetail)) {
        if (const GeoDetailSpreadsheetSource* geometry = dynamic_cast<const GeoDetailSpreadsheetSource*>(source.get())) {
            for (const std::string& name : geometry->detail()->groupNames(cls)) {
                names << QString::fromStdString(name);
            }
        } else if (const ScenePrimElementsSource* prim = dynamic_cast<const ScenePrimElementsSource*>(source.get())) {
            for (const ScenePrimElementGroup& group : prim->groups()) {
                if (group.cls == cls) {
                    names << QString::fromStdString(group.name);
                }
            }
        }
    }
    const QSignalBlocker blocker(_group);
    _group->clear();
    _group->addItem(QString());
    _group->addItems(names);
    _group->setEditText(_typed);
    _group->setEnabled(source && (cls != eGeoAttribClassDetail));
}

void
GeoSpreadsheetWidget::updateSortIndicator()
{
    _table->horizontalHeader()->setSortIndicator(_sortColumn, _descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void
GeoSpreadsheetWidget::setError(const QString& text)
{
    _error->setText(text);
    _error->setVisible(!text.isEmpty());
    _group->setToolTip(text.isEmpty() ? groupHelp() : text);
}

void
GeoSpreadsheetWidget::headerClicked(int column)
{
    if (!_model->source() || (column < 0) || (column >= _model->columnCount())) {
        return;
    }
    if (column == _sortColumn) {
        _descending = !_descending;
    } else {
        _sortColumn = column;
        _sortName = _model->headerData(column, Qt::Horizontal).toString();
        _descending = false;
    }
    updateSortIndicator();
    requestBuild(wantedPattern());
}

void
GeoSpreadsheetWidget::groupSubmitted()
{
    const QString text = _group->currentText();

    if (!_model->source() || (text == _typed)) {
        return;
    }
    _typed = text;
    requestBuild(text);
}

bool
GeoSpreadsheetWidget::shownMatchesWanted() const
{
    return _shownValid && (_shownColumn == _sortColumn) && (_shownDescending == _descending) && (_shownPattern == _pattern);
}

void
GeoSpreadsheetWidget::cancelBuild()
{
    if (_busy && _cancel) {
        _cancel->store(true);
        ++_sortsCancelled;
    }
    _cancel.reset();
    _busy = false;
    ++_generation;
    _busyLabel->hide();
}

void
GeoSpreadsheetWidget::requestBuild(const QString& pattern,
                                   bool keepError)
{
    cancelBuild();
    if (!_model->source()) {
        return;
    }
    if ((_sortColumn < 0) && pattern.trimmed().isEmpty()) {
        _pattern = pattern;
        if (!keepError) {
            setError(QString());
        }
        applyOrder(GeoSpreadsheetModel::RowOrderPtr());
        _shownValid = true;
        _shownPattern = _pattern;
        _shownColumn = -1;
        _shownDescending = _descending;

        return;
    }
    launchBuild(pattern, keepError);
}

void
GeoSpreadsheetWidget::launchBuild(const QString& pattern,
                                  bool keepError)
{
    const GeoSpreadsheetModel::SourcePtr source = _model->source();
    const GeoAttribClassEnum cls = _model->attribClass();
    const std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = ++_generation;
    const std::string text = pattern.toStdString();
    const int column = _sortColumn;
    const bool descending = _descending;

    _cancel = cancel;
    _busy = true;
    _pendingPattern = pattern;
    ++_sortsStarted;
    _busyLabel->show();
    QMetaObject::invokeMethod(_worker, [this, source, cls, text, column, descending, cancel, generation, pattern, keepError]() {
        GeoSpreadsheetBuildResult result = runBuild(source, cls, text, column, descending, *cancel);
        // The destructor joins this thread before the widget goes, so the widget outlives this call.
        QMetaObject::invokeMethod(this, [this, generation, result, pattern, keepError]() { finishBuild(generation, result, pattern, keepError); }, Qt::QueuedConnection); }, Qt::QueuedConnection);
}

void
GeoSpreadsheetWidget::finishBuild(quint64 generation,
                                  const GeoSpreadsheetBuildResult& result,
                                  const QString& pattern,
                                  bool keepError)
{
    if (generation != _generation) {
        return;
    }
    _busy = false;
    _cancel.reset();
    _busyLabel->hide();
    if (!result.error.isEmpty()) {
        setError(result.error);
        if (!shownMatchesWanted()) {
            // A pattern that cannot be applied leaves the previous filter, or none when the snapshot cannot take it.
            const QString fallback = (pattern == _pattern) ? QString() : _pattern;
            requestBuild(fallback, true);
        }

        return;
    }
    if (!result.order) {
        return;
    }
    _pattern = pattern;
    if (!keepError) {
        setError(QString());
    }
    applyOrder(result.order);
    _shownValid = true;
    _shownPattern = _pattern;
    _shownColumn = _sortColumn;
    _shownDescending = _descending;
}

// A model reset drops the column widths, which the user may have set.
void
GeoSpreadsheetWidget::applyOrder(const GeoSpreadsheetModel::RowOrderPtr& order)
{
    std::vector<int> widths;
    const int columns = _model->columnCount();

    for (int c = 0; c < columns; ++c) {
        widths.push_back(_table->columnWidth(c));
    }
    _model->setRowOrder(order);
    if (_model->columnCount() == columns) {
        for (int c = 0; c < columns; ++c) {
            _table->setColumnWidth(c, widths[(std::size_t)c]);
        }
    }
}

std::unique_ptr<ScenePrimInspectorTab>
createScenePrimSpreadsheetTab()
{
    return std::make_unique<ScenePrimSpreadsheetTab>();
}

NATRON_NAMESPACE_EXIT
