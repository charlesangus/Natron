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

#include "KnobGuiShuffleMap.h"

#include <algorithm>
#include <list>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QButtonGroup>
#include <QColor>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"

#include "Gui/Button.h"
#include "Gui/ChannelColor.h"
#include "Gui/Gui.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobGuiLayerSelect.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Label.h"

NATRON_NAMESPACE_ENTER

struct KnobGuiShuffleMapPrivate {
    struct Column {
        ShuffleSource src;
        std::string channel;
        QString text;
        QString toolTip;
        bool enabled;

        Column()
            : src()
            , channel()
            , text()
            , toolTip()
            , enabled(true)
        {
        }

        bool operator==(const Column& other) const
        {
            return src == other.src && channel == other.channel && text == other.text && toolTip == other.toolTip && enabled == other.enabled;
        }
    };

    struct Row {
        int outSlot;
        int outIndex;
        std::string channel;
        QString toolTip;

        Row()
            : outSlot(0)
            , outIndex(0)
            , channel()
            , toolTip()
        {
        }

        bool operator==(const Row& other) const
        {
            return outSlot == other.outSlot && outIndex == other.outIndex && channel == other.channel && toolTip == other.toolTip;
        }
    };

    /// The count columns (slot blocks) or rows (output blocks) from first; count is 0 for a None slot.
    struct Group {
        int slot;
        int first;
        int count;

        Group()
            : slot(0)
            , first(0)
            , count(0)
        {
        }

        bool operator==(const Group& other) const
        {
            return slot == other.slot && first == other.first && count == other.count;
        }
    };

    struct Layout {
        std::vector<Column> columns;
        std::vector<Row> rows;
        std::vector<Group> slotGroups;
        std::vector<Group> outputGroups;

        bool operator==(const Layout& other) const
        {
            return columns == other.columns && rows == other.rows && slotGroups == other.slotGroups && outputGroups == other.outputGroups;
        }
    };

    std::weak_ptr<KnobShuffleMap> knob;
    QWidget* matrix;
    QGridLayout* grid;
    Button* resetButton;
    LayerChannelRow* layerRows[4];
    Layout layout;
    std::vector<std::vector<Button*>> cells;
    std::vector<QButtonGroup*> groups;
    std::vector<QWidget*> gridWidgets;
    int layerEditDepth;

    KnobGuiShuffleMapPrivate()
        : knob()
        , matrix(0)
        , grid(0)
        , resetButton(0)
        , layout()
        , cells()
        , groups()
        , gridWidgets()
        , layerEditDepth(0)
    {
        for (int i = 0; i < 4; ++i) {
            layerRows[i] = 0;
        }
    }
};

namespace {
typedef KnobGuiShuffleMapPrivate::Layout MatrixLayout;

const char* const kLayerKnobNames[4] = {
    kShuffleParamIn1, kShuffleParamIn2, kShuffleParamOut1, kShuffleParamOut2
};

QString
qs(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}

KnobLayerSelectPtr
findLayerKnob(const NodePtr& node,
              int which)
{
    if (!node || which < 0 || which >= 4) {
        return KnobLayerSelectPtr();
    }

    return std::dynamic_pointer_cast<KnobLayerSelect>(node->getKnobByName(kLayerKnobNames[which]));
}

bool
findLayerDesc(const std::list<ImageLayerDesc>& layers,
              const std::string& layerID,
              ImageLayerDesc* desc)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->getLayerID() == layerID) {
            *desc = *it;

            return true;
        }
    }

    return false;
}

bool
findRegistryLayer(const NodePtr& node,
                  const std::string& layerID,
                  ImageLayerDesc* desc)
{
    AppInstancePtr app = node->getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!project) {
        return false;
    }
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();
    for (std::vector<LayerRegistryEntry>::const_iterator it = snapshot->begin(); it != snapshot->end(); ++it) {
        if (it->desc.getLayerID() == layerID) {
            *desc = it->desc;

            return true;
        }
    }

    return false;
}

QString
layerLabel(const ImageLayerDesc& desc)
{
    return desc.getLayerLabel().empty() ? qs(desc.getLayerID()) : qs(desc.getLayerLabel());
}

