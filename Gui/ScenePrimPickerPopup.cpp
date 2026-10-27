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

#include "ScenePrimPickerPopup.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "Engine/Node.h"
#include "Gui/Gui.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/SceneNodeStageSource.h"
#include "Gui/SceneSelectionActions.h"
#include "Gui/SceneViewport.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

struct ScenePrimPickerPopup::Implementation {
    Gui* gui = nullptr;
    bool multiple = false;
    QStringList current;
    bool synced = false;
    std::shared_ptr<SceneNodeStageSource> source;
    SceneGraphModel* model = nullptr;
    QLabel* status = nullptr;
    QTreeView* tree = nullptr;
    QPushButton* useViewer = nullptr;
    QDialogButtonBox* buttons = nullptr;
    QPointer<SceneViewport> viewport;

    SceneViewport* selectionViewport() const
    {
        ViewerTab* viewer = SceneSelectionActions::selectionViewer(gui);

        return (viewer && viewer->isShowingScene()) ? viewer->getSceneViewport() : nullptr;
    }

    // Selects the knob's exact paths; patterns stay unselected.
    void selectCurrent()
    {
        QItemSelection selection;
        QModelIndex last;
        for (const QString& path : current) {
            if (path.contains(QLatin1Char('*')) || path.contains(QLatin1Char('?')) || path.contains(QLatin1Char('['))) {
                continue;
            }
            const QModelIndex index = model->indexOfPath(path, true);
            if (!index.isValid()) {
                continue;
            }
            for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
                tree->expand(parent);
            }
            selection.select(index, index.sibling(index.row(), SceneGraphModel::eColumnCount - 1));
            last = index;
            if (!multiple) {
                break;
            }
        }
        tree->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
        if (last.isValid()) {
            tree->selectionModel()->setCurrentIndex(last, QItemSelectionModel::NoUpdate);
            tree->scrollTo(last);
        }
    }
};

ScenePrimPickerPopup::ScenePrimPickerPopup(Gui* gui,
                                           const NodePtr& sceneNode,
                                           bool multiple,
                                           const QStringList& current,
                                           double time,
                                           QWidget* parent)
    : QDialog(parent)
    , _imp(new Implementation)
{
    _imp->gui = gui;
    _imp->multiple = multiple;
    _imp->current = current;

    setObjectName(QString::fromUtf8("scenePrimPicker"));
    setWindowTitle(multiple ? tr("Pick Prims") : tr("Pick Prim"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(480, 560);

    QVBoxLayout* layout = new QVBoxLayout(this);
    _imp->status = new QLabel(this);
    _imp->status->setObjectName(QString::fromUtf8("scenePrimPickerStatus"));
    _imp->status->setWordWrap(true);
    layout->addWidget(_imp->status);

    _imp->model = new SceneGraphModel(this);
    _imp->tree = new QTreeView(this);
    _imp->tree->setObjectName(QString::fromUtf8("scenePrimPickerTree"));
    _imp->tree->setModel(_imp->model);
    _imp->tree->setSelectionMode(multiple ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
    _imp->tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->tree->setUniformRowHeights(true);
    _imp->tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _imp->tree->header()->setStretchLastSection(false);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnName, QHeaderView::Stretch);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnType, QHeaderView::ResizeToContents);
    _imp->tree->header()->setSectionResizeMode(SceneGraphModel::eColumnKind, QHeaderView::ResizeToContents);
    layout->addWidget(_imp->tree, 1);

    _imp->buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    _imp->buttons->setObjectName(QString::fromUtf8("scenePrimPickerButtons"));
    _imp->useViewer = _imp->buttons->addButton(tr("Use Viewer Selection"), QDialogButtonBox::ActionRole);
    _imp->useViewer->setObjectName(QString::fromUtf8("scenePrimPickerUseViewer"));
    _imp->useViewer->setToolTip(tr("Takes the prims selected in the 3D view instead of those picked here."));
    layout->addWidget(_imp->buttons);

    QObject::connect(_imp->buttons, &QDialogButtonBox::accepted, this, &ScenePrimPickerPopup::onAccepted);
    QObject::connect(_imp->buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(_imp->useViewer, &QPushButton::clicked, this, &ScenePrimPickerPopup::onUseViewerSelection);
    QObject::connect(_imp->tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ScenePrimPickerPopup::updateButtons);
    QObject::connect(_imp->tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!_imp->multiple && index.isValid()) {
            onAccepted();
        }
    });
    _imp->viewport = _imp->selectionViewport();
    if (_imp->viewport) {
        QObject::connect(_imp->viewport.data(), &SceneViewport::selectionChanged, this, &ScenePrimPickerPopup::updateButtons);
    }

    if (sceneNode) {
        _imp->status->setText(tr("Reading the scene of %1...").arg(QString::fromStdString(sceneNode->getLabel())));
        QPointer<ScenePrimPickerPopup> self(this);
        _imp->source = std::make_shared<SceneNodeStageSource>(sceneNode, this, [self](bool ok, const QString& error) {
            if (self) {
                self->onSynced(ok, error);
            }
        });
        _imp->source->request(time);
        _imp->model->setSource(_imp->source);
    } else {
        _imp->status->setText(tr("The node's input is not connected, so there is no scene to pick from."));
    }
    updateButtons();
}

ScenePrimPickerPopup::~ScenePrimPickerPopup()
{
    if (_imp->source) {
        _imp->source->stop();
    }
}

QStringList
ScenePrimPickerPopup::getTreeSelection() const
{
    QStringList paths;
    for (const QModelIndex& row : _imp->tree->selectionModel()->selectedRows(0)) {
        paths << _imp->model->pathOf(row);
    }

    return paths;
}

void
ScenePrimPickerPopup::onSynced(bool ok,
                               const QString& error)
{
    _imp->model->refresh();
    if (!ok) {
        _imp->status->setText(error);
        updateButtons();

        return;
    }
    _imp->status->setText(_imp->multiple ? tr("Select the prims; Ctrl-click adds one, Shift-click a range.") : tr("Select one prim."));
    if (!_imp->synced) {
        _imp->synced = true;
        _imp->tree->expandToDepth(0);
        _imp->selectCurrent();
    }
    setProperty("natronPickerSynced", true);
    updateButtons();
}

void
ScenePrimPickerPopup::onAccepted()
{
    const QStringList paths = getTreeSelection();

    if (paths.isEmpty()) {
        return;
    }
    Q_EMIT primsPicked(paths);
    accept();
}

void
ScenePrimPickerPopup::onUseViewerSelection()
{
    QStringList paths = _imp->viewport ? _imp->viewport->getSelection() : QStringList();

    if (paths.isEmpty()) {
        return;
    }
    if (!_imp->multiple) {
        // The current prim is last.
        paths = QStringList() << paths.back();
    }
    Q_EMIT primsPicked(paths);
    accept();
}

void
ScenePrimPickerPopup::updateButtons()
{
    _imp->buttons->button(QDialogButtonBox::Ok)->setEnabled(!getTreeSelection().isEmpty());
    const bool viewerSelection = _imp->viewport && !_imp->viewport->getSelection().isEmpty();
    _imp->useViewer->setEnabled(viewerSelection);
    _imp->useViewer->setToolTip(viewerSelection ? tr("Takes the prims selected in the 3D view instead of those picked here.")
                                                : tr("No prims are selected in the 3D view."));
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ScenePrimPickerPopup.cpp"
