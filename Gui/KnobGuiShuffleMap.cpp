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
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"

#include "Gui/Button.h"
#include "Gui/ChannelColor.h"
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

    /// A header spanning count columns (slot groups) or rows (output groups) from first.
    struct Group {
        int slot;
        QString text;
        int first;
        int count;

        Group()
            : slot(0)
            , text()
            , first(0)
            , count(0)
        {
        }

        bool operator==(const Group& other) const
        {
            return slot == other.slot && text == other.text && first == other.first && count == other.count;
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
    Button* resetButton;
    Layout layout;
    std::vector<std::vector<Button*>> cells;
    std::vector<QButtonGroup*> groups;

    KnobGuiShuffleMapPrivate()
        : knob()
        , matrix(0)
        , resetButton(0)
        , layout()
        , cells()
        , groups()
    {
    }
};

namespace {
typedef KnobGuiShuffleMapPrivate::Layout MatrixLayout;

QString
qs(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
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
                  const QString& absentMarker,
                  MatrixLayout* layout)
{
    const std::string layerID = shuffle->getSlotLayer(slot);

    if (layerID.empty()) {
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

    KnobGuiShuffleMapPrivate::Group group;
    group.slot = slot;
    group.first = (int)layout->columns.size();
    group.count = (int)channels.size();
    group.text = QString::fromUtf8("in%1 %2").arg(slot).arg(label);
    if (!listed) {
        group.text += QLatin1Char(' ');
        group.text += absentMarker;
    }
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
                 const QString& absentMarker,
                 MatrixLayout* layout)
{
    const std::string layerID = shuffle->getOutputLayer(slot);

    if (layerID.empty()) {
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

    KnobGuiShuffleMapPrivate::Group group;
    group.slot = slot;
    group.first = (int)layout->rows.size();
    group.count = (int)channels.size();
    group.text = label;
    if (!listed) {
        group.text += QLatin1Char(' ');
        group.text += absentMarker;
    }
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
        static const char* const siblings[] = {
            kShuffleParamIn1, kShuffleParamIn2, kShuffleParamOut1, kShuffleParamOut2
        };
        for (std::size_t i = 0; i < sizeof(siblings) / sizeof(siblings[0]); ++i) {
            KnobIPtr sibling = node->getKnobByName(siblings[i]);
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
    _imp->resetButton = 0;
    _imp->cells.clear();
    _imp->groups.clear();
    _imp->layout = KnobGuiShuffleMapPrivate::Layout();
}

void
KnobGuiShuffleMap::createWidget(QHBoxLayout* layout)
{
    createContainer(layout);

    _imp->resetButton = new Button(tr("Reset"), getContainer());
    _imp->resetButton->setToolTip(tr("Set every output channel back to its default source."));
    QObject::connect(_imp->resetButton, &QPushButton::clicked, this, [this]() {
        onResetClicked();
    });
    getContainerLayout()->addWidget(_imp->resetButton, 0, Qt::AlignLeft);

    refresh(true);
}

void
KnobGuiShuffleMap::refreshWidgets()
{
    std::shared_ptr<KnobShuffleMap> mapping = _imp->knob.lock();
    Shuffle* shuffle = mapping ? dynamic_cast<Shuffle*>(mapping->getHolder()) : 0;
    NodePtr node = getNode();

    KnobGuiShuffleMapPrivate::Layout layout;
    if (shuffle && node) {
        const std::vector<ShuffleMapRow> mapRows = mapping->getRows();
        appendSlotColumns(node, shuffle, mapRows, 1, getAbsentMarkerText(), &layout);
        appendSlotColumns(node, shuffle, mapRows, 2, getAbsentMarkerText(), &layout);
        appendConstantColumn(ShuffleSource::makeZero(), QString::fromUtf8("0"), tr("A constant 0."), &layout);
        appendConstantColumn(ShuffleSource::makeOne(), QString::fromUtf8("1"), tr("A constant 1."), &layout);
        appendOutputRows(node, shuffle, mapRows, 1, tr("(not in project)"), &layout);
        appendOutputRows(node, shuffle, mapRows, 2, tr("(not in project)"), &layout);
    }

    if (!_imp->matrix || !(layout == _imp->layout)) {
        _imp->layout = layout;
        rebuildMatrix();
    }
    syncCheckedButtons();
}

void
KnobGuiShuffleMap::rebuildMatrix()
{
    // Never reached from inside a cell's own click: the base class defers the refresh
    // while a user edit is being applied.
    delete _imp->matrix;
    _imp->matrix = 0;
    _imp->cells.clear();
    _imp->groups.clear();

    QWidget* container = getContainer();
    if (!container) {
        return;
    }

    const KnobGuiShuffleMapPrivate::Layout& layout = _imp->layout;
    const int firstCellRow = 1;
    const int firstCellColumn = 2;

    _imp->matrix = new QWidget(container);
    QGridLayout* grid = new QGridLayout(_imp->matrix);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(2);

    for (std::size_t i = 0; i < layout.slotGroups.size(); ++i) {
        const KnobGuiShuffleMapPrivate::Group& group = layout.slotGroups[i];
        if (group.count <= 0) {
            continue;
        }
        Label* header = new Label(group.text, _imp->matrix);
        grid->addWidget(header, 0, firstCellColumn + group.first, 1, group.count, Qt::AlignHCenter);
    }
    for (std::size_t i = 0; i < layout.outputGroups.size(); ++i) {
        const KnobGuiShuffleMapPrivate::Group& group = layout.outputGroups[i];
        if (group.count <= 0) {
            continue;
        }
        Label* header = new Label(group.text, _imp->matrix);
        grid->addWidget(header, firstCellRow + group.first, 0, Qt::AlignLeft);
    }

    const QString arrow = QString::fromUtf8(" \xe2\x86\x90");
    for (std::size_t r = 0; r < layout.rows.size(); ++r) {
        const KnobGuiShuffleMapPrivate::Row& row = layout.rows[r];
        Label* channelLabel = new Label(qs(row.channel) + arrow, _imp->matrix);
        channelLabel->setToolTip(row.toolTip);
        grid->addWidget(channelLabel, firstCellRow + (int)r, 1, Qt::AlignRight);

        QButtonGroup* buttonGroup = new QButtonGroup(_imp->matrix);
        buttonGroup->setExclusive(true);
        _imp->groups.push_back(buttonGroup);
        _imp->cells.push_back(std::vector<Button*>());

        for (std::size_t c = 0; c < layout.columns.size(); ++c) {
            const KnobGuiShuffleMapPrivate::Column& column = layout.columns[c];
            Button* b = new Button(column.text, _imp->matrix);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::StrongFocus);
            b->setToolTip(row.toolTip + QString::fromUtf8(" \xe2\x86\x90 ") + column.toolTip);
            b->setEnabled(column.enabled);
            QColor color;
            if (getChannelColorFromName(column.channel, &color)) {
                b->setStyleSheet(QString::fromUtf8("QPushButton:checked { background-color: %1; }").arg(color.name()));
            }
            buttonGroup->addButton(b, (int)c);
            grid->addWidget(b, firstCellRow + (int)r, firstCellColumn + (int)c);
            const int rowIndex = (int)r;
            const int columnIndex = (int)c;
            QObject::connect(b, &QPushButton::clicked, this, [this, rowIndex, columnIndex]() {
                onCellClicked(rowIndex, columnIndex);
            });
            _imp->cells.back().push_back(b);
        }
    }

    getContainerLayout()->insertWidget(0, _imp->matrix);
}

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

QString
KnobGuiShuffleMap::getSlotHeaderText(int slot) const
{
    for (std::size_t i = 0; i < _imp->layout.slotGroups.size(); ++i) {
        if (_imp->layout.slotGroups[i].slot == slot) {
            return _imp->layout.slotGroups[i].text;
        }
    }

    return QString();
}

Button*
KnobGuiShuffleMap::getResetButton() const
{
    return _imp->resetButton;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiShuffleMap.cpp"