// A layer whose channels are unknown still needs a column or row for every index the
// mapping names, or the stored source would have nothing to show it on.
void
padChannels(std::vector<std::string>* channels,
            int minCount)
{
    for (int i = (int)channels->size(); i < minCount; ++i) {
        channels->push_back(QString::number(i).toStdString());
    }
}

void
appendSlotColumns(const NodePtr& node,
                  Shuffle* shuffle,
                  const std::vector<ShuffleMapRow>& mapRows,
                  int slot,
                  MatrixLayout* layout)
{
    KnobGuiShuffleMapPrivate::Group group;

    group.slot = slot;
    group.first = (int)layout->columns.size();

    const std::string layerID = shuffle->getSlotLayer(slot);
    if (layerID.empty()) {
        layout->slotGroups.push_back(group);

        return;
    }

    std::list<ImageLayerDesc> present;
    node->listLayersForKnob(node->getKnobByName(slot == 1 ? kShuffleParamIn1 : kShuffleParamIn2), &present);
    ImageLayerDesc desc;
    const bool listed = findLayerDesc(present, layerID, &desc);
    const bool known = listed || findRegistryLayer(node, layerID, &desc);

    std::vector<std::string> channels;
    if (known) {
        channels = desc.getChannels();
    }
    const QString label = known ? layerLabel(desc) : qs(layerID);

    if (!known) {
        int maxIndex = -1;
        for (std::size_t i = 0; i < mapRows.size(); ++i) {
            if (mapRows[i].src.kind == ShuffleSource::eInput && mapRows[i].src.slot == slot) {
                maxIndex = std::max(maxIndex, mapRows[i].src.index);
            }
        }
        padChannels(&channels, maxIndex + 1);
    }

    group.count = (int)channels.size();
    layout->slotGroups.push_back(group);

    for (std::size_t i = 0; i < channels.size(); ++i) {
        KnobGuiShuffleMapPrivate::Column column;
        column.src = ShuffleSource::makeInput(slot, (int)i);
        column.channel = channels[i];
        column.text = qs(channels[i]);
        column.toolTip = QString::fromUtf8("in%1 %2.%3").arg(slot).arg(label).arg(qs(channels[i]));
        column.enabled = listed;
        layout->columns.push_back(column);
    }
}

void
appendOutputRows(const NodePtr& node,
                 Shuffle* shuffle,
                 const std::vector<ShuffleMapRow>& mapRows,
                 int slot,
                 MatrixLayout* layout)
{
    KnobGuiShuffleMapPrivate::Group group;

    group.slot = slot;
    group.first = (int)layout->rows.size();

    const std::string layerID = shuffle->getOutputLayer(slot);
    if (layerID.empty()) {
        layout->outputGroups.push_back(group);

        return;
    }

    std::list<ImageLayerDesc> registered;
    node->listLayersForKnob(node->getKnobByName(slot == 1 ? kShuffleParamOut1 : kShuffleParamOut2), &registered);
    ImageLayerDesc desc;
    const bool listed = findLayerDesc(registered, layerID, &desc);

    std::vector<std::string> channels;
    if (listed) {
        channels = desc.getChannels();
    }
    const QString label = listed ? layerLabel(desc) : qs(layerID);

    if (!listed) {
        int maxIndex = -1;
        for (std::size_t i = 0; i < mapRows.size(); ++i) {
            if (mapRows[i].outSlot == slot) {
                maxIndex = std::max(maxIndex, mapRows[i].outIndex);
            }
        }
        padChannels(&channels, maxIndex + 1);
    }

    group.count = (int)channels.size();
    layout->outputGroups.push_back(group);

    for (std::size_t i = 0; i < channels.size(); ++i) {
        KnobGuiShuffleMapPrivate::Row row;
        row.outSlot = slot;
        row.outIndex = (int)i;
        row.channel = channels[i];
        row.toolTip = label + QLatin1Char('.') + qs(channels[i]);
        layout->rows.push_back(row);
    }
}

void
appendConstantColumn(const ShuffleSource& src,
                     const QString& text,
                     const QString& toolTip,
                     MatrixLayout* layout)
{
    KnobGuiShuffleMapPrivate::Column column;

    column.src = src;
    column.text = text;
    column.toolTip = toolTip;
    layout->columns.push_back(column);
}

