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

#include "KnobGuiTable.h"

#include <sstream> // stringstream

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QItemSelectionModel>
#include <QHeaderView>
#include <QPainter>
#include <QStyle>
#include <QApplication>
#include <QStyledItemDelegate>

#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Utils.h" // convertFromPlainText

#include "Gui/Button.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/NewLayerDialog.h"
#include "Gui/TableModelView.h"

NATRON_NAMESPACE_ENTER


NATRON_NAMESPACE_ANONYMOUS_ENTER

struct Row
{
    std::vector<TableItem*> items;
};

typedef std::vector<Row> Variables;

NATRON_NAMESPACE_ANONYMOUS_EXIT


struct KnobGuiTablePrivate
{
    std::string encodeTable(const KnobTablePtr& knob) const;

    void createItem(const KnobTablePtr& knob, int row, const QStringList& values);

    QWidget* mainContainer;
    TableView *table;
    TableModel* model;
    Button *addPathButton;
    Button* removePathButton;
    Button* editPathButton;
    bool isInsertingItem;
    Variables items;
    bool dragAndDropping;

    KnobGuiTablePrivate()
        : mainContainer(0)
        , table(0)
        , model(0)
        , addPathButton(0)
        , removePathButton(0)
        , editPathButton(0)
        , isInsertingItem(false)
        , items()
        , dragAndDropping(false)
    {
    }
};

KnobGuiTable::KnobGuiTable(KnobIPtr knob,
                           KnobGuiContainerI *container)
    : KnobGui(knob, container)
    , _imp( new KnobGuiTablePrivate() )
{
}

KnobGuiTable::~KnobGuiTable()
{
}

void
KnobGuiTable::removeSpecificGui()
{
    if (_imp->mainContainer) {
        _imp->mainContainer->deleteLater();
    }
}

int
KnobGuiTable::rowCount() const
{
    return _imp->model->rowCount();
}

////////////// TableView delegate

class KnobTableItemDelegate
    : public QStyledItemDelegate
{
    TableView* _view;
    KnobTableWPtr _knob;

public:

    explicit KnobTableItemDelegate(const KnobTablePtr& knob,
                                   TableView* view);

private:

    virtual void paint(QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index) const OVERRIDE FINAL;
};

KnobTableItemDelegate::KnobTableItemDelegate(const KnobTablePtr& knob,
                                             TableView* view)
    : QStyledItemDelegate(view)
    , _view(view)
    , _knob(knob)
{
}

void
KnobTableItemDelegate::paint(QPainter * painter,
                             const QStyleOptionViewItem & option,
                             const QModelIndex & index) const
{
    if (!index.isValid() || option.state & QStyle::State_Selected) {
        QStyledItemDelegate::paint(painter, option, index);

        return;
    }

    KnobTablePtr knob = _knob.lock();
    if (!knob) {
        return;
    }


    TableModel* model = dynamic_cast<TableModel*>( _view->model() );
    assert(model);
    if (!model) {
        // coverity[dead_error_begin]
        QStyledItemDelegate::paint(painter, option, index);

        return;
    }
    TableItem* item = model->item(index);
    if (!item) {
        QStyledItemDelegate::paint(painter, option, index);

        return;
    }
    QPen pen;

    if ( !item->flags().testFlag(Qt::ItemIsEnabled) ) {
        pen.setColor(Qt::black);
    } else {
        pen.setColor( QColor(200, 200, 200) );
    }
    painter->setPen(pen);

    // get the proper subrect from the style
    QStyle *style = QApplication::style();
    QRect geom = style->subElementRect(QStyle::SE_ItemViewItemText, &option);

    ///Draw the item name column
    if (option.state & QStyle::State_Selected) {
        painter->fillRect( geom, option.palette.highlight() );
    }
    QRect r;
    QString str = item->data(Qt::DisplayRole).toString();
    bool decorateWithBrackets =  knob->isCellBracketDecorated( index.row(), index.column() );

    if (decorateWithBrackets) {
        ///Env vars are used between brackets
        str.prepend( QLatin1Char('[') );
        str.append( QLatin1Char(']') );
    }
    painter->drawText(geom, Qt::TextSingleLine, str, &r);
} // KnobTableItemDelegate::paint

