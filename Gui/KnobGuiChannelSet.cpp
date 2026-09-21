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

#include "KnobGuiChannelSet.h"

#include <algorithm>
#include <list>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/Button.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/LayerChannelRow.h"

NATRON_NAMESPACE_ENTER

namespace {
LayerChannelRow::SetRowModeEnum
toRowMode(ChannelSetRow::ModeEnum mode)
{
    switch (mode) {
    case ChannelSetRow::eModeNone:

        return LayerChannelRow::eSetRowModeNone;
    case ChannelSetRow::eModeAll:

        return LayerChannelRow::eSetRowModeAll;
    case ChannelSetRow::eModeRegex:

        return LayerChannelRow::eSetRowModeRegex;
    case ChannelSetRow::eModeLayer:
        break;
    }

    return LayerChannelRow::eSetRowModeLayer;
}

const LayerChannelRow::LayerEntry*
findEntry(const std::vector<LayerChannelRow::LayerEntry>& layers,
          const std::string& id)
{
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].id == id) {
            return &layers[i];
        }
    }

    return 0;
}

bool
sameLayers(const std::vector<LayerChannelRow::LayerEntry>& a,
           const std::vector<LayerChannelRow::LayerEntry>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].label != b[i].label || a[i].channels != b[i].channels) {
            return false;
        }
    }

    return true;
}
} // namespace

struct KnobGuiChannelSetPrivate {
    KnobChannelSetWPtr knob;
    QWidget* container;
    QVBoxLayout* layout;
    std::vector<LayerChannelRow*> rows;
    Button* addLayerButton;
    std::vector<LayerChannelRow::LayerEntry> layers;
    int userEditDepth;
    bool refreshPending;
    bool relistPending;

    KnobGuiChannelSetPrivate()
        : knob()
        , container(0)
        , layout(0)
        , rows()
        , addLayerButton(0)
        , layers()
        , userEditDepth(0)
        , refreshPending(false)
        , relistPending(false)
    {
    }

    NodePtr getNode() const
    {
        KnobChannelSetPtr k = knob.lock();
        EffectInstance* effect = k ? dynamic_cast<EffectInstance*>(k->getHolder()) : 0;

        return effect ? effect->getNode() : NodePtr();
    }

    int indexOf(const LayerChannelRow* row) const
    {
        std::vector<LayerChannelRow*>::const_iterator found = std::find(rows.begin(), rows.end(), row);

        return found == rows.end() ? -1 : (int)(found - rows.begin());
    }

    void listLayers()
    {
        layers.clear();
        KnobChannelSetPtr k = knob.lock();
        NodePtr node = getNode();
        if (!k || !node) {
            return;
        }
        std::list<ImageLayerDesc> descs;
        node->listLayersForKnob(k, &descs);
        descs.sort([](const ImageLayerDesc& a, const ImageLayerDesc& b) {
            return a.isColorLayer() && !b.isColorLayer();
        });
        for (std::list<ImageLayerDesc>::const_iterator it = descs.begin(); it != descs.end(); ++it) {
            LayerChannelRow::LayerEntry entry;
            entry.id = it->getLayerID();
            entry.label = it->getLayerLabel();
            entry.channels = it->getChannels();
            layers.push_back(entry);
        }
    }
};

KnobGuiChannelSet::KnobGuiChannelSet(KnobIPtr knob,
                                     KnobGuiContainerI* container)
    : KnobGui(knob, container)
    , _imp(new KnobGuiChannelSetPrivate())
{
    _imp->knob = std::dynamic_pointer_cast<KnobChannelSet>(knob);

    NodePtr node = _imp->getNode();
    if (node) {
        QObject::connect(node.get(), SIGNAL(layerListRefreshed()), this, SLOT(onLayerListRefreshed()));
        AppInstancePtr app = node->getApp();
        ProjectPtr project = app ? app->getProject() : ProjectPtr();
        if (project) {
            QObject::connect(project.get(), SIGNAL(projectLayersChanged()), this, SLOT(onLayerListRefreshed()));
        }
    }
}

KnobGuiChannelSet::~KnobGuiChannelSet()
{
}

KnobIPtr
KnobGuiChannelSet::getKnob() const
{
    return _imp->knob.lock();
}