// Holds a gap column or row open. Its size along the other axis is 0 so it never widens
// the row or column it sits in, and along the gap axis it may grow: a dropdown heading a
// block that is narrower than itself spans into the next gap column, and the overflow
// must land there instead of widening the block's cell columns.
QWidget*
createGapWidget(QWidget* parent,
                Qt::Orientation gapAxis,
                int gap)
{
    QWidget* w = new QWidget(parent);

    if (gapAxis == Qt::Horizontal) {
        w->setMinimumWidth(gap);
        w->setFixedHeight(0);
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        w->setFixedHeight(gap);
        w->setMinimumWidth(0);
        w->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    }

    return w;
}
} // namespace

KnobGuiShuffleMap::KnobGuiShuffleMap(KnobIPtr knob,
                                     KnobGuiContainerI* container)
    : KnobGuiLayerChannelBase(knob, container)
    , _imp(new KnobGuiShuffleMapPrivate())
{
    _imp->knob = std::dynamic_pointer_cast<KnobShuffleMap>(knob);

    // The matrix's shape comes from these knobs, and a value change on them does not
    // always relist the node's layers.
    NodePtr node = getNode();
    if (node) {
        for (int i = 0; i < 4; ++i) {
            KnobIPtr sibling = node->getKnobByName(kLayerKnobNames[i]);
            KnobSignalSlotHandler* handler = sibling ? sibling->getSignalSlotHandler().get() : 0;
            if (handler) {
                QObject::connect(handler, &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec /*view*/, int /*dimension*/, int reason) {
                    if ((ValueChangedReasonEnum)reason != eValueChangedReasonTimeChanged) {
                        onLayerListRefreshed();
                    }
                });
            }
        }
    }
}

KnobGuiShuffleMap::~KnobGuiShuffleMap()
{
}

void
KnobGuiShuffleMap::removeSpecificGui()
{
    KnobGuiLayerChannelBase::removeSpecificGui();
    _imp->matrix = 0;
    _imp->grid = 0;
    _imp->resetButton = 0;
    for (int i = 0; i < 4; ++i) {
        _imp->layerRows[i] = 0;
    }
    _imp->cells.clear();
    _imp->groups.clear();
    _imp->gridWidgets.clear();
    _imp->layout = KnobGuiShuffleMapPrivate::Layout();
}

void
KnobGuiShuffleMap::createWidget(QHBoxLayout* layout)
{
    createContainer(layout);

    _imp->matrix = new QWidget(getContainer());
    getContainerLayout()->addWidget(_imp->matrix, 0, Qt::AlignLeft);
    createLayerRows();

    _imp->resetButton = new Button(tr("Reset"), getContainer());
    _imp->resetButton->setToolTip(tr("Set every output channel back to its default source."));
    QObject::connect(_imp->resetButton, &QPushButton::clicked, this, [this]() {
        onResetClicked();
    });
    getContainerLayout()->addWidget(_imp->resetButton, 0, Qt::AlignLeft);

    refresh(true);
}

void
KnobGuiShuffleMap::createLayerRows()
{
    NodePtr node = getNode();

    for (int i = 0; i < 4; ++i) {
        const LayerRowEnum which = (LayerRowEnum)i;
        LayerChannelRow* row = new LayerChannelRow(LayerChannelRow::eModeLayerSelect, _imp->matrix);
        KnobLayerSelectPtr knob = findLayerKnob(node, i);
        if (knob) {
            row->setAllowNone(knob->getAllowNone());
            row->setAbsentMarker(node->isTargetLayerKnob(knob) ? tr("(not in project)") : tr("(not in input)"));
            row->setToolTip(qs(knob->getHintToolTip()));
        } else {
            row->hide();
        }
        QObject::connect(row, &LayerChannelRow::layerChosen, this, [this, which](const QString& id) {
            onLayerRowChosen(which, id);
        });
        QObject::connect(row, &LayerChannelRow::newLayerRequested, this, [this, which]() {
            onLayerRowNewLayerRequested(which);
        });
        _imp->layerRows[i] = row;
    }
}