void
KnobGuiTable::createWidget(QHBoxLayout* layout)
{
    KnobTablePtr knob = std::dynamic_pointer_cast<KnobTable>( getKnob() );

    assert(knob);

    _imp->mainContainer = new QWidget( layout->parentWidget() );
    QVBoxLayout* mainLayout = new QVBoxLayout(_imp->mainContainer);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    _imp->table = new TableView(_imp->mainContainer);
    QObject::connect( _imp->table, SIGNAL(aboutToDrop()), this, SLOT(onItemAboutToDrop()) );
    QObject::connect( _imp->table, SIGNAL(itemDropped()), this, SLOT(onItemDropped()) );
    QObject::connect( _imp->table, SIGNAL(itemDoubleClicked(TableItem*)), this, SLOT(onItemDoubleClicked(TableItem*)) );

    layout->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    _imp->table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _imp->table->setAttribute(Qt::WA_MacShowFocusRect, 0);
    _imp->table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _imp->table->setDragDropMode(QAbstractItemView::InternalMove);
    _imp->table->header()->setStretchLastSection(true);
    _imp->table->setUniformRowHeights(true);
    _imp->table->setItemDelegate( new KnobTableItemDelegate(knob, _imp->table) );

    _imp->model = new TableModel(0, 0, _imp->table);
    QObject::connect( _imp->model, SIGNAL(s_itemChanged(TableItem*)), this, SLOT(onItemDataChanged(TableItem*)) );


    _imp->table->setTableModel(_imp->model);
    const int numCols = knob->getColumnsCount();
    _imp->table->setColumnCount(numCols);
    if (numCols == 1) {
        _imp->table->header()->hide();
    }
    QStringList headers;
    for (int i = 0; i < numCols; ++i) {
        headers.push_back( QString::fromUtf8( knob->getColumnLabel(i).c_str() ) );
    }
    _imp->table->setHorizontalHeaderLabels(headers);

    ///set the copy/link actions in the right click menu
    enableRightClickMenu(_imp->table, 0);

    QWidget* buttonsContainer = new QWidget(_imp->mainContainer);
    QHBoxLayout* buttonsLayout = new QHBoxLayout(buttonsContainer);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);

    _imp->addPathButton = new Button( tr("Add..."), buttonsContainer );
    _imp->addPathButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Add a new value"), NATRON_NAMESPACE::WhiteSpaceNormal) );

    QObject::connect( _imp->addPathButton, SIGNAL(clicked()), this, SLOT(onAddButtonClicked()) );

    _imp->removePathButton = new Button( tr("Remove"), buttonsContainer);
    QObject::connect( _imp->removePathButton, SIGNAL(clicked()), this, SLOT(onRemoveButtonClicked()) );
    _imp->removePathButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Remove selected values"), NATRON_NAMESPACE::WhiteSpaceNormal) );

    _imp->editPathButton = new Button( tr("Edit..."), buttonsContainer);
    QObject::connect( _imp->editPathButton, SIGNAL(clicked()), this, SLOT(onEditButtonClicked()) );
    _imp->editPathButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Click to edit selected value."), NATRON_NAMESPACE::WhiteSpaceNormal) );


    buttonsLayout->addWidget(_imp->addPathButton);
    buttonsLayout->addWidget(_imp->removePathButton);
    buttonsLayout->addWidget(_imp->editPathButton);
    createExtraButtons(buttonsContainer, buttonsLayout);
    buttonsLayout->addStretch();

    mainLayout->addWidget(_imp->table);
    mainLayout->addWidget(buttonsContainer);

    if ( !knob->useEditButton() ) {
        _imp->editPathButton->hide();
    }

    layout->addWidget(_imp->mainContainer);
} // KnobGuiTable::createWidget

std::string
KnobGuiTablePrivate::encodeTable(const KnobTablePtr& knob) const
{
    std::stringstream ss;

    for (Variables::const_iterator it = items.begin(); it != items.end(); ++it) {
        // In order to use XML tags, the text inside the tags has to be escaped.
        for (std::size_t c = 0; c < it->items.size(); ++c) {
            std::string label = knob->getColumnLabel(c);
            ss << "<" << label << ">";
            ss << Project::escapeXML( it->items[c]->text().toStdString() );
            ss << "</" << label << ">";
        }
    }

    return ss.str();
}