void
KnobGuiChannelSet::removeSpecificGui()
{
    if (_imp->container) {
        _imp->container->deleteLater();
    }
    _imp->container = 0;
    _imp->layout = 0;
    _imp->rows.clear();
    _imp->addLayerButton = 0;
}

int
KnobGuiChannelSet::getRowCount() const
{
    return (int)_imp->rows.size();
}

LayerChannelRow*
KnobGuiChannelSet::getRow(int index) const
{
    if (index < 0 || index >= (int)_imp->rows.size()) {
        return 0;
    }

    return _imp->rows[index];
}

Button*
KnobGuiChannelSet::getAddLayerButton() const
{
    return _imp->addLayerButton;
}

void
KnobGuiChannelSet::createWidget(QHBoxLayout* layout)
{
    _imp->container = new QWidget(layout->parentWidget());
    _imp->layout = new QVBoxLayout(_imp->container);
    _imp->layout->setContentsMargins(0, 0, 0, 0);
    _imp->layout->setSpacing(2);

    layout->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    QWidget* addContainer = new QWidget(_imp->container);
    QHBoxLayout* addLayout = new QHBoxLayout(addContainer);
    addLayout->setContentsMargins(0, 0, 0, 0);
    _imp->addLayerButton = new Button(tr("+ Add layer"), addContainer);
    _imp->addLayerButton->setToolTip(tr("Append a layer row to the set"));
    _imp->addLayerButton->setFocusPolicy(Qt::StrongFocus);
    QObject::connect(_imp->addLayerButton, SIGNAL(clicked()), this, SLOT(onAddLayerClicked()));
    addLayout->addWidget(_imp->addLayerButton);
    addLayout->addStretch();
    _imp->layout->addWidget(addContainer);

    enableRightClickMenu(_imp->container, 0);

    refresh(true);

    layout->addWidget(_imp->container);
}

void
KnobGuiChannelSet::refresh(bool relistLayers)
{
    KnobChannelSetPtr knob = _imp->knob.lock();

    if (!knob || !_imp->container) {
        return;
    }
    if (relistLayers) {
        _imp->listLayers();
    }

    const std::vector<ChannelSetRow> rows = knob->getRows();

    while (_imp->rows.size() > rows.size()) {
        LayerChannelRow* row = _imp->rows.back();
        _imp->rows.pop_back();
        row->hide();
        row->disconnect(this);
        _imp->layout->removeWidget(row);
        row->deleteLater();
    }
    const std::size_t firstNewRow = _imp->rows.size();
    while (_imp->rows.size() < rows.size()) {
        const bool first = _imp->rows.empty();
        LayerChannelRow* row = new LayerChannelRow(first ? LayerChannelRow::eModeSetRow0 : LayerChannelRow::eModeSetRowN, _imp->container);
        row->setRowRemovable(!first);
        _imp->layout->insertWidget((int)_imp->rows.size(), row);
        QObject::connect(row, &LayerChannelRow::modeChosen, this, [this, row](LayerChannelRow::SetRowModeEnum mode) {
            onRowModeChosen(row, mode);
        });
        QObject::connect(row, &LayerChannelRow::layerChosen, this, [this, row](const QString& id) {
            onRowLayerChosen(row, id);
        });
        QObject::connect(row, &LayerChannelRow::channelToggled, this, [this, row](const QString&, bool) {
            onRowChannelToggled(row);
        });
        QObject::connect(row, &LayerChannelRow::patternCommitted, this, [this, row](const QString& pattern) {
            onRowPatternCommitted(row, pattern);
        });
        QObject::connect(row, &LayerChannelRow::removeRequested, this, [this, row]() {
            onRowRemoveRequested(row);
        });
        _imp->rows.push_back(row);
    }

    const bool restEnabled = rows.empty() || (rows[0].mode != ChannelSetRow::eModeNone && rows[0].mode != ChannelSetRow::eModeAll);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        LayerChannelRow* row = _imp->rows[i];
        const ChannelSetRow& value = rows[i];
        if (!sameLayers(row->getAvailableLayers(), _imp->layers)) {
            row->setAvailableLayers(_imp->layers, false);
        }

        const LayerChannelRow::SetRowModeEnum mode = toRowMode(value.mode);
        std::vector<std::string> enabled = value.channels;
        if (value.mode == ChannelSetRow::eModeLayer && enabled.empty()) {
            const LayerChannelRow::LayerEntry* entry = findEntry(_imp->layers, value.layerOrPattern);
            if (entry) {
                enabled = entry->channels;
            }
        }
        bool current = row->getSetRowMode() == mode;
        if (current && mode == LayerChannelRow::eSetRowModeLayer) {
            current = row->getCurrentLayerID() == value.layerOrPattern && row->getEnabledChannels() == enabled;
        } else if (current && mode == LayerChannelRow::eSetRowModeRegex) {
            current = row->getCommittedPattern() == value.layerOrPattern;
        }
        if (!current) {
            row->setSetRowValue(mode, value.layerOrPattern, enabled);
        }
        if (i >= firstNewRow) {
            row->setAbsentMarker(tr("(not in input)"));
        }
        row->setEnabled(i == 0 || restEnabled);
    }

    QWidget* previous = _imp->rows.empty() ? 0 : _imp->rows.back();
    if (previous) {
        QWidget::setTabOrder(previous, _imp->addLayerButton);
    }
} // KnobGuiChannelSet::refresh