void
KnobGuiShuffleMap::refreshLayerRows()
{
    NodePtr node = getNode();

    for (int i = 0; i < 4; ++i) {
        LayerChannelRow* row = _imp->layerRows[i];
        KnobLayerSelectPtr knob = findLayerKnob(node, i);
        if (!row || !knob) {
            continue;
        }
        const std::vector<LayerChannelRow::LayerEntry> layers = listLayerEntriesForKnob(node, knob);
        if (!sameLayerEntries(row->getAvailableLayers(), layers)) {
            row->setAvailableLayers(layers, node->isTargetLayerKnob(knob));
        }
        const std::string layerID = knob->getLayer();
        if (row->getCurrentLayerID() != layerID) {
            row->setLayerSelectValue(layerID, std::vector<std::string>(), false);
        }
    }
}

void
KnobGuiShuffleMap::refreshWidgets()
{
    // A dropdown is still inside its own change signal while the layer it asked for is
    // applied, and repopulating it or re-laying it out there would pull the combo out
    // from under that signal.
    if (_imp->layerEditDepth > 0) {
        scheduleRefresh(true);

        return;
    }
    if (!_imp->matrix) {
        return;
    }

    std::shared_ptr<KnobShuffleMap> mapping = _imp->knob.lock();
    Shuffle* shuffle = mapping ? dynamic_cast<Shuffle*>(mapping->getHolder()) : 0;
    NodePtr node = getNode();

    KnobGuiShuffleMapPrivate::Layout layout;
    if (shuffle && node) {
        const std::vector<ShuffleMapRow> mapRows = mapping->getRows();
        appendSlotColumns(node, shuffle, mapRows, 1, &layout);
        appendSlotColumns(node, shuffle, mapRows, 2, &layout);
        appendConstantColumn(ShuffleSource::makeZero(), QString::fromUtf8("0"), tr("A constant 0."), &layout);
        appendConstantColumn(ShuffleSource::makeOne(), QString::fromUtf8("1"), tr("A constant 1."), &layout);
        appendOutputRows(node, shuffle, mapRows, 1, &layout);
        appendOutputRows(node, shuffle, mapRows, 2, &layout);
    }

    refreshLayerRows();

    if (!_imp->grid || !(layout == _imp->layout)) {
        _imp->layout = layout;
        rebuildMatrix();
    }
    syncCheckedButtons();
}