void
KnobGuiTablePrivate::createItem(const KnobTablePtr& knob,
                                int row,
                                const QStringList& values)
{
    assert( values.size() == knob->getColumnsCount() );
    int col = 0;
    Row r;

    for (QStringList::const_iterator it = values.begin(); it != values.end(); ++it, ++col) {
        Qt::ItemFlags flags;
        if ( knob->isCellEnabled(row, col, values) ) {
            flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if ( knob->isColumnEditable(col) ) {
                flags |= Qt::ItemIsEditable;
            }

            if (values.size() == 1) {
                flags |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsUserCheckable;
            }
        }
        TableItem* cell = new TableItem;
        cell->setText(*it);
        cell->setFlags(flags);
        r.items.push_back(cell);
    }

    if ( row >= (int)items.size() ) {
        items.push_back(r);
    } else {
        std::vector<Row>::iterator it = items.begin();
        std::advance(it, row);
        items.insert(it, r);
    }
    int modelRowCount = model->rowCount();
    if (row >= modelRowCount) {
        model->insertRow(row);
    }
    isInsertingItem = true;
    for (std::size_t i = 0; i < r.items.size(); ++i) {
        table->setItem(row, i, r.items[i]);
        table->resizeColumnToContents(i);
    }
    isInsertingItem = false;
}

void
KnobGuiTable::onAddButtonClicked()
{
    KnobTablePtr knob = std::dynamic_pointer_cast<KnobTable>( getKnob() );

    assert(knob);

    const int numCols = knob->getColumnsCount();
    std::string oldTable = knob->getValue();
    QStringList row;

    if (numCols == 1) {
        QStringList existingEntries;
        for (Variables::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it) {
            existingEntries.push_back( it->items[0]->text() );
        }

        QString newItemName = QString::fromUtf8("Placeholder");
        int i = 1;
        while ( existingEntries.contains(newItemName) ) {
            newItemName = QString::fromUtf8("Placeholder") + QString::number(i);
            ++i;
        }
        row.push_back(newItemName);
    } else {
        if ( !addNewUserEntry(row) ) {
            return;
        }
    }

    int rowCount = (int)_imp->items.size();

    _imp->createItem(knob, rowCount, row);
    std::string newTable = _imp->encodeTable(knob);
    pushUndoCommand( new KnobUndoCommand<std::string>( shared_from_this(), oldTable, newTable) );
}

void
KnobGuiTable::onEditButtonClicked()
{
    KnobTablePtr knob = std::dynamic_pointer_cast<KnobTable>( getKnob() );

    assert(knob);
    std::string oldTable = knob->getValue();
    QModelIndexList selection = _imp->table->selectionModel()->selectedRows();

    if (selection.size() != 1) {
        return;
    }

    if ( (selection[0].row() >= 0) && ( selection[0].row() < (int)_imp->items.size() ) ) {
        Row& r = _imp->items[selection[0].row()];
        QStringList row;
        for (std::size_t i = 0; i < r.items.size(); ++i) {
            row.push_back( r.items[i]->text() );
        }
        if ( !editUserEntry(row) ) {
            return;
        }

        for (std::size_t i = 0; i < r.items.size(); ++i) {
            r.items[i]->setText(row[i]);
        }

        std::string newTable = _imp->encodeTable(knob);
        pushUndoCommand( new KnobUndoCommand<std::string>( shared_from_this(), oldTable, newTable) );
    }
}

void
KnobGuiTable::onRemoveButtonClicked()
{
    KnobTablePtr knob = std::dynamic_pointer_cast<KnobTable>( getKnob() );

    assert(knob);
    QModelIndexList selection = _imp->table->selectionModel()->selectedRows();

    if (selection.size() < 1) {
        return;
    }


    for (int i = 0; i < selection.size(); ++i) {
        QStringList removedRow;
        if ( (selection[0].row() >= 0) && ( selection[0].row() < (int)_imp->items.size() ) ) {
            Row& r = _imp->items[selection[0].row()];
            for (std::size_t i = 0; i < r.items.size(); ++i) {
                removedRow.push_back( r.items[i]->text() );
            }
            _imp->items.erase( _imp->items.begin() + selection[0].row() );
        }

        entryRemoved(removedRow);
    }

    _imp->model->removeRows( selection.front().row(), selection.size() );


    std::string oldTable = knob->getValue();
    std::string newTable = _imp->encodeTable(knob);
    pushUndoCommand( new KnobUndoCommand<std::string>( shared_from_this(), oldTable, newTable) );
}