void
KnobGuiChannelSet::scheduleRefresh(bool relistLayers)
{
    _imp->relistPending = _imp->relistPending || relistLayers;
    if (_imp->refreshPending) {
        return;
    }
    _imp->refreshPending = true;
    QTimer::singleShot(0, this, SLOT(onDeferredRefresh()));
}

void
KnobGuiChannelSet::onDeferredRefresh()
{
    const bool relist = _imp->relistPending;

    _imp->refreshPending = false;
    _imp->relistPending = false;
    refresh(relist);
}

void
KnobGuiChannelSet::updateGUI(int /*dimension*/)
{
    // A row is still inside its own signal emission while the value it asked for is
    // applied; rebuilding it there would delete the emitting widget.
    if (_imp->userEditDepth > 0) {
        scheduleRefresh(false);

        return;
    }
    refresh(false);
}

void
KnobGuiChannelSet::onLayerListRefreshed()
{
    if (_imp->userEditDepth > 0) {
        scheduleRefresh(true);

        return;
    }
    refresh(true);
}

void
KnobGuiChannelSet::pushRows(const std::vector<ChannelSetRow>& newRows)
{
    KnobChannelSetPtr knob = _imp->knob.lock();

    if (!knob) {
        return;
    }
    const std::string oldValue = knob->getValue();
    const std::string newValue = knob->encodeRows(newRows);
    if (oldValue == newValue) {
        return;
    }
    KnobUndoCommand<std::string>* cmd = new KnobUndoCommand<std::string>(shared_from_this(), oldValue, newValue);
    cmd->setMergeable(false);
    ++_imp->userEditDepth;
    pushUndoCommand(cmd);
    --_imp->userEditDepth;
}

void
KnobGuiChannelSet::onRowModeChosen(LayerChannelRow* row,
                                   int setRowMode)
{
    KnobChannelSetPtr knob = _imp->knob.lock();
    const int index = _imp->indexOf(row);

    if (!knob || index < 0) {
        return;
    }
    std::vector<ChannelSetRow> rows = knob->getRows();
    if (index >= (int)rows.size()) {
        return;
    }
    ChannelSetRow value;
    switch ((LayerChannelRow::SetRowModeEnum)setRowMode) {
    case LayerChannelRow::eSetRowModeNone:
        value.mode = ChannelSetRow::eModeNone;
        break;
    case LayerChannelRow::eSetRowModeAll:
        value.mode = ChannelSetRow::eModeAll;
        break;
    case LayerChannelRow::eSetRowModeRegex:
        value.mode = ChannelSetRow::eModeRegex;
        break;
    case LayerChannelRow::eSetRowModeLayer:

        return;
    }
    if (index > 0 && value.mode != ChannelSetRow::eModeRegex) {
        return;
    }
    rows[index] = value;
    pushRows(rows);
}