void
KnobGuiShuffleMap::rebuildMatrix()
{
    // Never reached from inside a cell's own click or a dropdown's own change: the base
    // class defers the refresh while a cell edit is applied, and refreshWidgets() does
    // while a dropdown edit is. The dropdowns outlive the grid; only its cells, labels
    // and gap holders are rebuilt.
    QWidget* matrix = _imp->matrix;

    if (!matrix) {
        return;
    }
    delete _imp->grid;
    _imp->grid = 0;
    for (std::size_t i = 0; i < _imp->gridWidgets.size(); ++i) {
        delete _imp->gridWidgets[i];
    }
    _imp->gridWidgets.clear();
    for (std::size_t i = 0; i < _imp->groups.size(); ++i) {
        delete _imp->groups[i];
    }
    _imp->groups.clear();
    _imp->cells.clear();

    const KnobGuiShuffleMapPrivate::Layout& layout = _imp->layout;

    QFontMetrics fm(matrix->font());
    {
        Button probe(QString::fromUtf8("0"), matrix);
        probe.ensurePolished();
        fm = probe.fontMetrics();
    }
    int widestText = fm.horizontalAdvance(QLatin1Char('W'));
    for (std::size_t c = 0; c < layout.columns.size(); ++c) {
        widestText = std::max(widestText, fm.horizontalAdvance(layout.columns[c].text));
    }
    // Each out dropdown sits in its block's first row, so a cell is at least as tall as the
    // dropdown: a taller dropdown would otherwise make that one row taller than the rest.
    int dropdownHeight = 0;
    for (int i = eLayerRowOut1; i <= eLayerRowOut2; ++i) {
        if (_imp->layerRows[i] && !_imp->layerRows[i]->isHidden()) {
            dropdownHeight = std::max(dropdownHeight, _imp->layerRows[i]->sizeHint().height());
        }
    }
    const int cellHeight = std::max(fm.height() + 4, dropdownHeight);
    const int cellWidth = std::max(cellHeight, widestText + 8);
    const int spacing = 2;
    const int gap = std::max(6, cellHeight / 2);

    QGridLayout* grid = new QGridLayout(matrix);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(spacing);
    grid->setVerticalSpacing(spacing);
    // The grid never stretches, so every cell column and row keeps its natural size and
    // the steps between them stay constant.
    grid->setSizeConstraint(QLayout::SetFixedSize);
    _imp->grid = grid;

    const int headerRow = 0;
    const int firstBodyRow = 1;

    // Columns: [in1 block][gap][in2 block][gap][0][1][gap][-> channel][out dropdown].
    // A None slot keeps one empty column so its dropdown still has a place.
    std::vector<int> gridColumns(layout.columns.size(), -1);
    int gridColumn = 0;
    for (std::size_t i = 0; i < layout.slotGroups.size() && i < 2; ++i) {
        const KnobGuiShuffleMapPrivate::Group& group = layout.slotGroups[i];
        const int blockFirst = gridColumn;
        for (int c = 0; c < group.count; ++c) {
            gridColumns[group.first + c] = blockFirst + c;
        }
        const int blockWidth = std::max(group.count, 1);
        const int gapColumn = blockFirst + blockWidth;
        QWidget* gapWidget = createGapWidget(matrix, Qt::Horizontal, gap);
        _imp->gridWidgets.push_back(gapWidget);
        grid->addWidget(gapWidget, firstBodyRow, gapColumn);

        LayerChannelRow* slotRow = _imp->layerRows[group.slot == 1 ? eLayerRowIn1 : eLayerRowIn2];
        if (slotRow) {
            grid->addWidget(slotRow, headerRow, blockFirst, 1, blockWidth + 1, Qt::AlignLeft | Qt::AlignBottom);
        }
        gridColumn = gapColumn + 1;
    }
    for (std::size_t c = 0; c < layout.columns.size(); ++c) {
        if (gridColumns[c] < 0) {
            gridColumns[c] = gridColumn++;
        }
    }
    {
        QWidget* gapWidget = createGapWidget(matrix, Qt::Horizontal, gap);
        _imp->gridWidgets.push_back(gapWidget);
        grid->addWidget(gapWidget, firstBodyRow, gridColumn++);
    }
    const int channelColumn = gridColumn++;
    const int outColumn = gridColumn++;

    // Rows: the header, the out1 block, a gap row, then the out2 block. A None output
    // keeps one row for its dropdown.
    std::vector<int> gridRows(layout.rows.size(), -1);
    int gridRow = firstBodyRow;
    for (std::size_t i = 0; i < layout.outputGroups.size() && i < 2; ++i) {
        const KnobGuiShuffleMapPrivate::Group& group = layout.outputGroups[i];
        if (i > 0) {
            QWidget* gapWidget = createGapWidget(matrix, Qt::Vertical, gap);
            _imp->gridWidgets.push_back(gapWidget);
            grid->addWidget(gapWidget, gridRow++, channelColumn);
        }
        const int blockFirst = gridRow;
        for (int r = 0; r < group.count; ++r) {
            gridRows[group.first + r] = blockFirst + r;
        }
        const int blockHeight = std::max(group.count, 1);
        LayerChannelRow* outRow = _imp->layerRows[group.slot == 1 ? eLayerRowOut1 : eLayerRowOut2];
        if (outRow) {
            grid->addWidget(outRow, blockFirst, outColumn, Qt::AlignLeft | Qt::AlignVCenter);
        }
        gridRow = blockFirst + blockHeight;
    }

    const QString arrow = QString::fromUtf8("\xe2\x86\x92 ");
    for (std::size_t r = 0; r < layout.rows.size(); ++r) {
        const KnobGuiShuffleMapPrivate::Row& row = layout.rows[r];
        if (gridRows[r] < 0) {
            continue;
        }
        Label* channelLabel = new Label(arrow + qs(row.channel), matrix);
        channelLabel->setToolTip(row.toolTip);
        channelLabel->setMaximumHeight(cellHeight);
        _imp->gridWidgets.push_back(channelLabel);
        grid->addWidget(channelLabel, gridRows[r], channelColumn, Qt::AlignLeft | Qt::AlignVCenter);

        QButtonGroup* buttonGroup = new QButtonGroup(matrix);
        buttonGroup->setExclusive(true);
        _imp->groups.push_back(buttonGroup);
        _imp->cells.push_back(std::vector<Button*>());

        for (std::size_t c = 0; c < layout.columns.size(); ++c) {
            const KnobGuiShuffleMapPrivate::Column& column = layout.columns[c];
            Button* b = new Button(column.text, matrix);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::StrongFocus);
            b->setFixedSize(cellWidth, cellHeight);
            b->setToolTip(row.toolTip + QString::fromUtf8(" \xe2\x86\x90 ") + column.toolTip);
            b->setEnabled(column.enabled);
            QColor color;
            if (getChannelColorFromName(column.channel, &color)) {
                b->setStyleSheet(QString::fromUtf8("QPushButton:checked { background-color: %1; }").arg(color.name()));
            }
            buttonGroup->addButton(b, (int)c);
            _imp->gridWidgets.push_back(b);
            grid->addWidget(b, gridRows[r], gridColumns[c]);
            const int rowIndex = (int)r;
            const int columnIndex = (int)c;
            QObject::connect(b, &QPushButton::clicked, this, [this, rowIndex, columnIndex]() {
                onCellClicked(rowIndex, columnIndex);
            });
            _imp->cells.back().push_back(b);
        }
    }
} // KnobGuiShuffleMap::rebuildMatrix