void
KnobGuiTable::updateGUI(int /*dimension*/)
{
    KnobTablePtr knob = std::dynamic_pointer_cast<KnobTable>( getKnob() );

    assert(knob);

    std::list<std::vector<std::string> > table;
    knob->getTable(&table);

    _imp->model->clear();
    // clear() deletes the items but keeps the row count; drop the rows so a shrinking
    // table does not leave empty rows behind.
    if (_imp->model->rowCount() > 0) {
        _imp->model->removeRows(0, _imp->model->rowCount());
    }
    _imp->items.clear();
    int i = 0;
    for (std::list<std::vector<std::string> >::const_iterator it = table.begin(); it != table.end(); ++it, ++i) {
        QStringList row;
        for (std::size_t j = 0; j < it->size(); ++j) {
            row.push_back( QString::fromUtf8( (*it)[j].c_str() ) );
        }
        _imp->createItem(knob, i, row);
    }
}

void
KnobGuiTable::_hide()
{
    if (_imp->mainContainer) {
        _imp->mainContainer->hide();
    }
}

void
KnobGuiTable::_show()
{
    if (_imp->mainContainer) {
        _imp->mainContainer->show();
    }
}

void
KnobGuiTable::setEnabled()
{
    bool enabled = getKnob()->isEnabled(0);

    if (_imp->table) {
        _imp->table->setEnabled(enabled);
        _imp->addPathButton->setEnabled(enabled);
        _imp->removePathButton->setEnabled(enabled);
    }
}

void
KnobGuiTable::setReadOnly(bool readOnly,
                          int /*dimension*/)
{
    if (_imp->table) {
        _imp->table->setEnabled(!readOnly);
        _imp->addPathButton->setEnabled(!readOnly);
        _imp->removePathButton->setEnabled(!readOnly);
    }
}

void
KnobGuiTable::onItemAboutToDrop()
{
    _imp->dragAndDropping = true;
}

void
KnobGuiTable::onItemDropped()
{
    _imp->items.clear();

    ///Rebuild the mapping
    int rowCount = _imp->table->rowCount();
    int colCount = _imp->table->columnCount();
    for (int i = 0; i < rowCount; ++i) {
        Row& r = _imp->items[i];
        r.items.resize(colCount);
        for (int j = 0; j < colCount; ++j) {
            r.items[j] = _imp->table->item(i, j);
        }
    }
    _imp->dragAndDropping = false;

    //Now refresh the knob balue
    onItemDataChanged(0);
}

void
KnobGuiTable::onItemDoubleClicked(TableItem* item)
{
    int row = -1;
    int col = -1;

    for (std::size_t i = 0; i < _imp->items.size(); ++i) {
        for (std::size_t j = 0; j < _imp->items[i].items.size(); ++j) {
            if (_imp->items[i].items[j] == item) {
                row = i;
                col = j;
                break;
            }
        }
        if (row != -1) {
            break;
        }
    }
    assert(row >= 0 && col >= 0);
    KnobTablePtr knob = std::dynamic_pointer_cast<KnobTable>( getKnob() );
    assert(knob);
    if ( !knob || (row < 0) || (col < 0) ) {
        return;
    }

    Row& r = _imp->items[row];
    QStringList values;
    for (std::size_t i = 0; i < r.items.size(); ++i) {
        values.push_back( r.items[i]->text() );
    }

    // Not enabled
    if ( !knob->isCellEnabled(row, col, values) ) {
        return;
    }

    // Let the standard editor
    if ( knob->isColumnEditable(col) ) {
        return;
    }
    if ( !editUserEntry(values) ) {
        return;
    }

    for (std::size_t i = 0; i < r.items.size(); ++i) {
        r.items[i]->setText(values[i]);
    }

    std::string oldTable = knob->getValue();
    std::string newTable = _imp->encodeTable(knob);
    pushUndoCommand( new KnobUndoCommand<std::string>( shared_from_this(), oldTable, newTable) );
} // KnobGuiTable::onItemDoubleClicked

void
KnobGuiTable::onItemDataChanged(TableItem* item)
{
    if (_imp->isInsertingItem || _imp->dragAndDropping) {
        return;
    }

    int row = -1;
    int col = -1;
    for (std::size_t i = 0; i < _imp->items.size(); ++i) {
        for (std::size_t j = 0; j < _imp->items[i].items.size(); ++j) {
            if (_imp->items[i].items[j] == item) {
                row = i;
                col = j;
                break;
            }
        }
        if (row != -1) {
            break;
        }
    }

    if (row != -1) {
        KnobTablePtr knob = std::dynamic_pointer_cast<KnobTable>( getKnob() );
        assert(knob);

        std::string oldValue = knob->getValue();
        std::string newValue = _imp->encodeTable(knob);
        if (oldValue != newValue) {
            tableChanged(row, col, &newValue);
            pushUndoCommand( new KnobUndoCommand<std::string>( shared_from_this(), oldValue, newValue) );
        }
    }
}

