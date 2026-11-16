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
#include <string>
#include <vector>

#include <QtCore/QEvent>
#include <QtCore/QLocale>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
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
    _table->installEventFilter(this);
    QObject::connect(_table, &QTableView::doubleClicked, this, [this](const QModelIndex& index) { openFullValue(index); });

    _message->setObjectName(objectName + QString::fromUtf8("Message"));
    _message->setAlignment(Qt::AlignCenter);
    _message->setWordWrap(true);
    // A long message must not set the inspector's minimum width.
    _message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _stack->addWidget(_table);
    _stack->addWidget(_message);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(classBar);
    layout->addWidget(_stack, 1);

    updateClassBar();
}

GeoSpreadsheetWidget::~GeoSpreadsheetWidget() = default;

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
}

void
GeoSpreadsheetWidget::showMessage(const QString& text)
{
    _model->setSnapshot(GeoSpreadsheetModel::SourcePtr(), _model->attribClass());
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
        _model->setClass(cls);
        updateMetrics();
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

std::unique_ptr<ScenePrimInspectorTab>
createScenePrimSpreadsheetTab()
{
    return std::make_unique<ScenePrimSpreadsheetTab>();
}

NATRON_NAMESPACE_EXIT