void
KnobGuiShuffleMap::syncCheckedButtons()
{
    std::shared_ptr<KnobShuffleMap> mapping = _imp->knob.lock();
    Shuffle* shuffle = mapping ? dynamic_cast<Shuffle*>(mapping->getHolder()) : 0;

    if (!mapping || !shuffle) {
        return;
    }
    const KnobGuiShuffleMapPrivate::Layout& layout = _imp->layout;
    for (std::size_t r = 0; r < layout.rows.size() && r < _imp->cells.size(); ++r) {
        const ShuffleSource src = shuffle->getEffectiveSource(layout.rows[r].outSlot, layout.rows[r].outIndex);
        const int column = findSourceColumn(src);
        if (column >= 0) {
            if (!_imp->cells[r][column]->isChecked()) {
                _imp->cells[r][column]->setChecked(true);
            }
            continue;
        }
        // A source the matrix cannot show, such as a slot now set to None, leaves the row
        // with nothing checked, which an exclusive group only allows while not exclusive.
        _imp->groups[r]->setExclusive(false);
        for (std::size_t c = 0; c < _imp->cells[r].size(); ++c) {
            _imp->cells[r][c]->setChecked(false);
        }
        _imp->groups[r]->setExclusive(true);
    }
}

void
KnobGuiShuffleMap::onCellClicked(int row,
                                 int column)
{
    std::shared_ptr<KnobShuffleMap> mapping = _imp->knob.lock();
    const KnobGuiShuffleMapPrivate::Layout& layout = _imp->layout;

    if (!mapping || row < 0 || row >= (int)layout.rows.size() || column < 0 || column >= (int)layout.columns.size()) {
        return;
    }
    const int outSlot = layout.rows[row].outSlot;
    const int outIndex = layout.rows[row].outIndex;
    const ShuffleSource src = layout.columns[column].src;
    const ShuffleSource defaultSrc = KnobShuffleMap::defaultSource(outSlot, outIndex);

    std::vector<ShuffleMapRow> rows = mapping->getRows();
    bool found = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].outSlot != outSlot || rows[i].outIndex != outIndex) {
            continue;
        }
        if (src == defaultSrc) {
            rows.erase(rows.begin() + i);
        } else {
            rows[i].src = src;
        }
        found = true;
        break;
    }
    if (!found && src != defaultSrc) {
        ShuffleMapRow added;
        added.outSlot = outSlot;
        added.outIndex = outIndex;
        added.src = src;
        rows.push_back(added);
    }
    pushValue(mapping->encodeRows(rows));
    // The exclusive group has already moved the check; bring it back in line with the
    // knob in case the value did not change.
    scheduleRefresh(false);
}