void
KnobGuiTable::updateToolTip()
{
    if ( hasToolTip() ) {
        QString tt = toolTip();
        _imp->table->setToolTip(tt);
    }
}

QStringList
KnobGuiTable::rowValues(int row) const
{
    QStringList values;

    if ((row < 0) || (row >= (int)_imp->items.size())) {
        return values;
    }
    const Row& r = _imp->items[row];
    for (std::size_t i = 0; i < r.items.size(); ++i) {
        values.push_back(r.items[i]->text());
    }

    return values;
}

QList<int>
KnobGuiTable::selectedRowIndices() const
{
    QList<int> rows;
    QModelIndexList selection = _imp->table->selectionModel()->selectedRows();

    for (int i = 0; i < selection.size(); ++i) {
        rows.push_back(selection[i].row());
    }

    return rows;
}

KnobGuiLayers::KnobGuiLayers(KnobIPtr knob,
                             KnobGuiContainerI *container)
    : KnobGuiTable(knob, container)
{
    _knob = std::dynamic_pointer_cast<KnobLayers>(knob);
}

KnobGuiLayers::~KnobGuiLayers()
{
}

ProjectPtr
KnobGuiLayers::getProject() const
{
    Gui* gui = getGui();

    if (!gui) {
        return ProjectPtr();
    }
    GuiAppInstancePtr app = gui->getApp();
    if (!app) {
        return ProjectPtr();
    }

    return app->getProject();
}

void
KnobGuiLayers::createExtraButtons(QWidget* parent,
                                  QHBoxLayout* layout)
{
    Button* removeUnusedButton = new Button(tr("Remove unused"), parent);

    removeUnusedButton->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Remove every layer that is not built-in and that no node references."), NATRON_NAMESPACE::WhiteSpaceNormal));
    QObject::connect(removeUnusedButton, SIGNAL(clicked()), this, SLOT(onRemoveUnusedButtonClicked()));
    layout->addWidget(removeUnusedButton);
}

void
KnobGuiLayers::onAddButtonClicked()
{
    ProjectPtr project = getProject();

    if (!project) {
        return;
    }
    NewLayerDialog dialog(ImageLayerDesc::getNoneComponents(), getGui());
    if (!dialog.exec()) {
        return;
    }

    std::string error;
    LayerRegistry::AddResultEnum ret = project->addLayer(dialog.getComponents(), LayerRegistryEntry::eOriginUser, &error);
    if (ret == LayerRegistry::eAddResultRefused) {
        Dialogs::errorDialog(tr("Layer").toStdString(), error);
    }
}

void
KnobGuiLayers::onRemoveButtonClicked()
{
    ProjectPtr project = getProject();

    if (!project) {
        return;
    }
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();
    QList<int> rows = selectedRowIndices();
    std::list<std::string> ids;
    for (int i = 0; i < rows.size(); ++i) {
        QStringList values = rowValues(rows[i]);
        if (values.isEmpty()) {
            continue;
        }
        const std::string label = values[0].toStdString();
        for (std::size_t e = 0; e < snapshot->size(); ++e) {
            if ((*snapshot)[e].desc.getLayerLabel() == label) {
                ids.push_back((*snapshot)[e].desc.getLayerID());
                break;
            }
        }
    }

    for (std::list<std::string>::const_iterator it = ids.begin(); it != ids.end(); ++it) {
        std::string error;
        if (!project->removeLayer(*it, &error)) {
            Dialogs::errorDialog(tr("Layer").toStdString(), error);
        }
    }
}

void
KnobGuiLayers::onRemoveUnusedButtonClicked()
{
    ProjectPtr project = getProject();

    if (!project) {
        return;
    }
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();
    for (std::size_t e = 0; e < snapshot->size(); ++e) {
        const LayerRegistryEntry& entry = (*snapshot)[e];
        if (entry.origin == LayerRegistryEntry::eOriginBuiltin) {
            continue;
        }
        std::list<NodePtr> users;
        project->getLayerUsers(entry.desc.getLayerID(), &users);
        if (!users.empty()) {
            continue;
        }
        project->removeLayer(entry.desc.getLayerID(), 0);
    }
}

KnobIPtr
KnobGuiLayers::getKnob() const
{
    return _knob.lock();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiTable.cpp"