void
KnobGuiChannelSet::onRowLayerChosen(LayerChannelRow* row,
                                    const QString& layerID)
{
    KnobChannelSetPtr knob = _imp->knob.lock();
    const int index = _imp->indexOf(row);

    if (!knob || index < 0) {
        return;
    }
    std::vector<ChannelSetRow> rows = knob->getRows();
    if (index >= (int)rows.size()) {
        return;
    }
    ChannelSetRow value;
    value.mode = ChannelSetRow::eModeLayer;
    value.layerOrPattern = layerID.toStdString();
    rows[index] = value;
    pushRows(rows);
}

void
KnobGuiChannelSet::onRowChannelToggled(LayerChannelRow* row)
{
    KnobChannelSetPtr knob = _imp->knob.lock();
    const int index = _imp->indexOf(row);

    if (!knob || index < 0) {
        return;
    }
    std::vector<ChannelSetRow> rows = knob->getRows();
    if (index >= (int)rows.size() || rows[index].mode != ChannelSetRow::eModeLayer) {
        return;
    }
    const std::vector<std::string> enabled = row->getEnabledChannels();
    // An empty channel list is the knob's spelling of "every channel", so a row cannot
    // hold zero channels: the last button springs back and the row is removed instead.
    if (enabled.empty()) {
        scheduleRefresh(false);

        return;
    }
    rows[index].channels = enabled;
    pushRows(rows);
}

void
KnobGuiChannelSet::onRowPatternCommitted(LayerChannelRow* row,
                                         const QString& pattern)
{
    KnobChannelSetPtr knob = _imp->knob.lock();
    const int index = _imp->indexOf(row);

    if (!knob || index < 0) {
        return;
    }
    std::vector<ChannelSetRow> rows = knob->getRows();
    if (index >= (int)rows.size()) {
        return;
    }
    ChannelSetRow value;
    value.mode = ChannelSetRow::eModeRegex;
    value.layerOrPattern = pattern.toStdString();
    rows[index] = value;
    pushRows(rows);
}

void
KnobGuiChannelSet::onRowRemoveRequested(LayerChannelRow* row)
{
    KnobChannelSetPtr knob = _imp->knob.lock();
    const int index = _imp->indexOf(row);

    if (!knob || index < 1) {
        return;
    }
    std::vector<ChannelSetRow> rows = knob->getRows();
    if (index >= (int)rows.size()) {
        return;
    }
    rows.erase(rows.begin() + index);
    pushRows(rows);
}

void
KnobGuiChannelSet::onAddLayerClicked()
{
    KnobChannelSetPtr knob = _imp->knob.lock();

    if (!knob) {
        return;
    }
    std::vector<ChannelSetRow> rows = knob->getRows();
    ChannelSetRow value;
    value.mode = ChannelSetRow::eModeLayer;
    value.layerOrPattern = kNatronColorLayerID;
    for (std::size_t i = 0; i < _imp->layers.size(); ++i) {
        if (!ImageLayerDesc::isColorLayer(_imp->layers[i].id)) {
            value.layerOrPattern = _imp->layers[i].id;
            break;
        }
    }
    rows.push_back(value);
    pushRows(rows);
}

void
KnobGuiChannelSet::_hide()
{
    if (_imp->container) {
        _imp->container->hide();
    }
}

void
KnobGuiChannelSet::_show()
{
    if (_imp->container) {
        _imp->container->show();
    }
}

void
KnobGuiChannelSet::setEnabled()
{
    if (_imp->container) {
        _imp->container->setEnabled(getKnob()->isEnabled(0));
    }
}

void
KnobGuiChannelSet::setReadOnly(bool readOnly,
                               int /*dimension*/)
{
    if (_imp->container) {
        _imp->container->setEnabled(!readOnly);
    }
}

void
KnobGuiChannelSet::setDirty(bool /*dirty*/)
{
}

void
KnobGuiChannelSet::reflectAnimationLevel(int /*dimension*/,
                                         AnimationLevelEnum /*level*/)
{
}

void
KnobGuiChannelSet::reflectExpressionState(int /*dimension*/,
                                          bool /*hasExpr*/)
{
}

void
KnobGuiChannelSet::updateToolTip()
{
    if (_imp->container && hasToolTip()) {
        _imp->container->setToolTip(toolTip());
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiChannelSet.cpp"