void
KnobGuiShuffleMap::onResetClicked()
{
    std::shared_ptr<KnobShuffleMap> mapping = _imp->knob.lock();

    if (mapping) {
        pushValue(mapping->getDefaultValue(0));
    }
}

void
KnobGuiShuffleMap::onLayerRowChosen(LayerRowEnum which,
                                    const QString& layerID)
{
    KnobLayerSelectPtr knob = findLayerKnob(getNode(), which);

    if (knob) {
        pushLayerValue(which, knob->encode(layerID.toStdString(), std::vector<std::string>()));
    }
}

void
KnobGuiShuffleMap::onLayerRowNewLayerRequested(LayerRowEnum which)
{
    // The row is still inside its combo's own change signal; the registry add below
    // repopulates that combo, so the dialog waits for the event loop.
    QTimer::singleShot(0, this, [this, which]() {
        KnobLayerSelectPtr knob = findLayerKnob(getNode(), which);
        if (!knob) {
            return;
        }
        runNewLayerDialog(knob, getGui(), [this, knob, which](const std::string& layerID, const std::string& /*layerLabel*/) {
            pushLayerValue(which, knob->encode(layerID, std::vector<std::string>()));
        });
    });
}

void
KnobGuiShuffleMap::pushLayerValue(LayerRowEnum which,
                                  const std::string& newValue)
{
    KnobLayerSelectPtr knob = findLayerKnob(getNode(), which);

    if (!knob) {
        return;
    }
    const std::string oldValue = knob->getValue();
    if (oldValue == newValue) {
        scheduleRefresh(false);

        return;
    }
    KnobGuiContainerI* container = KnobGui::getContainer();
    KnobGuiPtr knobGui = container ? container->getKnobGui(knob) : KnobGuiPtr();
    if (!knobGui) {
        knobGui = std::dynamic_pointer_cast<KnobGui>(knob->getKnobGuiPointer());
    }

    ++_imp->layerEditDepth;
    if (knobGui) {
        KnobUndoCommand<std::string>* cmd = new KnobUndoCommand<std::string>(knobGui, oldValue, newValue);
        cmd->setMergeable(false);
        knobGui->pushUndoCommand(cmd);
    } else {
        knob->setValue(newValue);
    }
    --_imp->layerEditDepth;
    scheduleRefresh(true);
}

int
KnobGuiShuffleMap::getOutputRowCount() const
{
    return (int)_imp->cells.size();
}

int
KnobGuiShuffleMap::getSourceColumnCount() const
{
    return _imp->cells.empty() ? 0 : (int)_imp->cells.front().size();
}

bool
KnobGuiShuffleMap::getOutputRow(int row,
                                int* outSlot,
                                int* outIndex) const
{
    if (row < 0 || row >= (int)_imp->layout.rows.size()) {
        return false;
    }
    *outSlot = _imp->layout.rows[row].outSlot;
    *outIndex = _imp->layout.rows[row].outIndex;

    return true;
}

int
KnobGuiShuffleMap::findSourceColumn(const ShuffleSource& src) const
{
    for (std::size_t c = 0; c < _imp->layout.columns.size(); ++c) {
        if (_imp->layout.columns[c].src == src) {
            return (int)c;
        }
    }

    return -1;
}

Button*
KnobGuiShuffleMap::getCellButton(int row,
                                 int column) const
{
    if (row < 0 || row >= (int)_imp->cells.size() || column < 0 || column >= (int)_imp->cells[row].size()) {
        return 0;
    }

    return _imp->cells[row][column];
}

LayerChannelRow*
KnobGuiShuffleMap::getLayerRow(LayerRowEnum which) const
{
    const int index = (int)which;

    if (index < 0 || index >= 4) {
        return 0;
    }

    return _imp->layerRows[index];
}

QWidget*
KnobGuiShuffleMap::getMatrixWidget() const
{
    return _imp->matrix;
}

Button*
KnobGuiShuffleMap::getResetButton() const
{
    return _imp->resetButton;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiShuffleMap.cpp"
